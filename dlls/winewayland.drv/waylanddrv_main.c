/*
 * WAYLANDDRV initialization code
 *
 * Copyright 2020 Alexandre Frantzis for Collabora Ltd
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
#include <errno.h>
#include <poll.h>

#include <stdlib.h>

#include "ntstatus.h"

#include "waylanddrv.h"

char *process_name = NULL;

static const struct user_driver_funcs waylanddrv_funcs =
{
    .pClipboardWindowProc = WAYLAND_ClipboardWindowProc,
    .pClipCursor = WAYLAND_ClipCursor,
    .pDesktopWindowProc = WAYLAND_DesktopWindowProc,
    .pDestroyWindow = WAYLAND_DestroyWindow,
    .pSetIMECompositionRect = WAYLAND_SetIMECompositionRect,
    .pKbdLayerDescriptor = WAYLAND_KbdLayerDescriptor,
    .pReleaseKbdTables = WAYLAND_ReleaseKbdTables,
    .pSetCursor = WAYLAND_SetCursor,
    .pSetCursorPos = WAYLAND_SetCursorPos,
    .pSetLayeredWindowAttributes = WAYLAND_SetLayeredWindowAttributes,
    .pSetWindowIcons = WAYLAND_SetWindowIcons,
    .pSetWindowStyle = WAYLAND_SetWindowStyle,
    .pSetWindowText = WAYLAND_SetWindowText,
    .pActivateWindow = WAYLAND_ActivateWindow,
    .pSysCommand = WAYLAND_SysCommand,
    .pUpdateLayeredWindow = WAYLAND_UpdateLayeredWindow,
    .pUpdateDisplayDevices = WAYLAND_UpdateDisplayDevices,
    .pWindowMessage = WAYLAND_WindowMessage,
    .pWindowPosChanged = WAYLAND_WindowPosChanged,
    .pWindowPosChanging = WAYLAND_WindowPosChanging,
    .pCreateClientSurface = WAYLAND_CreateClientSurface,
    .pCreateWindowSurface = WAYLAND_CreateWindowSurface,
    .pVulkanInit = WAYLAND_VulkanInit,
    .pOpenGLInit = WAYLAND_OpenGLInit,
};

static void wayland_init_process_name(void)
{
    WCHAR *p, *appname;
    WCHAR appname_lower[MAX_PATH];
    DWORD appname_len;
    DWORD appnamez_size;
    DWORD utf8_size;
    int i;

    appname = RtlGetCurrentPeb()->ProcessParameters->ImagePathName.Buffer;
    if ((p = wcsrchr(appname, '/'))) appname = p + 1;
    if ((p = wcsrchr(appname, '\\'))) appname = p + 1;
    appname_len = lstrlenW(appname);

    if (appname_len == 0 || appname_len >= MAX_PATH) return;

    for (i = 0; appname[i]; i++) appname_lower[i] = RtlDowncaseUnicodeChar(appname[i]);
    appname_lower[i] = 0;

    appnamez_size = (appname_len + 1) * sizeof(WCHAR);

    if (!RtlUnicodeToUTF8N(NULL, 0, &utf8_size, appname_lower, appnamez_size) &&
        (process_name = malloc(utf8_size)))
    {
        RtlUnicodeToUTF8N(process_name, utf8_size, &utf8_size, appname_lower, appnamez_size);
    }
}

static NTSTATUS waylanddrv_unix_init(void *arg)
{
    /* Set the user driver functions now so that they are available during
     * our initialization. We clear them on error. */
    __wine_set_user_driver(&waylanddrv_funcs, WINE_GDI_DRIVER_VERSION);

    wayland_init_process_name();

    if (!wayland_process_init()) goto err;

    return 0;

err:
    __wine_set_user_driver(NULL, WINE_GDI_DRIVER_VERSION);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS waylanddrv_unix_read_events(void *arg)
{
    struct wl_display *display = process_wayland.wl_display;
    struct wl_event_queue *queue = process_wayland.wl_event_queue;
    struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };

    for (;;)
    {
        int ret;

        while (wl_display_prepare_read_queue(display, queue) != 0)
            if (wl_display_dispatch_queue_pending(display, queue) == -1) goto error;
        /* Events dispatched here may have moved the keyboard focus out of
         * Wine: act on that before blocking in poll(). */
        wayland_keyboard_update_foreground();
        wl_display_flush(display);

        /* wake up in time to present contents whose frame callback never came */
        ret = poll(&pfd, 1, wayland_has_pending_present() ? 8 : -1);
        if (ret > 0 && (pfd.revents & POLLIN))
        {
            if (wl_display_read_events(display) == -1) goto error;
        }
        else
        {
            wl_display_cancel_read(display);
            if (ret < 0 && errno != EINTR) goto error;
        }
        if (wl_display_dispatch_queue_pending(display, queue) == -1) goto error;
        wayland_keyboard_update_foreground();
        wayland_present_expired();
    }

error:
    /* This function only returns on a fatal error, e.g., if our connection
     * to the Wayland server is lost. */
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS waylanddrv_unix_init_clipboard(void *arg)
{
    /* If the compositor supports zwlr_data_control_manager_v1, we don't need
     * per-process clipboard window and handling, we can use the default clipboard
     * window from the desktop process. */
    if (process_wayland.zwlr_data_control_manager_v1) return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
    waylanddrv_unix_init_clipboard,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == waylanddrv_unix_func_count);

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
    waylanddrv_unix_init_clipboard,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == waylanddrv_unix_func_count);

#endif /* _WIN64 */
