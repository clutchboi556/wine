/*
 * Wayland window handling
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include "wine/list.h"

#include "ntstatus.h"

#include "waylanddrv.h"
#include "wine/server.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);
WINE_DECLARE_DEBUG_CHANNEL(handoff);


static int wayland_win_data_cmp_rb(const void *key,
                                   const struct rb_entry *entry)
{
    HWND key_hwnd = (HWND)key; /* cast to work around const */
    const struct wayland_win_data *entry_win_data =
        RB_ENTRY_VALUE(entry, const struct wayland_win_data, entry);

    if (key_hwnd < entry_win_data->hwnd) return -1;
    if (key_hwnd > entry_win_data->hwnd) return 1;
    return 0;
}

static pthread_mutex_t win_data_mutex;
static struct rb_tree win_data_rb = { wayland_win_data_cmp_rb };

static void pending_present_remove(HWND hwnd);

/***********************************************************************
 *           wayland_win_data_create
 *
 * Create a data window structure for an existing window.
 */
static struct wayland_win_data *wayland_win_data_create(HWND hwnd, const struct window_rects *rects)
{
    struct wayland_win_data *data;
    struct rb_entry *rb_entry;
    HWND parent;

    /* Don't create win data for desktop or HWND_MESSAGE windows. */
    if (!(parent = NtUserGetAncestor(hwnd, GA_PARENT))) return NULL;
    if (parent != NtUserGetDesktopWindow() && !NtUserGetAncestor(parent, GA_PARENT))
        return NULL;

    if (!(data = calloc(1, sizeof(*data)))) return NULL;

    data->hwnd = hwnd;
    data->rects = *rects;

    pthread_mutex_lock(&win_data_mutex);

    /* Check that another thread hasn't already created the wayland_win_data. */
    if ((rb_entry = rb_get(&win_data_rb, hwnd)))
    {
        free(data);
        return RB_ENTRY_VALUE(rb_entry, struct wayland_win_data, entry);
    }

    rb_put(&win_data_rb, hwnd, &data->entry);

    TRACE("hwnd=%p\n", data->hwnd);

    return data;
}

/***********************************************************************
 *           wayland_win_data_destroy
 */
static void wayland_win_data_destroy(struct wayland_win_data *data)
{
    TRACE("hwnd=%p\n", data->hwnd);

    rb_remove(&win_data_rb, &data->entry);

    pthread_mutex_unlock(&win_data_mutex);

    if (data->wayland_surface) wayland_surface_destroy(data->wayland_surface);
    if (data->window_contents) wayland_shm_buffer_unref(data->window_contents);
    if (data->pending_contents) wayland_shm_buffer_unref(data->pending_contents);
    pending_present_remove(data->hwnd);
    if (data->pending_damage) NtGdiDeleteObjectApp(data->pending_damage);
    free(data);
}

/***********************************************************************
 *           wayland_win_data_get
 *
 * Lock and return the data structure associated with a window.
 */
struct wayland_win_data *wayland_win_data_get(HWND hwnd)
{
    struct rb_entry *entry;

    pthread_mutex_lock(&win_data_mutex);
    if ((entry = rb_get(&win_data_rb, hwnd)))
        return RB_ENTRY_VALUE(entry, struct wayland_win_data, entry);
    pthread_mutex_unlock(&win_data_mutex);

    return NULL;
}

/***********************************************************************
 *           wayland_win_data_release
 *
 * Release the data returned by wayland_win_data_get.
 */
void wayland_win_data_release(struct wayland_win_data *data)
{
    assert(data);
    pthread_mutex_unlock(&win_data_mutex);
}

static void wayland_win_data_get_config(struct wayland_win_data *data,
                                        struct wayland_window_config *conf)
{
    enum wayland_surface_config_state window_state = 0;
    DWORD style;

    conf->rect = data->rects.visible;
    style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);

    TRACE("window=%s style=%#x\n", wine_dbgstr_rect(&conf->rect), style);

    conf->minimized = !!(style & WS_MINIMIZE);

    /* The fullscreen state is implied by the window position and style. */
    if (data->is_fullscreen)
    {
        if ((style & WS_MAXIMIZE) && (style & WS_CAPTION) == WS_CAPTION)
            window_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
        else if (!(style & WS_MINIMIZE))
            window_state |= WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN;
    }
    else if (style & WS_MAXIMIZE)
    {
        window_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
    }

    conf->resizeable = data->resizeable;
    conf->state = window_state;
    conf->visible = (style & WS_VISIBLE) == WS_VISIBLE;
    conf->managed = data->managed;
}

static void reapply_cursor_clipping(void)
{
    RECT rect;
    UINT context = NtUserSetThreadDpiAwarenessContext(NTUSER_DPI_PER_MONITOR_AWARE);
    if (NtUserGetClipCursor(&rect)) NtUserClipCursor(&rect);
    NtUserSetThreadDpiAwarenessContext(context);
}

static BOOL wayland_win_data_create_wayland_surface(struct wayland_win_data *data, struct wayland_surface *owner_surface)
{
    struct wayland_surface *surface;
    enum wayland_surface_role role;
    BOOL visible;
    DWORD exstyle = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);
    struct wl_region *input_region;

    TRACE("hwnd=%p\n", data->hwnd);

    visible = ((NtUserGetWindowLongW(data->hwnd, GWL_STYLE) & WS_VISIBLE) == WS_VISIBLE) &&
               (!(exstyle & WS_EX_LAYERED) || data->layered_attribs_set);

    /* A managed window gets its own toplevel, and its owner (if any) becomes
     * the toplevel's parent; an unmanaged one is drawn as part of its owner. */
    if (!visible) role = WAYLAND_SURFACE_ROLE_NONE;
    else if (owner_surface && !data->managed) role = WAYLAND_SURFACE_ROLE_SUBSURFACE;
    else role = WAYLAND_SURFACE_ROLE_TOPLEVEL;

    /* we can temporarily clear the role of a surface but cannot assign a different one after it's set */
    if ((surface = data->wayland_surface) && role && surface->role && surface->role != role)
    {
        /* Make sure any attached client surface is detached before we destroy the surface.
         * They will be reattached when win32u updates them again after WindowPosChanged.
         */
        data->wayland_surface = NULL;
        update_client_surfaces(data->hwnd);
        wayland_surface_destroy(surface);
    }

    if (!(surface = data->wayland_surface) && !(surface = wayland_surface_create(data->hwnd))) return FALSE;

    /* Pass through mouse events for layered, transparent windows, to match
     * Windows behavior. */
    input_region = ((exstyle & WS_EX_TRANSPARENT) && (exstyle & WS_EX_LAYERED)) ?
                   wl_compositor_create_region(process_wayland.wl_compositor) :
                   NULL;
    wl_surface_set_input_region(surface->wl_surface, input_region);
    if (input_region) wl_region_destroy(input_region);

    /* If the window is a visible toplevel make it a wayland
     * xdg_toplevel. Otherwise keep it role-less to avoid polluting the
     * compositor with empty xdg_toplevels. */
    switch (role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        wayland_surface_clear_role(surface);
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        wayland_surface_make_toplevel(surface);
        /* Lets the compositor keep dialogs above, and float them over, their owner. */
        if (surface->xdg_toplevel)
            xdg_toplevel_set_parent(surface->xdg_toplevel,
                                    owner_surface && wayland_surface_is_toplevel(owner_surface) ?
                                    owner_surface->xdg_toplevel : NULL);
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        wayland_surface_make_subsurface(surface, owner_surface);
        break;
    }

    wayland_win_data_get_config(data, &surface->window);

    /* Size/position changes affect the effective pointer constraint, so update
     * it as needed. */
    if (data->hwnd == NtUserGetForegroundWindow()) reapply_cursor_clipping();

    TRACE("hwnd=%p surface=%p=>%p\n", data->hwnd, data->wayland_surface, surface);
    data->wayland_surface = surface;
    return TRUE;
}

static void wayland_surface_update_state_toplevel(struct wayland_surface *surface)
{
    BOOL processing_config = surface->processing.serial &&
                             !surface->processing.processed;

    TRACE("hwnd=%p window_state=%#x %s->state=%#x\n",
          surface->hwnd, surface->window.state,
          processing_config ? "processing" : "current",
          processing_config ? surface->processing.state : surface->current.state);

    /* If we are not processing a compositor requested config, use the
     * window state to determine and update the Wayland state. */
    if (!processing_config)
    {
        xdg_toplevel_set_min_size(surface->xdg_toplevel, 0, 0);
        xdg_toplevel_set_max_size(surface->xdg_toplevel, 0, 0);

         /* First do all state unsettings, before setting new state. Some
          * Wayland compositors misbehave if the order is reversed. */
        if (!(surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            !surface->window.minimized && !surface->compositor_maximized)
        {
            xdg_toplevel_unset_maximized(surface->xdg_toplevel);
        }
        if (!(surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
            (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
            !surface->window.minimized)
        {
            xdg_toplevel_unset_fullscreen(surface->xdg_toplevel);
        }

        if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
           !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))
        {
            xdg_toplevel_set_maximized(surface->xdg_toplevel);
        }
        if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
           !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
        {
            xdg_toplevel_set_fullscreen(surface->xdg_toplevel, NULL);
        }
        if (surface->window.minimized)
        {
            xdg_toplevel_set_minimized(surface->xdg_toplevel);
        }
    }
    else
    {
        surface->processing.processed = TRUE;
    }
}

static void wayland_win_data_update_wayland_state(struct wayland_win_data *data)
{
    struct wayland_surface *surface = data->wayland_surface;

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (!surface->xdg_surface) break; /* surface role has been cleared */
        wayland_surface_update_state_toplevel(surface);
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        TRACE("hwnd=%p subsurface owner=%p\n", surface->hwnd, surface->owner_hwnd);
        /* Although subsurfaces don't have a dedicated surface config mechanism,
         * we use the config fields to mark them as updated. */
        surface->processing.serial = 1;
        surface->processing.processed = TRUE;
        /* A move alone brings no new contents, so nothing would push the new
         * position to the compositor until the next paint: apply it now. */
        if (data->window_contents) wayland_surface_reconfigure(surface);
        break;
    }

    wl_display_flush(process_wayland.wl_display);
}

static BOOL is_managed(HWND hwnd)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    BOOL ret = data && data->managed;
    if (data) wayland_win_data_release(data);
    return ret;
}

static HWND *build_hwnd_list(void)
{
    NTSTATUS status;
    HWND *list;
    ULONG count = 128;

    for (;;)
    {
        if (!(list = malloc(count * sizeof(*list)))) return NULL;
        status = NtUserBuildHwndList(0, 0, 0, 0, 0, count, list, &count);
        if (!status) return list;
        free(list);
        if (status != STATUS_BUFFER_TOO_SMALL) return NULL;
    }
}

/* The last window this process mapped as a managed toplevel.  An unowned
 * notification bubble has no Win32 owner to hang on, so the window it is
 * being shown over -- the application's own toplevel -- is the only surface
 * owner there is. */
static HWND last_managed_toplevel;

/* Returns the toplevel an unowned caption-less popup belongs to, or 0.
 *
 * Only a popup that is visibly smaller than that toplevel *and* drawn within
 * its rect is taken for a bubble.  A caption-less popup of the same size is a
 * part of the application's own UI (a WebView2 host the size of the frame), the
 * loading screen is as large as the frame it covers, and one shown outside the
 * application's rect (a toast in the corner of the screen) cannot be drawn as
 * part of it at all.  All of those must keep a surface of their own. */
static HWND popup_anchor(HWND hwnd, const RECT *rect)
{
    static int enabled = -1;
    struct wayland_win_data *data;
    HWND anchor = last_managed_toplevel;
    RECT anchor_rect;
    BOOL ok = FALSE;

    if (enabled < 0)
    {
        /* Kill switch while the shape is being verified on the real app: a
         * popup wrongly demoted is clipped by the window it hangs on. */
        const char *env = getenv("WINEWAYLAND_POPUP_ANCHOR");
        enabled = !(env && *env == '0');
    }
    if (!enabled) return 0;
    if (!anchor || anchor == hwnd || !(data = wayland_win_data_get(anchor))) return 0;
    if (data->wayland_surface && wayland_surface_is_toplevel(data->wayland_surface) &&
        (data->wayland_surface->style & WS_VISIBLE))
    {
        anchor_rect = data->rects.window;
        ok = rect->left >= anchor_rect.left && rect->top >= anchor_rect.top &&
             rect->right <= anchor_rect.right && rect->bottom <= anchor_rect.bottom &&
             rect->right - rect->left < (anchor_rect.right - anchor_rect.left) / 2 &&
             rect->bottom - rect->top < (anchor_rect.bottom - anchor_rect.top) / 2;
    }
    wayland_win_data_release(data);
    return ok ? anchor : 0;
}

/* Hands a subsurface back its own toplevel.
 *
 * A subsurface's role belongs to the surface it hangs on, so it cannot outlive
 * it: the compositor drops the role and the next set_position would be a
 * protocol error, which kills the client.  A win32-owned popup is destroyed
 * with its owner and never gets here, but a notification bubble hangs on the
 * application's window for drawing only, so it can. */
static void detach_subsurfaces(HWND owner)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    HWND *list;
    UINT i;

    if (!(list = build_hwnd_list())) return;
    for (i = 0; list[i] != HWND_BOTTOM; i++)
    {
        if (list[i] == owner || !(data = wayland_win_data_get(list[i]))) continue;
        if ((surface = data->wayland_surface) &&
            surface->role == WAYLAND_SURFACE_ROLE_SUBSURFACE && surface->owner_hwnd == owner)
        {
            TRACE("hwnd %p takes its own surface back from %p\n", list[i], owner);
            wayland_surface_clear_role(surface);
            surface->role = WAYLAND_SURFACE_ROLE_NONE;
            wayland_surface_make_toplevel(surface);
            if (surface->xdg_toplevel) xdg_toplevel_set_parent(surface->xdg_toplevel, NULL);
        }
        wayland_win_data_release(data);
    }
    free(list);
}

static BOOL has_owned_popups(HWND hwnd)
{
    HWND *list;
    UINT i;
    BOOL ret = FALSE;

    if (!(list = build_hwnd_list())) return FALSE;

    for (i = 0; list[i] != HWND_BOTTOM; i++)
    {
        if (list[i] == hwnd) break;  /* popups are always above owner */
        if (NtUserGetWindowRelative(list[i], GW_OWNER) != hwnd) continue;
        if ((ret = is_managed(list[i]))) break;
    }

    free(list);
    return ret;
}

static inline HWND get_active_window(void)
{
    GUITHREADINFO info;
    info.cbSize = sizeof(info);
    return NtUserGetGUIThreadInfo(GetCurrentThreadId(), &info) ? info.hwndActive : 0;
}

/* Experimental: owned caption-less resizable windows (floating palettes) as
 * toplevels, with their app-driven drags handed to the compositor. Off by
 * default: the hand-off is not yet reliable (WINEWAYLAND_FLOATING_PALETTES=1). */
static BOOL floating_palettes_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char *env = getenv("WINEWAYLAND_FLOATING_PALETTES");
        enabled = env && *env && *env != '0';
    }
    return enabled;
}

/* EXPERIMENT (WINEWAYLAND_HIDE_UNMAP=1): re-evaluate a window's wayland role when
 * its WS_VISIBLE changes, so a window the application hides (rather than destroys)
 * is unmapped instead of leaving a frozen frame on screen (see
 * WAYLAND_WindowPosChanged). */
static BOOL hide_unmap_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char *env = getenv("WINEWAYLAND_HIDE_UNMAP");
        enabled = env && *env && *env != '0';
    }
    return enabled;
}

/* Phase B of the floating-palette work: hand the app's own move/resize of such
 * a toplevel to the compositor (xdg_toplevel_move/resize).  Kept separate from
 * floating_palettes_enabled so the toplevel change can be tested without it
 * (WINEWAYLAND_PALETTE_DRAG=1). */
static BOOL palette_drag_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char *env = getenv("WINEWAYLAND_PALETTE_DRAG");
        enabled = env && *env && *env != '0';
    }
    return enabled;
}

/***********************************************************************
 *		is_window_managed
 *
 * Check if a given window should be managed
 */
static BOOL is_window_managed(HWND hwnd, UINT swp_flags, BOOL fullscreen)
{
    DWORD style, ex_style;

    /* child windows are not managed */
    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if ((style & (WS_CHILD|WS_POPUP)) == WS_CHILD) return FALSE;
    /* activated windows are managed */
    if (!(swp_flags & (SWP_NOACTIVATE|SWP_HIDEWINDOW))) return TRUE;
    if (hwnd == get_active_window()) return TRUE;
    /* windows with caption are managed */
    if ((style & WS_CAPTION) == WS_CAPTION) return TRUE;
    /* windows with thick frame are managed */
    if (style & WS_THICKFRAME) return TRUE;
    if (style & WS_POPUP)
    {
        /* popup with sysmenu == caption are managed */
        if (style & WS_SYSMENU) return TRUE;
        /* full-screen popup windows are managed */
        if (fullscreen) return TRUE;
    }
    /* application windows are managed */
    ex_style = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_APPWINDOW) return TRUE;
    /* windows that own popups are managed */
    if (has_owned_popups(hwnd)) return TRUE;
    /* default: not managed */
    return FALSE;
}

/***********************************************************************
 *           WAYLAND_DestroyWindow
 */
void WAYLAND_DestroyWindow(HWND hwnd)
{
    struct wayland_win_data *data;

    TRACE("%p\n", hwnd);

    detach_subsurfaces(hwnd);
    if (!(data = wayland_win_data_get(hwnd))) return;
    if (hwnd == last_managed_toplevel) last_managed_toplevel = 0;
    wayland_win_data_destroy(data);
}

/***********************************************************************
 *           WAYLAND_WindowPosChanging
 */
BOOL WAYLAND_WindowPosChanging(HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);

    TRACE("hwnd %p, swp_flags %04x, shaped %u, rects %s\n", hwnd, swp_flags, shaped, debugstr_window_rects(rects));

    if (!data && !(data = wayland_win_data_create(hwnd, rects))) return FALSE;

    wayland_win_data_release(data);

    return TRUE;
}

/***********************************************************************
 *           WAYLAND_WindowPosChanged
 */
void WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface)
{
    HWND owner = NtUserGetAncestor(hwnd, GA_ROOT), win_owner = NtUserGetWindowRelative(hwnd, GW_OWNER);
    DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    struct wayland_surface *owner_surface;
    struct wayland_win_data *data, *owner_data;
    HWND anchor;
    BOOL managed, fullscreen = swp_flags & WINE_SWP_FULLSCREEN;

    TRACE("hwnd %p new_rects %s after %p flags %08x\n", hwnd, debugstr_window_rects(new_rects), insert_after, swp_flags);

    /* Get the managed state with win_data unlocked, as is_window_managed
     * may need to query win_data information about other HWNDs and thus
     * acquire the lock itself internally. */
    managed = is_window_managed(hwnd, swp_flags, fullscreen);
    if (win_owner) win_owner = NtUserGetAncestor(win_owner, GA_ROOT);
    /* Owned windows without a caption (command line, notification bubbles,
     * menus, tooltips) belong to their owner's surface even when they are
     * activated: as separate toplevels they could be neither positioned nor
     * kept over it. A *resizable* one (WS_THICKFRAME: a floating palette) is
     * a real window the user drags around and onto other monitors, which only
     * a toplevel can do; its app-driven moves and resizes are handed to the
     * compositor below. */
    if (managed && win_owner && !fullscreen && (style & WS_CAPTION) != WS_CAPTION &&
        !(floating_palettes_enabled() && (style & WS_THICKFRAME)) &&
        !(NtUserGetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_APPWINDOW))
        managed = FALSE;
    /* An unowned caption-less popup shown over the application's own window (a
     * notification bubble) is a separate, unowned window on Windows, but it is
     * drawn over the application and the user never manages it.  As a toplevel
     * the compositor tiles it, borders it and focuses it, which is felt as the
     * bubble grabbing the application.  Hang it on that toplevel as a *surface*
     * owner only: Win32 ownership is untouched -- GetWindow(GW_OWNER) stays
     * null -- but it is drawn as part of the application, like the owned popups
     * above.
     *
     * Decide that from the window's shape alone.  Both of win32u's rules fail
     * here: a bubble is shown without activation, so it is not "managed", and
     * the owner hint is a WindowFromPoint() guess at the popup's corner, which
     * misses whenever that corner is not over the application -- and would
     * also clip a popup that is too large to fit, since a subsurface cannot
     * exceed its parent's surface.  Anything that is not drawn inside the
     * application's toplevel and smaller than it cannot be drawn as part of
     * it, so it keeps the surface of its own that it asked for. */
    if (!win_owner && !fullscreen && (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION &&
        !(NtUserGetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_APPWINDOW))
    {
        if ((anchor = popup_anchor(hwnd, &new_rects->window)))
        {
            owner = anchor;
            managed = FALSE;
        }
    }
    else if (!managed && surface) owner = owner_hint ? owner_hint : win_owner;
    else if (managed && win_owner) owner = win_owner;

    TRACE("hwnd %p style %#x owner %p managed %d surface owner %p\n", hwnd, (UINT)style, win_owner, managed, owner);

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (data->wayland_surface)
    {
        BOOL was_visible = (data->wayland_surface->style & WS_VISIBLE) == WS_VISIBLE;
        BOOL now_visible = (style & WS_VISIBLE) == WS_VISIBLE;

        data->wayland_surface->style = style;
        /* EXPERIMENT (WINEWAYLAND_HIDE_UNMAP=1): the wayland role follows
         * WS_VISIBLE (wayland_win_data_update_wayland_state picks
         * WAYLAND_SURFACE_ROLE_NONE for a hidden window, which clears the role and
         * unpaints the surface), but that state was only recomputed when the
         * surface was *created* -- so a window the application *hides* rather than
         * destroys kept its role and its last buffer, and the compositor kept
         * showing it: AutoCAD's loading screen stayed on screen as a frozen ghost
         * over a running application, which reads as a stuck load (2026-10-07). */
        if (hide_unmap_enabled() && was_visible != now_visible)
            wayland_win_data_update_wayland_state(data);
    }
    owner_data = owner && owner != hwnd ? wayland_win_data_get(owner) : NULL;
    owner_surface = owner_data ? owner_data->wayland_surface : NULL;

    /* A managed toplevel that the application moves or resizes while the
     * pointer button is held on it is being dragged by the user through the
     * app's own code (custom title bars and grips: AutoCAD's palettes). The
     * compositor ignores client positions, so hand the drag to it. */
    if (palette_drag_enabled() && managed && surface && data->wayland_surface && (style & WS_VISIBLE) &&
        wayland_surface_is_toplevel(data->wayland_surface))
    {
        const RECT *o = &data->rects.window, *n = &new_rects->window;
        BOOL moved = (o->left != n->left || o->top != n->top) &&
                     (o->right - o->left == n->right - n->left) && (o->bottom - o->top == n->bottom - n->top);
        BOOL resized = (o->right - o->left != n->right - n->left) || (o->bottom - o->top != n->bottom - n->top);
        BOOL held;
        uint32_t button_serial;
        HWND focused;

        pthread_mutex_lock(&process_wayland.pointer.mutex);
        button_serial = process_wayland.pointer.button_serial;
        focused = process_wayland.pointer.focused_hwnd;
        pthread_mutex_unlock(&process_wayland.pointer.mutex);

        /* The pointer is normally over a child subsurface of the toplevel while
         * the user drags a palette's title strip (AutoCAD's AfxControlBar140u),
         * not over the toplevel itself, so accept any window whose root ancestor
         * is the toplevel being moved. */
        held = button_serial && focused &&
               (focused == hwnd || NtUserGetAncestor(focused, GA_ROOT) == hwnd);

        /* Diagnostic: which side of the hand-off condition fails during a drag. */
        if (moved || resized)
            TRACE_(handoff)("hwnd %p: moved=%d resized=%d button_serial=%u focused=%p held=%d flags=%x\n",
                            hwnd, moved, resized, button_serial, focused, held, swp_flags);

        /* Hand only a *move* to the compositor (xdg_toplevel_move): a Wayland
         * toplevel cannot position itself, so the app's SetWindowPos for a move
         * cannot be honoured any other way.  A resize is left to the app's own
         * size commit (the compositor honours a toplevel's requested size), which
         * already works and is Phase 3's concern. */
        if (held && moved && !(swp_flags & SWP_NOSENDCHANGING))
        {
            data->drag_pending = TRUE;
            data->drag_is_resize = FALSE;
            TRACE_(handoff)("hwnd %p: app move with button held, handing to the compositor\n", hwnd);
        }
    }

    /* The application moved or resized this window: it wants it placed somewhere,
     * so stop treating it as a popup that is merely covered (see
     * wayland_unmap_covered_popups).  A window that is only repainted keeps the
     * same rect and stays covered. */
    if (data->wayland_surface &&
        (data->rects.window.left != new_rects->window.left ||
         data->rects.window.top != new_rects->window.top ||
         data->rects.window.right != new_rects->window.right ||
         data->rects.window.bottom != new_rects->window.bottom))
    {
        data->wayland_surface->covered_popup = FALSE;
    }

    data->rects = *new_rects;
    data->is_fullscreen = fullscreen;
    data->resizeable = swp_flags & WINE_SWP_RESIZABLE;
    data->managed = managed;
    if (managed && data->wayland_surface && wayland_surface_is_toplevel(data->wayland_surface))
        last_managed_toplevel = hwnd;


    if (!surface)
    {
        if (data->wayland_surface)
        {
            wayland_surface_destroy(data->wayland_surface);
            data->wayland_surface = NULL;
        }
    }
    else if (wayland_win_data_create_wayland_surface(data, owner_surface))
    {
        wayland_win_data_update_wayland_state(data);
    }

    /* an already mapped toplevel can hand the drag over right away; a new one
     * does it from set_window_surface_contents once its first buffer is up */
    if (data->drag_pending && data->window_contents) wayland_start_compositor_drag(data);

    if (owner_data) wayland_win_data_release(owner_data);
    wayland_win_data_release(data);
}

/**********************************************************************
 *           wayland_start_compositor_drag
 *
 * Turn the application's own move/resize drag of a toplevel into a
 * compositor interactive move/resize, and end the application's drag with a
 * synthesized button release: it will get no more pointer events until the
 * compositor is done, and never the release itself.
 */
void wayland_start_compositor_drag(struct wayland_win_data *data)
{
    struct wayland_surface *surface = data->wayland_surface;
    struct wl_seat *wl_seat;
    HWND focused;
    uint32_t serial = 0;
    INPUT input = {0};

    data->drag_pending = FALSE;
    if (!surface || !wayland_surface_is_toplevel(surface)) return;

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    focused = process_wayland.pointer.focused_hwnd;
    serial = process_wayland.pointer.button_serial;
    process_wayland.pointer.button_serial = 0;
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    /* The button is usually held over a child subsurface of the toplevel, not
     * the toplevel itself, so also accept its root ancestor (see the hand-off
     * decision in WAYLAND_WindowPosChanged). */
    if (serial && focused != data->hwnd &&
        (!focused || NtUserGetAncestor(focused, GA_ROOT) != data->hwnd))
        serial = 0;
    if (!serial) return;

    pthread_mutex_lock(&process_wayland.seat.mutex);
    if ((wl_seat = process_wayland.seat.wl_seat))
    {
        if (data->drag_is_resize)
            xdg_toplevel_resize(surface->xdg_toplevel, wl_seat, serial, data->drag_resize_edge);
        else
            xdg_toplevel_move(surface->xdg_toplevel, wl_seat, serial);
    }
    pthread_mutex_unlock(&process_wayland.seat.mutex);
    wl_display_flush(process_wayland.wl_display);
    TRACE_(handoff)("hwnd %p: compositor %s started (serial %u)\n", data->hwnd, data->drag_is_resize ? "resize" : "move", serial);

    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    NtUserSendHardwareInput(data->hwnd, SEND_HWMSG_RAWINPUT, &input, 0);
}

static void wayland_configure_window(HWND hwnd)
{
    struct wayland_surface *surface;
    INT width, height;
    UINT flags = 0;
    uint32_t state;
    DWORD style;
    BOOL needs_enter_size_move = FALSE;
    BOOL needs_exit_size_move = FALSE;
    BOOL restoring_from_minimize = FALSE;
    struct wayland_win_data *data;
    RECT rect, surface_rect;

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface))
    {
        wayland_win_data_release(data);
        return;
    }

    if (!wayland_surface_is_toplevel(surface))
    {
        TRACE("missing xdg_toplevel, returning\n");
        wayland_win_data_release(data);
        return;
    }

    if (!surface->requested.serial)
    {
        TRACE("requested configure event already handled, returning\n");
        wayland_win_data_release(data);
        return;
    }

    surface->processing = surface->requested;
    memset(&surface->requested, 0, sizeof(surface->requested));

    state = surface->processing.state;
    /* Ignore size hints if we don't have a state that requires strict
     * size adherence, in order to avoid spurious resizes. */
    if (state)
    {
        width = surface->processing.rect.right - surface->processing.rect.left;
        height = surface->processing.rect.bottom - surface->processing.rect.top;
    }
    else
    {
        width = height = 0;
    }

    if ((state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) && !surface->resizing)
    {
        surface->resizing = TRUE;
        needs_enter_size_move = TRUE;
    }

    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) && surface->resizing)
    {
        surface->resizing = FALSE;
        needs_exit_size_move = TRUE;
    }

    /* Transitions between normal/max/fullscreen may entail a frame change. */
    if ((state ^ surface->current.state) &
        (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
         WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
    {
        flags |= SWP_FRAMECHANGED;
    }

    surface_rect = map_rect_to_surface(surface, surface->window.rect);

    /* If the window is already fullscreen and its size is compatible with what
     * the compositor is requesting, don't force a resize, since some applications
     * are very insistent on a particular fullscreen size (which may not match
     * the monitor size). */
    if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
        wayland_surface_config_is_compatible(&surface->processing, surface_rect,
                                             surface->window.state))
    {
        flags |= SWP_NOSIZE;
    }

    /* Detect a restore from an application-initiated minimize: the last
     * requested config placed the window at the offscreen sentinel position
     * with WS_MINIMIZE, and the compositor is now sending a configure. Ack
     * the configure to avoid a protocol violation and send SC_RESTORE so
     * Win32 runs the full restore sequence (clearing WS_MINIMIZE, restoring
     * position/size, sending WM_SIZE, etc.), which triggers a new configure
     * cycle. */
    restoring_from_minimize = surface->window.rect.left <= -32000 &&
                              surface->window.rect.top  <= -32000 &&
                              surface->window.minimized;
    if (restoring_from_minimize)
    {
        TRACE("hwnd=%p restoring from minimize\n", hwnd);
        surface->current = surface->processing;
        memset(&surface->processing, 0, sizeof(surface->processing));
        xdg_surface_ack_configure(surface->xdg_surface,
                                  surface->current.serial);
        wayland_win_data_release(data);
        send_message(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        return;
    }

    SetRect(&rect, 0, 0, width, height);
    rect = map_rect_from_surface(surface, rect);
    OffsetRect(&rect, data->rects.window.left, data->rects.window.top);

    /* Some compositors (Hyprland) report every tiled, and even floating,
     * window as maximized. Turning that into WS_MAXIMIZE makes the app lay
     * itself out for the monitor's work area and get cropped to the tile, and
     * leaves dialogs in a state they never asked for. Only adopt the size, as
     * for a tiled state, and remember that the maximized state is the
     * compositor's so we don't keep asking it to unmaximize. */
    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
    {
        if ((state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) && !(style & WS_MAXIMIZE))
            surface->compositor_maximized = TRUE;
        else
            surface->compositor_maximized = FALSE;
    }

    wayland_win_data_release(data);

    TRACE("processing=%dx%d,%#x\n", width, height, state);

    if (needs_enter_size_move) send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    if (needs_exit_size_move) send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);

    flags |= SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE;
    if (rect.left == rect.right || rect.bottom == rect.top) flags |= SWP_NOSIZE;

    /* The compositor restored a window that the app had maximized. */
    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) && (style & WS_MAXIMIZE)
        && !(state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
        NtUserSetWindowLong(hwnd, GWL_STYLE, style & ~WS_MAXIMIZE, FALSE);

    /* The Wayland maximized and fullscreen states are very strict about
     * surface size, so don't let the application override it. The tiled state
     * is not as strict, but it indicates a strong size preference, so try to
     * respect it. */
    if (state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                 WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN |
                 WAYLAND_SURFACE_CONFIG_STATE_TILED))
    {
        flags |= SWP_NOSENDCHANGING;
    }

    NtUserSetRawWindowPos(hwnd, rect, flags, FALSE);
}

/**********************************************************************
 *           wayland_restack_popups
 *
 * Stack every popup subsurface owned by a toplevel in Win32 z-order, above
 * the toplevel's client (GL/Vulkan) subsurface if it has one, else directly
 * above the toplevel. Wayland keeps a new subsurface on top of its siblings
 * only until it is placed, and placing it "above the parent" puts it under
 * all of them, so the order has to be rebuilt from Win32's.
 */
void wayland_restack_popups(HWND owner)
{
    struct wayland_win_data *owner_data, *data;
    struct wayland_surface *owner_surface, *surface;
    struct wl_surface *ref;
    HWND hwnd, desktop = NtUserGetDesktopWindow();
    int placed = 0;

    if (!(owner_data = wayland_win_data_get(owner))) return;
    if (!(owner_surface = owner_data->wayland_surface) || !owner_surface->wl_surface)
    {
        wayland_win_data_release(owner_data);
        return;
    }
    ref = owner_surface->client_ref ? owner_surface->client_ref : owner_surface->wl_surface;

    /* walk the top-level windows from bottom-most to top-most */
    hwnd = NtUserGetWindowRelative(desktop, GW_CHILD);
    if (hwnd) hwnd = NtUserGetWindowRelative(hwnd, GW_HWNDLAST);
    for (; hwnd; hwnd = NtUserGetWindowRelative(hwnd, GW_HWNDPREV))
    {
        if (hwnd == owner) continue;
        if (!(data = wayland_win_data_get(hwnd))) continue;
        if ((surface = data->wayland_surface) && surface->role == WAYLAND_SURFACE_ROLE_SUBSURFACE &&
            surface->wl_subsurface && surface->owner_hwnd == owner)
        {
            wl_subsurface_place_above(surface->wl_subsurface, ref);
            ref = surface->wl_surface;
            placed++;
        }
        wayland_win_data_release(data);
    }

    TRACE("owner=%p %d popup(s) restacked above %s\n", owner, placed,
          owner_surface->client_ref ? "client surface" : "owner");
    wl_surface_commit(owner_surface->wl_surface);
    wayland_win_data_release(owner_data);
}

/**********************************************************************
 *           wayland_unmap_covered_popups
 *
 * Windows does not show a popup that another window of the same application
 * covers: the covering window is above it in the z-order, and activating a
 * window raises it over anything it created earlier.  AutoCAD's splash screen
 * relies on exactly that -- it is created *after* the main frame (so it starts
 * above it) and activated while loading, which is the visible loading screen,
 * and it is then neither hidden nor destroyed; on Windows the frame's activation
 * when loading finishes raises the frame over it and the splash is not seen
 * again.  Wayland has no raise request (the compositor stacks) and asking for
 * activation without input would be focus stealing, so a covered popup keeps the
 * stacking it got by being created last and floats over the application: the
 * "stuck on loading" ghost over a running AutoCAD (2026-10-07).
 *
 * So when a window is activated, stop showing any visible, unowned, caption-less
 * popup toplevel that it covers -- the same windows Windows would leave behind
 * the activated one.  Only that shape is considered: an owned popup is drawn as
 * part of its owner, and a captioned window is a window the user works with.  The
 * popup comes back by itself if the application ever paints or shows it again.
 */
static void wayland_unmap_covered_popups(HWND activated)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    HWND hwnd, desktop;
    RECT rect;

    if (!(data = wayland_win_data_get(activated))) return;
    surface = data->wayland_surface;
    if (!surface || !surface->wl_surface || !wayland_surface_is_toplevel(surface) ||
        !(surface->style & WS_VISIBLE))
    {
        wayland_win_data_release(data);
        return;
    }
    rect = data->rects.window;
    wayland_win_data_release(data);

    /* walk the top-level windows from bottom-most to top-most; everything before
     * the activated window is below it, and that is what it can cover. */
    desktop = NtUserGetDesktopWindow();
    hwnd = NtUserGetWindowRelative(desktop, GW_CHILD);
    if (hwnd) hwnd = NtUserGetWindowRelative(hwnd, GW_HWNDLAST);
    for (; hwnd; hwnd = NtUserGetWindowRelative(hwnd, GW_HWNDPREV))
    {
        struct wayland_win_data *other;
        struct wayland_surface *other_surface;
        RECT *r;

        if (hwnd == activated) break;
        if (!(other = wayland_win_data_get(hwnd))) continue;
        other_surface = other->wayland_surface;
        r = &other->rects.window;
        if (other_surface && other_surface->wl_surface &&
            wayland_surface_is_toplevel(other_surface) &&
            (other_surface->style & WS_VISIBLE) &&
            (other_surface->style & WS_POPUP) && !(other_surface->style & WS_CAPTION) &&
            !NtUserGetWindowRelative(hwnd, GW_OWNER) &&
            r->left >= rect.left && r->top >= rect.top &&
            r->right <= rect.right && r->bottom <= rect.bottom)
        {
            TRACE("hwnd=%p is covered by activated hwnd=%p, unmapping it\n", hwnd, activated);
            other_surface->covered_popup = TRUE;
            wayland_surface_unmap(other_surface);
        }
        wayland_win_data_release(other);
    }
}

/**********************************************************************
 *           WAYLAND_ActivateWindow
 *
 * Win32 activated a window: ask the compositor to give it keyboard focus.
 * Wayland clients cannot take focus themselves; xdg-activation lets us ask
 * for it with a token derived from the last input event we received, which
 * compositors honour (Hyprland with focus_on_activate, KDE, GNOME with its
 * focus-stealing rules).  Without this a modal dialog opened by the
 * application only gets keyboard input once the pointer moves over it.
 */
struct activation_request
{
    HWND hwnd;
};

static void activation_token_handle_done(void *data, struct xdg_activation_token_v1 *token,
                                         const char *token_str)
{
    struct activation_request *req = data;
    struct wayland_win_data *win;
    struct wayland_surface *surface;

    if ((win = wayland_win_data_get(req->hwnd)))
    {
        if ((surface = win->wayland_surface) && surface->wl_surface &&
            wayland_surface_is_toplevel(surface))
        {
            TRACE("hwnd=%p token=%s\n", req->hwnd, token_str);
            xdg_activation_v1_activate(process_wayland.xdg_activation_v1, token_str,
                                       surface->wl_surface);
            wl_display_flush(process_wayland.wl_display);
        }
        wayland_win_data_release(win);
    }
    xdg_activation_token_v1_destroy(token);
    free(req);
}

static const struct xdg_activation_token_v1_listener activation_token_listener =
{
    activation_token_handle_done
};

void WAYLAND_ActivateWindow(HWND hwnd, HWND previous)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    struct xdg_activation_token_v1 *token;
    struct activation_request *req;
    HWND focused;
    uint32_t serial;
    BOOL ok;

    /* Before anything else, and even when we cannot ask for focus (no input
     * serial: this is how a launch finishes loading), stop showing a popup that
     * the window being activated covers -- that is what Windows' z-order does
     * for it. */
    wayland_unmap_covered_popups(hwnd);

    if (!process_wayland.xdg_activation_v1) return;

    /* The compositor already gave this window keyboard focus (this is how
     * our own WM_WAYLAND_SET_FOREGROUND ends up here). */
    pthread_mutex_lock(&process_wayland.keyboard.mutex);
    focused = process_wayland.keyboard.focused_hwnd;
    pthread_mutex_unlock(&process_wayland.keyboard.mutex);
    if (focused == hwnd) return;

    /* Only ask on behalf of a user input event we received: a token without
     * a serial is a focus-stealing request (a freshly launched program
     * pulling the user to its workspace), and compositors that don't reject
     * it would let every Wine launch do that. input_serial is armed by the
     * keyboard and pointer button handlers only: the serials on focus
     * enter/leave and modifier updates belong to no user action. */
    serial = InterlockedCompareExchange(&process_wayland.input_serial, 0, 0);
    if (!serial) return;

    if (!(data = wayland_win_data_get(hwnd))) return;
    surface = data->wayland_surface;
    ok = surface && surface->wl_surface && wayland_surface_is_toplevel(surface) && data->managed;
    wayland_win_data_release(data);
    if (!ok) return;

    if (!(req = malloc(sizeof(*req)))) return;
    req->hwnd = hwnd;
    token = xdg_activation_v1_get_activation_token(process_wayland.xdg_activation_v1);
    xdg_activation_token_v1_add_listener(token, &activation_token_listener, req);

    pthread_mutex_lock(&process_wayland.seat.mutex);
    if (process_wayland.seat.wl_seat)
        xdg_activation_token_v1_set_serial(token, serial, process_wayland.seat.wl_seat);
    pthread_mutex_unlock(&process_wayland.seat.mutex);

    /* the surface that currently has focus, if it is ours, vouches for the request */
    if (focused && (data = wayland_win_data_get(focused)))
    {
        if ((surface = data->wayland_surface) && surface->wl_surface)
            xdg_activation_token_v1_set_surface(token, surface->wl_surface);
        wayland_win_data_release(data);
    }

    xdg_activation_token_v1_commit(token);
    wl_display_flush(process_wayland.wl_display);

    TRACE("hwnd=%p previous=%p focused=%p serial=%u\n", hwnd, previous, focused, serial);
}

/**********************************************************************
 *           WAYLAND_WindowMessage
 */
LRESULT WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_WAYLAND_INIT_DISPLAY_DEVICES:
        NtUserCallNoParam(NtUserCallNoParam_DisplayModeChanged);
        return 0;
    case WM_WAYLAND_CONFIGURE:
        wayland_configure_window(hwnd);
        return 0;
    case WM_WAYLAND_SET_FOREGROUND:
    {
        /* The message is posted when the keyboard enter event arrives, and
         * its thread may have a backlog to work through before it is
         * dispatched: the focus can have left this window (and Wine) in the
         * meantime, and wayland_keyboard_update_foreground() has then
         * already released the foreground window.  Claiming it now would
         * name a window the user is no longer in, which is exactly what
         * these two together exist to prevent. */
        HWND focused;

        pthread_mutex_lock(&process_wayland.keyboard.mutex);
        focused = process_wayland.keyboard.focused_hwnd;
        pthread_mutex_unlock(&process_wayland.keyboard.mutex);
        if (focused != hwnd) return 0;

        NtUserSetForegroundWindowInternal(hwnd);
        return 0;
    }
    default:
        FIXME("got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, (long)wp, lp);
        return 0;
    }
}

/**********************************************************************
 *           WAYLAND_DesktopWindowProc
 */
LRESULT WAYLAND_DesktopWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return NtUserMessageCall(hwnd, msg, wp, lp, 0, NtUserDefWindowProc, FALSE);
}

/*****************************************************************
 *		WAYLAND_SetLayeredWindowAttributes
 */
void WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    TRACE("hwnd=%p key=%#x alpha=%u flags=%#x\n", hwnd, (UINT)key, alpha, (UINT)flags);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface))
        wayland_surface_set_opacity(surface, alpha, flags);
    data->layered_attribs_set = TRUE;

    wayland_win_data_release(data);
}

static enum xdg_toplevel_resize_edge hittest_to_resize_edge(WPARAM hittest)
{
    switch (hittest)
    {
    case WMSZ_LEFT:        return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    case WMSZ_RIGHT:       return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    case WMSZ_TOP:         return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case WMSZ_TOPLEFT:     return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    case WMSZ_TOPRIGHT:    return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    case WMSZ_BOTTOM:      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case WMSZ_BOTTOMLEFT:  return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    case WMSZ_BOTTOMRIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    default:               return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }
}

/*****************************************************************
 *		WAYLAND_SetWindowIcons
 */
void WAYLAND_SetWindowIcons(HWND hwnd, HICON icon, const ICONINFO *ii, HICON icon_small, const ICONINFO *ii_small)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("hwnd=%p icon=%p ii=%p icon_small=%p ii_small=%p\n", hwnd, icon, ii, icon_small, ii_small);

    if (process_wayland.xdg_toplevel_icon_manager_v1)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            if ((surface = data->wayland_surface))
            {
                wayland_surface_set_icon_buffer(surface, ICON_BIG, ii);
                if (icon_small) wayland_surface_set_icon_buffer(surface, ICON_SMALL, ii_small);
                if (wayland_surface_is_toplevel(surface))
                    wayland_surface_assign_icon(surface);
            }
            wayland_win_data_release(data);
        }
    }
}

/***********************************************************************
 *		WAYLAND_SetWindowStyle
 */
void WAYLAND_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    DWORD changed = style->styleNew ^ style->styleOld;

    if (hwnd == NtUserGetDesktopWindow()) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    /* Changing WS_EX_LAYERED resets attributes */
    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED))
    {
        if ((surface = data->wayland_surface))
            wayland_surface_set_opacity(surface, 0, 0);
        data->layered_attribs_set = FALSE;
    }

    wayland_win_data_release(data);
}

/*****************************************************************
 *		WAYLAND_SetWindowText
 */
void WAYLAND_SetWindowText(HWND hwnd, LPCWSTR text)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("hwnd=%p text=%s\n", hwnd, wine_dbgstr_w(text));

    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
            wayland_surface_set_title(surface, text);
        wayland_win_data_release(data);
    }
}

/***********************************************************************
 *          WAYLAND_SysCommand
 */
LRESULT WAYLAND_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam, const POINT *pos)
{
    LRESULT ret = -1;
    WPARAM command = wparam & 0xfff0;
    uint32_t button_serial;
    struct wl_seat *wl_seat;
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("cmd=%lx hwnd=%p, %lx, %lx\n",
          (long)command, hwnd, (long)wparam, lparam);

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.focused_hwnd == hwnd)
        button_serial = process_wayland.pointer.button_serial;
    else
        button_serial = 0;
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    if (command == SC_MOVE || command == SC_SIZE)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            pthread_mutex_lock(&process_wayland.seat.mutex);
            wl_seat = process_wayland.seat.wl_seat;
            if (wl_seat && (surface = data->wayland_surface) &&
                wayland_surface_is_toplevel(surface) && button_serial)
            {
                if (command == SC_MOVE)
                {
                    xdg_toplevel_move(surface->xdg_toplevel, wl_seat, button_serial);
                }
                else if (command == SC_SIZE)
                {
                    xdg_toplevel_resize(surface->xdg_toplevel, wl_seat, button_serial,
                                        hittest_to_resize_edge(wparam & 0x0f));
                }
                /* the compositor does it; otherwise (a subsurface, or no button
                 * press to hand over) leave it to win32u's own move/size loop,
                 * which works for a subsurface since the app positions it */
                ret = 0;
            }
            pthread_mutex_unlock(&process_wayland.seat.mutex);
            wayland_win_data_release(data);
        }
    }

    wl_display_flush(process_wayland.wl_display);
    return ret;
}

/***********************************************************************
 *          WAYLAND_UpdateLayeredWindow
 */
void WAYLAND_UpdateLayeredWindow(HWND hwnd, BYTE alpha, UINT flags)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    TRACE("hwnd=%p alpha=%u flags=%#x\n", hwnd, alpha, flags);
    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface))
        wayland_surface_set_opacity(surface, alpha, flags);

    wayland_win_data_release(data);
}

void set_client_surface(HWND hwnd, struct wayland_client_surface *new_client)
{
    HWND toplevel = new_client->client.toplevel;
    RECT rect = new_client->client.monitor_rect;
    struct wayland_client_surface *old_client;
    struct wayland_win_data *data;
    BOOL visible = FALSE;

    /* ownership is shared with the callers, the last caller to release
     * its reference will also destroy it and clear our pointer. */
    if(toplevel) visible = NtUserIsWindowVisible(hwnd);
    if (!(data = wayland_win_data_get(hwnd))) return;

    if (new_client != data->client_surface)
    {
        if ((old_client = data->client_surface))
            wayland_client_surface_attach(old_client, NULL, NULL);

        if ((data->client_surface = new_client))
        {
            if (toplevel && visible)
                wayland_client_surface_attach(new_client, toplevel, &rect);
            else
                wayland_client_surface_attach(new_client, NULL, NULL);
        }
    }

    wayland_win_data_release(data);
}

/* Present one buffer per compositor frame: commit now and ask for a frame
 * callback; until it arrives, later contents are only recorded (they keep
 * accumulating in the same not-yet-committed buffer, see
 * wayland_window_surface_flush) and go out in one commit at the callback.
 * Apps that repaint in many small pieces (WPF resizing a palette: 600-900
 * commits per second, a full buffer copy each) otherwise swamp the
 * compositor and show every partial state. */
static void frame_callback_done(void *data, struct wl_callback *callback, uint32_t time);
static const struct wl_callback_listener frame_callback_listener = { frame_callback_done };

static void request_frame_callback(struct wayland_surface *surface)
{
    surface->frame_cb = wl_surface_frame(surface->wl_surface);
    wl_callback_add_listener(surface->frame_cb, &frame_callback_listener, surface->hwnd);
}

/* Windows with contents deferred to a frame callback, with the time of the
 * deferral: a compositor does not send frame callbacks to a surface it does
 * not render (occluded, or, in Hyprland's case, some subsurfaces), so the
 * event loop presents what has waited longer than a frame anyway. */
struct pending_present
{
    struct list entry;
    HWND hwnd;
    ULONGLONG since;
};
static struct list pending_presents = LIST_INIT(pending_presents);
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;

static ULONGLONG monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pending_present_add(HWND hwnd)
{
    struct pending_present *p;
    pthread_mutex_lock(&pending_mutex);
    LIST_FOR_EACH_ENTRY(p, &pending_presents, struct pending_present, entry)
        if (p->hwnd == hwnd) goto done;
    if ((p = malloc(sizeof(*p))))
    {
        p->hwnd = hwnd;
        p->since = monotonic_ms();
        list_add_tail(&pending_presents, &p->entry);
    }
done:
    pthread_mutex_unlock(&pending_mutex);
}

static void pending_present_remove(HWND hwnd)
{
    struct pending_present *p;
    pthread_mutex_lock(&pending_mutex);
    LIST_FOR_EACH_ENTRY(p, &pending_presents, struct pending_present, entry)
        if (p->hwnd == hwnd) { list_remove(&p->entry); free(p); break; }
    pthread_mutex_unlock(&pending_mutex);
}

BOOL wayland_has_pending_present(void)
{
    BOOL ret;
    pthread_mutex_lock(&pending_mutex);
    ret = !list_empty(&pending_presents);
    pthread_mutex_unlock(&pending_mutex);
    return ret;
}

/* present a window's deferred contents now (win data held by the caller) */
static void present_pending(struct wayland_win_data *data, struct wayland_surface *surface)
{
    int win_width = surface->window.rect.right - surface->window.rect.left;
    int win_height = surface->window.rect.bottom - surface->window.rect.top;

    if (surface->frame_cb)
    {
        wl_callback_destroy(surface->frame_cb);
        surface->frame_cb = NULL;
    }
    /* a buffer deferred before a resize can be smaller than the window now;
     * presenting it would sample past its end (stripes) - the resize brings
     * new contents anyway */
    if (data->pending_contents->width < win_width || data->pending_contents->height < win_height)
        TRACE("hwnd=%p dropping stale %dx%d contents for a %dx%d window\n", data->hwnd,
              data->pending_contents->width, data->pending_contents->height, win_width, win_height);
    else if (wayland_surface_reconfigure(surface))
    {
        wayland_surface_attach_shm(surface, data->pending_contents, data->pending_damage);
        wl_surface_commit(surface->wl_surface);
        request_frame_callback(surface);
        wl_display_flush(process_wayland.wl_display);
    }
    wayland_shm_buffer_unref(data->pending_contents);
    data->pending_contents = NULL;
    NtGdiSetRectRgn(data->pending_damage, 0, 0, 0, 0);
    pending_present_remove(data->hwnd);
}

/* called by the event loop: present whatever has waited more than a frame */
void wayland_present_expired(void)
{
    ULONGLONG now = monotonic_ms();
    HWND expired[32];
    int n = 0, i;
    struct pending_present *p;

    pthread_mutex_lock(&pending_mutex);
    LIST_FOR_EACH_ENTRY(p, &pending_presents, struct pending_present, entry)
        if (now - p->since >= 16 && n < ARRAY_SIZE(expired)) expired[n++] = p->hwnd;
    pthread_mutex_unlock(&pending_mutex);

    for (i = 0; i < n; i++)
    {
        struct wayland_win_data *data = wayland_win_data_get(expired[i]);
        if (!data) { pending_present_remove(expired[i]); continue; }
        if (data->pending_contents && data->wayland_surface)
        {
            TRACE("hwnd=%p frame callback overdue, presenting\n", expired[i]);
            present_pending(data, data->wayland_surface);
        }
        else pending_present_remove(expired[i]);
        wayland_win_data_release(data);
    }
}

static void frame_callback_done(void *arg, struct wl_callback *callback, uint32_t time)
{
    HWND hwnd = arg;
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    data = wayland_win_data_get(hwnd);
    TRACE("hwnd=%p pending=%p\n", hwnd, data ? data->pending_contents : NULL);
    if (data)
    {
        if ((surface = data->wayland_surface) && surface->frame_cb == callback)
        {
            surface->frame_cb = NULL;
            if (data->pending_contents) present_pending(data, surface);
        }
        wayland_win_data_release(data);
    }
    wl_callback_destroy(callback);
}

BOOL set_window_surface_contents(HWND hwnd, struct wayland_shm_buffer *shm_buffer, HRGN damage_region)
{
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;
    BOOL committed = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if ((wayland_surface = data->wayland_surface))
    {
        if (!wayland_surface_reconfigure(wayland_surface))
        {
            TRACE("Wayland surface not configured yet, not flushing\n");
        }
        else if (wayland_surface->frame_cb)
        {
            /* a frame is in flight: present these contents at its callback */
            TRACE("hwnd=%p deferring %p to the frame callback\n", hwnd, shm_buffer);
            if (data->pending_contents != shm_buffer)
            {
                if (data->pending_contents) wayland_shm_buffer_unref(data->pending_contents);
                wayland_shm_buffer_ref((data->pending_contents = shm_buffer));
            }
            if (!data->pending_damage) data->pending_damage = NtGdiCreateRectRgn(0, 0, 0, 0);
            NtGdiCombineRgn(data->pending_damage, data->pending_damage, damage_region, RGN_OR);
            pending_present_add(hwnd);
            committed = TRUE;
        }
        else
        {
            wayland_surface_attach_shm(wayland_surface, shm_buffer, damage_region);
            wl_surface_commit(wayland_surface->wl_surface);
            request_frame_callback(wayland_surface);
            committed = TRUE;
            if (data->drag_pending) wayland_start_compositor_drag(data);
        }
    }

    /* Update the latest window buffer for the wayland surface. Note that we
     * only care whether the buffer contains the latest window contents,
     * it's irrelevant if it was actually committed or not. */
    if (data->window_contents)
        wayland_shm_buffer_unref(data->window_contents);
    wayland_shm_buffer_ref((data->window_contents = shm_buffer));

    wayland_win_data_release(data);

    return committed;
}

struct wayland_shm_buffer *get_window_surface_contents(HWND hwnd)
{
    struct wayland_shm_buffer *shm_buffer;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return NULL;
    if ((shm_buffer = data->window_contents)) wayland_shm_buffer_ref(shm_buffer);
    wayland_win_data_release(data);

    return shm_buffer;
}

void wayland_window_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&win_data_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
