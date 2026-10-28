/*
 * Copyright 2023 Zhiyi Zhang for CodeWeavers
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
#ifndef __WINE_DCOMP_PRIVATE_H
#define __WINE_DCOMP_PRIVATE_H

#include "dcomp.h"
#include "dcomp_private_iface.h"
#include "d2d1_1.h"
#include <d3d11.h>
#include "wine/list.h"

struct composition_device
{
    IDCompositionDevice IDCompositionDevice_iface;
    IDCompositionDeviceUnknown IDCompositionDeviceUnknown_iface;
    IDCompositionSurfaceUnknown *drawing_surface;
    struct list targets;
    HANDLE thread;
    BOOL exit_thread;
    BOOL thread_exited;
    int version;
    LONG ref;
};

struct composition_surface_factory
{
    IDCompositionSurfaceFactory IDCompositionSurfaceFactory_iface;
    IDCompositionDevice *device;
    IUnknown *rendering_device;
    GUID rendering_device_iid;
    LONG ref;
};

struct composition_surface
{
    IDCompositionSurfaceUnknown IDCompositionSurfaceUnknown_iface;
    IDCompositionSurfaceFactory *factory;
    ID3D11Texture2D *draw_surface;
    RECT draw_rect;
    IUnknown *physical_surface;
    GUID physical_surface_iid;
    HWND hwnd; /* surface created from a window, captured instead of read back */
    UINT width;
    UINT height;
    DXGI_FORMAT pixel_format;
    DXGI_ALPHA_MODE alpha_mode;
    LONG draw_count;
    LONG ref;
};

struct composition_target
{
    IDCompositionTarget IDCompositionTarget_iface;
    IDCompositionDevice *device;
    IDCompositionVisual *root;
    BOOL topmost;
    HWND hwnd;
    HANDLE shared_visual_handle;
    /* Where the tree of a target created from a shared visual handle is rendered, so that
     * visuals created from the same handle in other processes can composite it. */
    HANDLE shared_mapping;
    void *shared_view;
    HDC shared_dc;
    HBITMAP shared_bitmap;
    void *shared_bits;
    HGDIOBJ shared_old_bitmap;
    struct list entry;
    LONG ref;
};

/* Header of the mapping a shared visual target renders into; the pixels follow it as
 * 32-bit BGRA rows, top down. */
struct shared_visual_header
{
    unsigned int seq;    /* bumped after a whole frame was written */
    unsigned int width;
    unsigned int height;
};

struct composition_visual
{
    IDCompositionVisualUnknown IDCompositionVisualUnknown_iface;
    ID2D1GdiInteropRenderTarget *interop;
    ID2D1DeviceContext *device_context;
    IUnknown *content;
    GUID content_iid;
    enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode;
    enum DCOMPOSITION_BORDER_MODE border_mode;
    enum DCOMPOSITION_BACKFACE_VISIBILITY visibility;
    float offset_x;
    float offset_y;
    D2D_MATRIX_3X2_F transform;
    struct composition_transform *transform_object;
    BOOL has_clip;
    D2D_RECT_F clip;
    enum DCOMPOSITION_COMPOSITE_MODE composite_mode;
    enum DCOMPOSITION_OPACITY_MODE opacity_mode;
    struct list child_visuals; /* visuals closer to head are lower in z-order */
    struct list entry;
    struct composition_visual *parent;
    BOOL is_root;
    BOOL is_child;
    HANDLE shared_visual_handle;
    /* The rendered target of a shared visual that belongs to another process. */
    HANDLE shared_mapping;
    void *shared_view;
    HDC shared_dc;
    HBITMAP shared_bitmap;
    void *shared_bits;
    HGDIOBJ shared_old_bitmap;
    UINT shared_width;
    UINT shared_height;
    int version;
    /* The last composited content, so it is read back from the GPU only when it changes */
    struct
    {
        HDC dc;
        HBITMAP bitmap;
        HGDIOBJ old_bitmap;
        UINT width;
        UINT height;
        IUnknown *content;
        UINT content_version;
    } cache;
    LONG ref;
};

static inline struct composition_device *impl_from_IDCompositionDevice(IDCompositionDevice *iface)
{
    return CONTAINING_RECORD(iface, struct composition_device, IDCompositionDevice_iface);
}

static inline struct composition_device *impl_from_IDCompositionDeviceUnknown(IDCompositionDeviceUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct composition_device, IDCompositionDeviceUnknown_iface);
}

static inline struct composition_surface *impl_from_IDCompositionSurfaceUnknown(IDCompositionSurfaceUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct composition_surface, IDCompositionSurfaceUnknown_iface);
}

static inline struct composition_surface *impl_from_IDCompositionSurface(IDCompositionSurface *iface)
{
    return CONTAINING_RECORD(iface, struct composition_surface, IDCompositionSurfaceUnknown_iface);
}

static inline struct composition_surface_factory *impl_from_IDCompositionSurfaceFactory(IDCompositionSurfaceFactory *iface)
{
    return CONTAINING_RECORD(iface, struct composition_surface_factory, IDCompositionSurfaceFactory_iface);
}

struct composition_surface *unsafe_impl_from_IDCompositionSurface(IDCompositionSurface *iface);

static inline struct composition_target *impl_from_IDCompositionTarget(IDCompositionTarget *iface)
{
    return CONTAINING_RECORD(iface, struct composition_target, IDCompositionTarget_iface);
}

static inline struct composition_visual *impl_from_IDCompositionVisual(IDCompositionVisual *iface)
{
    return CONTAINING_RECORD(iface, struct composition_visual, IDCompositionVisualUnknown_iface);
}

static inline struct composition_visual *impl_from_IDCompositionVisualUnknown(IDCompositionVisualUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct composition_visual, IDCompositionVisualUnknown_iface);
}

void visual_free_cache(struct composition_visual *visual);
void dcomp_lock(void);
void dcomp_unlock(void);
HRESULT create_surface(struct composition_surface_factory *factory, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **dcomp_surface);
HRESULT create_surface_factory(struct composition_device *device, IUnknown *rendering_device, IDCompositionSurfaceFactory **factory);
void composite_thread_start(struct composition_device *device);
HRESULT create_target(struct composition_device *device, HWND hwnd, BOOL topmost, IDCompositionTarget **target);
HRESULT create_target_from_shared_visual_handle(struct composition_device *device, HANDLE shared_visual_handle, void **new_target);
HRESULT create_surface_from_hwnd(HWND hwnd, IDCompositionSurface **surface);
HRESULT create_visual(int version, REFIID iid, void **visual);
HRESULT create_visual_from_shared_visual_handle(HANDLE shared_visual_handle, void **new_visual);
HRESULT WINAPI DCompositionCreateSharedVisualHandle(HANDLE *handle);
struct composition_transform;
struct composition_transform *composition_transform_from_object(IDCompositionTransform *transform);
void composition_transform_release(struct composition_transform *transform);
HRESULT composition_transform_get_matrix(struct composition_transform *transform, D2D_MATRIX_3X2_F *matrix);
HRESULT composition_transform_create_translate(IDCompositionTranslateTransform **transform);
HRESULT composition_transform_create_scale(IDCompositionScaleTransform **transform);
HRESULT composition_transform_create_rotate(IDCompositionRotateTransform **transform);
HRESULT composition_transform_create_skew(IDCompositionSkewTransform **transform);
HRESULT composition_transform_create_matrix(IDCompositionMatrixTransform **transform);
HRESULT composition_transform_create_group(IDCompositionTransform **transforms, UINT elements,
        IDCompositionTransform **group);
void shared_visual_target_free(struct composition_target *target);
void shared_visual_cache_free(struct composition_visual *visual);


#endif /* __WINE_DCOMP_PRIVATE_H */
