/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
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
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winternl.h"
#include "initguid.h"
#include "objidl.h"
#include "dcomp_private.h"
#include "dxgi.h"
#include "dwmapi.h"
#include "d3d11_4.h"
#include "d3dcompiler.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static CRITICAL_SECTION dcomp_cs;
static CRITICAL_SECTION_DEBUG dcomp_debug =
{
    0, 0, &dcomp_cs,
    { &dcomp_debug.ProcessLocksList, &dcomp_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dcomp_cs") }
};
static CRITICAL_SECTION dcomp_cs = { &dcomp_debug, -1, 0, 0, 0, 0 };

void dcomp_lock(void)
{
    EnterCriticalSection(&dcomp_cs);
}

void dcomp_unlock(void)
{
    LeaveCriticalSection(&dcomp_cs);
}

static HRESULT STDMETHODCALLTYPE device_QueryInterface(IDCompositionDevice *iface,
        REFIID iid, void **out)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, iid %s, out %p\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionDevice))
    {
        IUnknown_AddRef(&device->IDCompositionDevice_iface);
        *out = &device->IDCompositionDevice_iface;
        return S_OK;
    }
    else if ((device->version >= 2
              && (IsEqualGUID(iid, &IID_IDCompositionDevice2)
                  || IsEqualGUID(iid, &IID_IDCompositionDevice3)
                  || IsEqualGUID(iid, &IID_IDCompositionDesktopDevice)))
              || IsEqualGUID(iid, &IID_IDCompositionDesktopDevicePartner)
              || IsEqualGUID(iid, &IID_IDCompositionDeviceUnknown))
    {
        IUnknown_AddRef(&device->IDCompositionDeviceUnknown_iface);
        *out = &device->IDCompositionDeviceUnknown_iface;

        if (IsEqualGUID(iid, &IID_IDCompositionDesktopDevicePartner)
            || IsEqualGUID(iid, &IID_IDCompositionDeviceUnknown))
            FIXME("Returning undocumented interface %s %p.\n", wine_dbgstr_guid(iid), *out);
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE device_AddRef(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedIncrement(&device->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE device_Release(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedDecrement(&device->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (device->thread)
        {
            WaitForSingleObject(device->thread, INFINITE);
            CloseHandle(device->thread);
        }
        free(device);
    }

    return ref;
}

/* TODO: Optimize this. There is plenty of room */
static HRESULT create_bgra_surface_from_rgba(IDXGISurface *rgba_surface, IDXGISurface **bgra_surface)
{
    static const D3D11_INPUT_ELEMENT_DESC layout_desc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    static const char vs_code[] =
        "void main(in float4 in_position : POSITION,\n"
        "          in float2 in_texcoord : TEXCOORD,\n"
        "          out float4 position : SV_Position,\n"
        "          out float2 texcoord : TEXCOORD)\n"
        "{\n"
        "     position = in_position;\n"
        "     texcoord = in_texcoord;\n"
        "}";

    static const char ps_code[] =
        "Texture2D src_texture : register(t0);\n"
        "SamplerState sam_linear : register(s0);\n"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET"
        "{\n"
        "    return src_texture.Sample(sam_linear, uv);\n"
        "}";

    struct vec2
    {
        float x, y;
    };

    static const struct
    {
        struct vec2 position;
        struct vec2 texcoord;
    } quad[] =
    {
        {{-1.0f, 1.0f}, {0.0f, 0.0f}},   /* Top-Left */
        {{1.0f, 1.0f}, {1.0f, 0.0f}},    /* Top-Right */
        {{-1.0f, -1.0f}, {0.0f, 1.0f}},  /* Bottom-Left */
        {{1.0f, -1.0f}, {1.0f, 1.0f}},   /* Bottom-Right */
    };

    ID3D11Texture2D *src_texture = NULL, *dst_texture = NULL;
    ID3DDeviceContextState *state = NULL, *old_state = NULL;
    ID3D11DeviceContext1 *d3d11_device_context1 = NULL;
    ID3D11DeviceContext *d3d11_device_context = NULL;
    static ID3D10Blob *vs_blob = NULL, *ps_blob = NULL;
    ID3D11InputLayout *input_layout = NULL;
    D3D11_SAMPLER_DESC sampler_desc = {0};
    D3D11_SUBRESOURCE_DATA resource_data;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Device1 *d3d11_device = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11SamplerState *sampler = NULL;
    D3D11_TEXTURE2D_DESC texture_desc;
    DXGI_SURFACE_DESC surface_desc;
    D3D11_BUFFER_DESC buffer_desc;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    unsigned int stride, offset;
    ID3D11Buffer *vb = NULL;
    D3D11_VIEWPORT vp;
    HRESULT hr;

    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    hr = IDXGISurface_QueryInterface(rgba_surface, &IID_ID3D11Texture2D, (void **)&src_texture);
    if (FAILED(hr))
    {
        ERR("Failed to get a ID3D11Texture2D, hr %#lx.\n", hr);
        goto done;
    }

    hr = IDXGISurface_GetDevice(rgba_surface, &IID_ID3D11Device1, (void **)&d3d11_device);
    if (FAILED(hr))
    {
        ERR("Failed to get device, hr %#lx.\n", hr);
        goto done;
    }

    hr = IDXGISurface_GetDesc(rgba_surface, &surface_desc);
    if (FAILED(hr))
    {
        ERR("Failed to get the dxgi surface description, hr %#lx.\n", hr);
        goto done;
    }

    texture_desc.Width = surface_desc.Width;
    texture_desc.Height = surface_desc.Height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = D3D11_RESOURCE_MISC_GUARDED | D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    hr = ID3D11Device1_CreateTexture2D(d3d11_device, &texture_desc, NULL, &dst_texture);
    if (FAILED(hr))
    {
        ERR("Failed to create a ID3D11Texture2D, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID3D11Device1_CreateShaderResourceView(d3d11_device, (ID3D11Resource *)src_texture, NULL, &srv);
    if (FAILED(hr))
    {
        ERR("Failed to create a SRV, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID3D11Device1_CreateRenderTargetView(d3d11_device, (ID3D11Resource *)dst_texture, NULL, &rtv);
    if (FAILED(hr))
    {
        ERR("Failed to create a RTV, hr %#lx.\n", hr);
        goto done;
    }

    ID3D11Device1_GetImmediateContext(d3d11_device, &d3d11_device_context);
    if (FAILED(hr = ID3D11DeviceContext_QueryInterface(d3d11_device_context, &IID_ID3D11DeviceContext1,
                                                       (void **)&d3d11_device_context1)))
    {
        ERR("Failed to query ID3D11DeviceContext1, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = ID3D11Device1_CreateDeviceContextState(d3d11_device, 0, feature_levels,
                                                           ARRAY_SIZE(feature_levels), D3D11_SDK_VERSION,
                                                           &IID_ID3D11Device1, NULL, &state)))
    {
        ERR("Failed to create device context state, hr %#lx.\n", hr);
        goto done;
    }
    ID3D11DeviceContext1_SwapDeviceContextState(d3d11_device_context1, state, &old_state);

    ID3D11DeviceContext1_OMSetRenderTargets(d3d11_device_context1, 1, &rtv, NULL);
    ID3D11DeviceContext1_PSSetShaderResources(d3d11_device_context1, 0, 1, &srv);

    /* Move this to dcomp device instance ? */
    if (vs_blob == NULL)
    {
        hr = D3DCompile(vs_code, sizeof(vs_code) - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vs_blob, NULL);
        if (FAILED(hr))
        {
            ERR("Failed to compile vertex shader, hr %#lx.\n", hr);
            goto done;
        }

        hr = D3DCompile(ps_code, sizeof(ps_code) - 1, "ps", NULL, NULL, "main", "ps_4_0", 0, 0, &ps_blob, NULL);
        if (FAILED(hr))
        {
            ERR("Failed to compile pixel shader, hr %#lx.\n", hr);
            goto done;
        }
    }

    hr = ID3D11Device1_CreateVertexShader(d3d11_device, ID3D10Blob_GetBufferPointer(vs_blob),
                                          ID3D10Blob_GetBufferSize(vs_blob), NULL, &vs);
    if (FAILED(hr))
    {
        ERR("Failed to create vertex shader, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID3D11Device1_CreatePixelShader(d3d11_device, ID3D10Blob_GetBufferPointer(ps_blob),
                                         ID3D10Blob_GetBufferSize(ps_blob), NULL, &ps);
    if (FAILED(hr))
    {
        ERR("Failed to create pixel shader, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID3D11Device1_CreateInputLayout(d3d11_device, layout_desc, ARRAY_SIZE(layout_desc),
                                         ID3D10Blob_GetBufferPointer(vs_blob),
                                         ID3D10Blob_GetBufferSize(vs_blob), &input_layout);
    if (FAILED(hr))
    {
        ERR("Failed to create input layout, hr %#lx.\n", hr);
        goto done;
    }

    buffer_desc.ByteWidth = sizeof(quad);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_desc.CPUAccessFlags = 0;
    buffer_desc.MiscFlags = 0;
    buffer_desc.StructureByteStride = 0;
    resource_data.pSysMem = quad;
    resource_data.SysMemPitch = 0;
    resource_data.SysMemSlicePitch = 0;
    hr = ID3D11Device1_CreateBuffer(d3d11_device, &buffer_desc, &resource_data, &vb);
    if (FAILED(hr))
    {
        ERR("Failed to create vertex buffer, hr %#lx.\n", hr);
        goto done;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = ID3D11Device1_CreateSamplerState(d3d11_device, &sampler_desc, &sampler);
    if (FAILED(hr))
    {
        ERR("Failed to create sampler state, hr %#lx.\n", hr);
        goto done;
    }
    ID3D11DeviceContext1_PSSetSamplers(d3d11_device_context1, 0, 1, &sampler);

    ID3D11DeviceContext1_IASetInputLayout(d3d11_device_context1, input_layout);
    ID3D11DeviceContext1_IASetPrimitiveTopology(d3d11_device_context1, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    stride = sizeof(quad[0]);
    offset = 0;
    ID3D11DeviceContext1_IASetVertexBuffers(d3d11_device_context1, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext1_VSSetShader(d3d11_device_context1, vs, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(d3d11_device_context1, ps, NULL, 0);

    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = surface_desc.Width;
    vp.Height = surface_desc.Height;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext1_RSSetViewports(d3d11_device_context1, 1, &vp);
    ID3D11DeviceContext1_Draw(d3d11_device_context1, 4, 0);

    hr = ID3D11Texture2D_QueryInterface(dst_texture, &IID_IDXGISurface, (void **)bgra_surface);
    if (FAILED(hr))
    {
        ERR("Failed to get bgra surface, hr %#lx.\n", hr);
        goto done;
    }

done:
    if (old_state)
    {
        ID3D11DeviceContext1_SwapDeviceContextState(d3d11_device_context1, old_state, NULL);
        ID3DDeviceContextState_Release(old_state);
    }
    if (state)
        ID3DDeviceContextState_Release(state);
    if (vb)
        ID3D11Buffer_Release(vb);
    if (sampler)
        ID3D11SamplerState_Release(sampler);
    if (input_layout)
        ID3D11InputLayout_Release(input_layout);
    if (ps)
        ID3D11PixelShader_Release(ps);
    if (vs)
        ID3D11VertexShader_Release(vs);
    if (d3d11_device_context1)
        ID3D11DeviceContext1_Release(d3d11_device_context1);
    if (d3d11_device_context)
        ID3D11DeviceContext_Release(d3d11_device_context);
    if (rtv)
        ID3D11RenderTargetView_Release(rtv);
    if (srv)
        ID3D11ShaderResourceView_Release(srv);
    if (dst_texture)
        ID3D11Texture2D_Release(dst_texture);
    if (d3d11_device)
        ID3D11Device1_Release(d3d11_device);
    if (src_texture)
        ID3D11Texture2D_Release(src_texture);
    return hr;
}

/* Keep the cached copy of the visual's content as a DIB section of the given size. */
static BOOL visual_resize_cache(struct composition_visual *visual, UINT width, UINT height)
{
    BITMAPINFO info = {{sizeof(info.bmiHeader)}};
    void *bits;

    if (visual->cache.dc && visual->cache.width == width && visual->cache.height == height)
        return TRUE;

    visual_free_cache(visual);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -(LONG)height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (!(visual->cache.dc = CreateCompatibleDC(0)))
        return FALSE;
    if (!(visual->cache.bitmap = CreateDIBSection(visual->cache.dc, &info, DIB_RGB_COLORS, &bits, NULL, 0)))
    {
        DeleteDC(visual->cache.dc);
        visual->cache.dc = NULL;
        return FALSE;
    }
    visual->cache.old_bitmap = SelectObject(visual->cache.dc, visual->cache.bitmap);
    visual->cache.width = width;
    visual->cache.height = height;
    return TRUE;
}

/* Read the surface back from the GPU into the visual's cache. */
static HRESULT update_visual_cache(struct composition_visual *visual,
                                   IDXGISurface *dxgi_surface)
{
    ID3DDeviceContextState *state = NULL, *old_state = NULL;
    ID3D11DeviceContext1 *d3d11_device_context1 = NULL;
    ID3D11DeviceContext *d3d11_device_context = NULL;
    ID3D11Multithread *multithread = NULL;
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    ID2D1Bitmap1 *target_bitmap = NULL;
    ID3D11Device1 *d3d11_device = NULL;
    IDXGISurface *bgra_surface = NULL;
    DXGI_SURFACE_DESC surface_desc;
    ID2D1Bitmap *src_bitmap = NULL;
    ID2D1Image *old_target = NULL;
    D2D1_SIZE_U size;
    HRESULT hr = E_FAIL;
    HDC src_dc;

    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    hr = IDXGISurface_GetDevice(dxgi_surface, &IID_ID3D11Device1, (void **)&d3d11_device);
    if (FAILED(hr))
    {
        ERR("Failed to get device, hr %#lx.\n", hr);
        goto done;
    }

    ID3D11Device1_GetImmediateContext(d3d11_device, &d3d11_device_context);
    if (FAILED(hr = ID3D11DeviceContext_QueryInterface(d3d11_device_context, &IID_ID3D11DeviceContext1,
                                                       (void **)&d3d11_device_context1)))
    {
        ERR("Failed to query ID3D11DeviceContext1, hr %#lx.\n", hr);
        goto done;
    }

    /* Use ID3D11Multithread to avoid conflicts with rendering in an another thread with the same
     * immediate ID3D11DeviceContext */
    hr = ID3D11DeviceContext_QueryInterface(d3d11_device_context, &IID_ID3D11Multithread, (void **)&multithread);
    if (FAILED(hr))
    {
        ERR("Failed to query ID3D11Multithread, hr %#lx.\n", hr);
        goto done;
    }

    ID3D11Multithread_SetMultithreadProtected(multithread, TRUE);
    ID3D11Multithread_Enter(multithread);

    /* Is this really necessary? anyway, just to be safe */
    if (FAILED(hr = ID3D11Device1_CreateDeviceContextState(d3d11_device, 0, feature_levels,
                                                           ARRAY_SIZE(feature_levels), D3D11_SDK_VERSION,
                                                           &IID_ID3D11Device1, NULL, &state)))
    {
        ERR("Failed to create device context state, hr %#lx.\n", hr);
        goto done;
    }
    ID3D11DeviceContext1_SwapDeviceContextState(d3d11_device_context1, state, &old_state);

    if (FAILED(hr = IDXGISurface_GetDesc(dxgi_surface, &surface_desc)))
    {
        ERR("Failed to get the dxgi surface description, hr %#lx.\n", hr);
        goto done;
    }

    /* Convert DXGI_FORMAT_R8G8B8A8_UNORM to DXGI_FORMAT_B8G8R8A8_UNORM */
    if (surface_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = create_bgra_surface_from_rgba(dxgi_surface, &bgra_surface);
        if (FAILED(hr))
        {
            ERR("Failed to convert DXGI_FORMAT_R8G8B8A8_UNORM surface to DXGI_FORMAT_B8G8R8A8_UNORM, hr %#lx.\n", hr);
            goto done;
        }
        dxgi_surface = bgra_surface;
    }
    else if (surface_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        ERR("DXGI_FORMAT_R16G16B16A16_FLOAT surface is currently unsupported.\n");
        hr = E_NOTIMPL;
        goto done;
    }

    size.width = surface_desc.Width;
    size.height = surface_desc.Height;
    bitmap_desc.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bitmap_desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bitmap_desc.dpiX = 0;
    bitmap_desc.dpiY = 0;
    bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE;
    bitmap_desc.colorContext = NULL;

    assert(visual->device_context);
    if (FAILED(hr = ID2D1DeviceContext_CreateBitmap(visual->device_context, size, NULL, 0,
            &bitmap_desc, &target_bitmap)))
    {
        ERR("Failed to create a bitmap, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = ID2D1DeviceContext_CreateSharedBitmap(visual->device_context, &IID_IDXGISurface,
            dxgi_surface, NULL, &src_bitmap)))
    {
        ERR("Failed to create a bitmap from dxgi surface, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = ID2D1Bitmap1_CopyFromBitmap(target_bitmap, NULL, src_bitmap, NULL)))
    {
        ERR("Failed to copy bitmap, hr %#lx.\n", hr);
        goto done;
    }

    ID2D1DeviceContext_GetTarget(visual->device_context, &old_target);
    ID2D1DeviceContext_SetTarget(visual->device_context, (ID2D1Image *)target_bitmap);
    if (FAILED(hr = ID2D1GdiInteropRenderTarget_GetDC(visual->interop, D2D1_DC_INITIALIZE_MODE_COPY,
            &src_dc)))
    {
        ERR("GetDC failed, hr %#lx.\n", hr);
        ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
        if (old_target)
            ID2D1Image_Release(old_target);
        goto done;
    }

    if (visual_resize_cache(visual, size.width, size.height))
        BitBlt(visual->cache.dc, 0, 0, size.width, size.height, src_dc, 0, 0, SRCCOPY);
    else
        hr = E_OUTOFMEMORY;

    ID2D1GdiInteropRenderTarget_ReleaseDC(visual->interop, NULL);
    ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
    if (old_target)
        ID2D1Image_Release(old_target);

done:
    if (bgra_surface)
        IDXGISurface_Release(bgra_surface);
    if (src_bitmap)
        ID2D1Bitmap_Release(src_bitmap);
    if (target_bitmap)
        ID2D1Bitmap1_Release(target_bitmap);
    if (d3d11_device_context1 && old_state)
    {
        ID3D11DeviceContext1_SwapDeviceContextState(d3d11_device_context1, old_state, NULL);
        ID3DDeviceContextState_Release(old_state);
    }
    if (state)
        ID3DDeviceContextState_Release(state);
    if (multithread)
    {
        ID3D11Multithread_Leave(multithread);
        ID3D11Multithread_Release(multithread);
    }
    if (d3d11_device_context1)
        ID3D11DeviceContext1_Release(d3d11_device_context1);
    if (d3d11_device_context)
        ID3D11DeviceContext_Release(d3d11_device_context);
    if (d3d11_device)
        ID3D11Device1_Release(d3d11_device);
    return hr;
}

/* Where a visual lands in the target window: the translation accumulated from the
 * root, and the clip accumulated from the root, both in window coordinates. */
struct composite_state
{
    float x, y;
    RECT clip;
};

static LONG clamp_coord(float value)
{
    if (value <= -1e9f) return -1000000000;
    if (value >= 1e9f) return 1000000000;
    return (LONG)floorf(value + 0.5f);
}

static void blit_visual_cache(HDC dst_dc, const struct composition_visual *visual,
                              const struct composite_state *state)
{
    LONG x = clamp_coord(state->x), y = clamp_coord(state->y);
    RECT rect;

    static const BLENDFUNCTION blend_func =
    {
      AC_SRC_OVER, /* BlendOp */
      0,           /* BlendFlag */
      255,         /* SourceConstantAlpha */
      AC_SRC_ALPHA /* AlphaFormat */
    };

    SetRect(&rect, x, y, x + visual->cache.width, y + visual->cache.height);
    if (!IntersectRect(&rect, &rect, &state->clip))
        return;

    GdiAlphaBlend(dst_dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                  visual->cache.dc, rect.left - x, rect.top - y, rect.right - rect.left,
                  rect.bottom - rect.top, blend_func);
}

struct shared_visual_info
{
    HANDLE mapping;                  /* the target rendered by the owning process */
    UINT width;
    UINT height;
};

static HRESULT shared_visual_get_info(HANDLE shared_visual_handle, struct shared_visual_info *info)
{
    NTSTATUS status;

    memset(info, 0, sizeof(*info));

    SERVER_START_REQ(dcomp_get_shared_visual_info)
    {
        req->handle = wine_server_obj_handle(shared_visual_handle);
        if (!(status = wine_server_call(req)))
        {
            info->mapping = wine_server_ptr_handle(reply->section);
            info->width = reply->width;
            info->height = reply->height;
        }
    }
    SERVER_END_REQ;

    return status ? E_FAIL : S_OK;
}

/* Composite the target that another process rendered for this shared visual. */
static void blit_shared_visual(HDC dst_dc, struct composition_visual *visual,
                               const struct composite_state *state, const struct shared_visual_info *info)
{
    static const BLENDFUNCTION blend_func = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    const struct shared_visual_header *header;
    BITMAPINFO bitmap_info;
    SIZE_T size, row;
    LONG x, y, i;
    RECT rect;
    UINT src_x, src_y;

    if (!info->mapping || !info->width || !info->height)
        return;

    if (!visual->shared_view)
    {
        size = sizeof(*header) + (SIZE_T)info->width * info->height * 4;
        if (!(visual->shared_view = MapViewOfFile(info->mapping, FILE_MAP_READ, 0, 0, size)))
        {
            ERR("Failed to map the shared visual target, error %lu.\n", GetLastError());
            return;
        }
        visual->shared_mapping = info->mapping;
        visual->shared_width = info->width;
        visual->shared_height = info->height;

        memset(&bitmap_info, 0, sizeof(bitmap_info));
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = info->width;
        bitmap_info.bmiHeader.biHeight = -((LONG)info->height);
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;
        visual->shared_dc = CreateCompatibleDC(NULL);
        visual->shared_bitmap = CreateDIBSection(visual->shared_dc, &bitmap_info, DIB_RGB_COLORS,
                                                &visual->shared_bits, NULL, 0);
        visual->shared_old_bitmap = SelectObject(visual->shared_dc, visual->shared_bitmap);
    }
    else
    {
        /* The handle was allocated for this call only; the mapping is already mapped. */
        CloseHandle(info->mapping);
    }

    if (!visual->shared_view || visual->shared_width != info->width || visual->shared_height != info->height)
        return;

    header = visual->shared_view;

    x = clamp_coord(state->x);
    y = clamp_coord(state->y);
    SetRect(&rect, x, y, x + (LONG)visual->shared_width, y + (LONG)visual->shared_height);
    if (!IntersectRect(&rect, &rect, &state->clip))
        return;

    src_x = rect.left - x;
    src_y = rect.top - y;
    row = (SIZE_T)(rect.right - rect.left) * 4;
    for (i = rect.top; i < rect.bottom; i++)
        memcpy((BYTE *)visual->shared_bits + (i - rect.top) * row,
               (const BYTE *)visual->shared_view + sizeof(*header)
                       + ((SIZE_T)(src_y + i - rect.top) * visual->shared_width + src_x) * 4, row);

    TRACE("Blitting shared visual %p, %ldx%ld at %ld,%ld.\n", visual, rect.right - rect.left,
          rect.bottom - rect.top, rect.left, rect.top);

    /* A frame rendered while the copy above was in flight is not retried: the next refresh
     * composites the whole target again. */
    GdiAlphaBlend(dst_dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                  visual->shared_dc, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                  blend_func);
}

/* Copy a window and its visible children into a DC at the given origin. A window DC leaves out
 * child windows, so the content of a composited window only shows up when they are drawn too. */
static void capture_window(HDC dst_dc, HWND hwnd, LONG x, LONG y)
{
    RECT rect, child_rect;
    HDC hdc;
    HWND child;

    if (!GetClientRect(hwnd, &rect) || rect.right <= 0 || rect.bottom <= 0)
        return;

    if ((hdc = GetDCEx(hwnd, 0, DCX_CACHE | DCX_USESTYLE | DCX_CLIPCHILDREN)))
    {
        BitBlt(dst_dc, x, y, rect.right, rect.bottom, hdc, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, hdc);
    }

    for (child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        if (!IsWindowVisible(child))
            continue;
        if (!GetWindowRect(child, &child_rect))
            continue;
        MapWindowPoints(NULL, hwnd, (POINT *)&child_rect, 2);
        capture_window(dst_dc, child, x + child_rect.left, y + child_rect.top);
    }
}

/* Read back the window that a surface created from an HWND shows. */
static HRESULT update_visual_cache_from_window(struct composition_visual *visual, HWND hwnd)
{
    RECT rect;

    if (!GetClientRect(hwnd, &rect))
        return E_FAIL;
    if (rect.right <= 0 || rect.bottom <= 0)
        return E_FAIL;
    if (!visual_resize_cache(visual, rect.right, rect.bottom))
        return E_OUTOFMEMORY;

    capture_window(visual->cache.dc, hwnd, 0, 0);
    return S_OK;
}

/* Window content is opaque: the captured DIB has no alpha channel, so blend it in with SRCCOPY. */
static void blit_visual_window(HDC dst_dc, const struct composition_visual *visual,
                               const struct composite_state *state)
{
    LONG x = clamp_coord(state->x), y = clamp_coord(state->y);
    RECT rect;

    SetRect(&rect, x, y, x + visual->cache.width, y + visual->cache.height);
    if (!IntersectRect(&rect, &rect, &state->clip))
        return;

    BitBlt(dst_dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
           visual->cache.dc, rect.left - x, rect.top - y, SRCCOPY);
}

static HRESULT do_composite(HDC dst_dc, struct composition_visual *visual,
                            const struct composite_state *parent_state)
{
    struct composition_visual *child_visual;
    struct composite_state state = *parent_state;
    D2D_MATRIX_3X2_F transform;
    IDXGISurface *dxgi_surface = NULL;
    UINT content_version = 0;
    HRESULT hr;

    /* The visual's transform is applied before its offset; only translations are supported. */
    transform = visual->transform;
    if (visual->transform_object)
        composition_transform_get_matrix(visual->transform_object, &transform);
    state.x += visual->offset_x + transform._31;
    state.y += visual->offset_y + transform._32;
    if (visual->has_clip)
    {
        RECT clip;

        SetRect(&clip, clamp_coord(state.x + visual->clip.left), clamp_coord(state.y + visual->clip.top),
                clamp_coord(state.x + visual->clip.right), clamp_coord(state.y + visual->clip.bottom));
        if (!IntersectRect(&state.clip, &state.clip, &clip))
            return S_OK;
    }

    if (visual->shared_visual_handle)
    {
        struct shared_visual_info info;

        /* The target that renders into the shared visual belongs to the process that created
         * it; only the rendered mapping is valid across processes. */
        if (SUCCEEDED(shared_visual_get_info(visual->shared_visual_handle, &info)))
            blit_shared_visual(dst_dc, visual, &state, &info);
    }

    /* Render content */
    if (visual->content)
    {
        if (IsEqualGUID(&visual->content_iid, &IID_IDXGISwapChain1))
        {
            DXGI_SWAP_CHAIN_DESC swapchain_desc;
            IDXGISwapChain1 *swapchain;

            hr = IUnknown_QueryInterface(visual->content, &IID_IDXGISwapChain1, (void **)&swapchain);
            if (FAILED(hr))
            {
                FIXME("Failed to query IDXGISwapChain1.\n");
                return hr;
            }

            hr = IDXGISwapChain1_GetDesc(swapchain, &swapchain_desc);
            if (FAILED(hr))
            {
                ERR("Failed to get the swapchain description, hr %#lx.\n", hr);
                IDXGISwapChain1_Release(swapchain);
                return hr;
            }

            /* Only a Present() changes what the swapchain shows. */
            if (FAILED(IDXGISwapChain1_GetLastPresentCount(swapchain, &content_version)))
                content_version = ~0u;

            /* The front buffer is the last back buffer because the composition swapchain must
                     * be created with DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL */
            if (!visual->cache.content || visual->cache.content != visual->content
                    || visual->cache.content_version != content_version || content_version == ~0u)
            {
                hr = IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_IDXGISurface,
                                               (void **)&dxgi_surface);
                if (FAILED(hr))
                {
                    ERR("Failed to get the swapchain front buffer, hr %#lx.\n", hr);
                    IDXGISwapChain1_Release(swapchain);
                    return hr;
                }
            }
            IDXGISwapChain1_Release(swapchain);
        }
        else if (IsEqualGUID(&visual->content_iid, &IID_IDCompositionSurface))
        {
            struct composition_surface *surface_impl;
            IDCompositionSurface *surface;

            hr = IUnknown_QueryInterface(visual->content, &IID_IDCompositionSurface, (void **)&surface);
            if (FAILED(hr))
            {
                FIXME("Failed to query IDCompositionSurface.\n");
                return hr;
            }

            surface_impl = impl_from_IDCompositionSurface(surface);
            if (surface_impl->hwnd)
            {
                HWND hwnd = surface_impl->hwnd;

                IDCompositionSurface_Release(surface);
                if (FAILED(hr = update_visual_cache_from_window(visual, hwnd)))
                    return hr;
                blit_visual_window(dst_dc, visual, &state);
                goto composite_children;
            }
            if (IsEqualGUID(&surface_impl->physical_surface_iid, &IID_IDXGISurface))
            {
                /* Only EndDraw() changes what the surface shows. */
                content_version = ReadAcquire(&surface_impl->draw_count);
                if (!visual->cache.content || visual->cache.content != visual->content
                        || visual->cache.content_version != content_version)
                {
                    dxgi_surface = (IDXGISurface *)surface_impl->physical_surface;
                    IDXGISurface_AddRef(dxgi_surface);
                }
                IDCompositionSurface_Release(surface);
            }
            else
            {
                FIXME("IDCompositionSurface physical_surface_iid %s is unsupported.\n",
                      wine_dbgstr_guid(&surface_impl->physical_surface_iid));
                IDCompositionSurface_Release(surface);
                return E_FAIL;
            }
        }
        else
        {
            FIXME("content_iid %s is unsupported.\n", wine_dbgstr_guid(&visual->content_iid));
            return E_FAIL;
        }

        if (dxgi_surface)
        {
            /* Reading the surface back holds the rendering device's lock; skip it when
             * the content did not change and repaint the window from the cache. */
            if (SUCCEEDED(update_visual_cache(visual, dxgi_surface)))
            {
                visual->cache.content = visual->content;
                visual->cache.content_version = content_version;
            }
            else
            {
                visual->cache.content = NULL;
            }
            IDXGISurface_Release(dxgi_surface);
        }
        if (visual->cache.content)
            blit_visual_cache(dst_dc, visual, &state);
    }

composite_children:
    LIST_FOR_EACH_ENTRY(child_visual, &visual->child_visuals, struct composition_visual, entry)
    {
        do_composite(dst_dc, child_visual, &state);
    }

    return S_OK;
}


/* The rendered target of a shared visual is published to the other processes through the
 * server, so that a visual created from the same handle can composite it there. */
#define SHARED_VISUAL_WIDTH  1280
#define SHARED_VISUAL_HEIGHT 1024

static void shared_visual_target_set_info(struct composition_target *target, UINT width, UINT height)
{
    NTSTATUS status;

    SERVER_START_REQ(dcomp_set_shared_visual_info)
    {
        req->handle = wine_server_obj_handle(target->shared_visual_handle);
        req->section = wine_server_obj_handle(target->shared_mapping);
        req->width = width;
        req->height = height;
        status = wine_server_call(req);
    }
    SERVER_END_REQ;

    if (status)
        ERR("dcomp_set_shared_visual_info failed, status %#lx.\n", status);
}

static BOOL shared_target_create_surface(struct composition_target *target)
{
    struct shared_visual_header *header;
    BITMAPINFO bitmap_info;
    SIZE_T size;

    size = sizeof(*header) + (SIZE_T)SHARED_VISUAL_WIDTH * SHARED_VISUAL_HEIGHT * 4;

    if (!(target->shared_mapping = CreateFileMappingW(NULL, NULL, PAGE_READWRITE, 0, size, NULL)))
    {
        ERR("Failed to create the shared visual mapping, error %lu.\n", GetLastError());
        return FALSE;
    }
    if (!(target->shared_view = MapViewOfFile(target->shared_mapping, FILE_MAP_ALL_ACCESS, 0, 0, size)))
    {
        ERR("Failed to map the shared visual mapping, error %lu.\n", GetLastError());
        CloseHandle(target->shared_mapping);
        target->shared_mapping = NULL;
        return FALSE;
    }
    if (!(target->shared_dc = CreateCompatibleDC(NULL)))
        goto failed;

    memset(&bitmap_info, 0, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = SHARED_VISUAL_WIDTH;
    bitmap_info.bmiHeader.biHeight = -SHARED_VISUAL_HEIGHT;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    if (!(target->shared_bitmap = CreateDIBSection(target->shared_dc, &bitmap_info, DIB_RGB_COLORS,
                                                  &target->shared_bits, NULL, 0)))
        goto failed;
    target->shared_old_bitmap = SelectObject(target->shared_dc, target->shared_bitmap);

    header = target->shared_view;
    header->seq = 0;
    header->width = SHARED_VISUAL_WIDTH;
    header->height = SHARED_VISUAL_HEIGHT;

    shared_visual_target_set_info(target, SHARED_VISUAL_WIDTH, SHARED_VISUAL_HEIGHT);
    return TRUE;

failed:
    ERR("Failed to create the shared visual surface, error %lu.\n", GetLastError());
    shared_visual_target_free(target);
    return FALSE;
}

static BOOL present_shared_target(struct composition_target *target)
{
    const struct shared_visual_header *header;
    struct composite_state state;
    SIZE_T pixels;

    TRACE("Rendering shared target %p, root %p.\n", target, target->root);
    if (!target->root)
        return FALSE;
    if (!target->shared_view && !shared_target_create_surface(target))
        return FALSE;

    pixels = (SIZE_T)SHARED_VISUAL_WIDTH * SHARED_VISUAL_HEIGHT * 4;
    memset(target->shared_bits, 0, pixels);
    state.x = state.y = 0.0f;
    SetRect(&state.clip, 0, 0, SHARED_VISUAL_WIDTH, SHARED_VISUAL_HEIGHT);
    if (FAILED(do_composite(target->shared_dc, impl_from_IDCompositionVisual(target->root), &state)))
        return FALSE;

    header = target->shared_view;
    memcpy((BYTE *)target->shared_view + sizeof(*header), target->shared_bits, pixels);
    TRACE("Presented shared target %p, frame %u.\n", target, header->seq + 1);
    ((struct shared_visual_header *)header)->seq++;
    return TRUE;
}

void shared_visual_target_free(struct composition_target *target)
{
    if (target->shared_dc)
    {
        if (target->shared_old_bitmap)
            SelectObject(target->shared_dc, target->shared_old_bitmap);
        if (target->shared_bitmap)
            DeleteObject(target->shared_bitmap);
        DeleteDC(target->shared_dc);
        target->shared_dc = NULL;
    }
    if (target->shared_view)
    {
        UnmapViewOfFile(target->shared_view);
        target->shared_view = NULL;
    }
    if (target->shared_mapping)
    {
        CloseHandle(target->shared_mapping);
        target->shared_mapping = NULL;
    }
}

void shared_visual_cache_free(struct composition_visual *visual)
{
    if (visual->shared_dc)
    {
        if (visual->shared_old_bitmap)
            SelectObject(visual->shared_dc, visual->shared_old_bitmap);
        if (visual->shared_bitmap)
            DeleteObject(visual->shared_bitmap);
        DeleteDC(visual->shared_dc);
        visual->shared_dc = NULL;
    }
    if (visual->shared_view)
    {
        UnmapViewOfFile(visual->shared_view);
        visual->shared_view = NULL;
    }
    if (visual->shared_mapping)
    {
        CloseHandle(visual->shared_mapping);
        visual->shared_mapping = NULL;
    }
}

static DWORD WINAPI composite_thread_proc(void *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    unsigned int count, frequency, refresh_period;
    struct composition_target *target;
    struct composition_visual *visual;
    struct composite_state state;
    HRESULT hr;
    DWORD style;
    HDC hdc;

    /* TODO: Implement and use D3DKMTWaitForVerticalBlankEvent() */
    hdc = GetDC(0);
    frequency = GetDeviceCaps(hdc, VREFRESH);
    refresh_period = 1000 / frequency;
    ReleaseDC(0, hdc);

    while (TRUE)
    {
        dcomp_lock();

        count = 0;
        LIST_FOR_EACH_ENTRY(target, &device->targets, struct composition_target, entry)
        {
            /* Targets created from a shared visual handle have no window; they are rendered into
             * the mapping from which the matching visual composites them in the other process. */
            /* A shared target is driven by the compositor of its own process, which does not
             * necessarily commit: it must keep its producer thread alive on its own. */
            if (target->shared_visual_handle)
            {
                TRACE("Shared target %p, root %p.\n", target, target->root);
                present_shared_target(target);
                count++;
                continue;
            }

            if (!target->root)
            {
                FIXME("Target %p has no root.\n", &target->IDCompositionTarget_iface);
                continue;
            }

            visual = impl_from_IDCompositionVisual(target->root);
            state.x = state.y = 0.0f;
            GetClientRect(target->hwnd, &state.clip);

            style = DCX_USESTYLE | DCX_CACHE;
            if (!target->topmost)
                style |= DCX_CLIPCHILDREN;
            if (!(hdc = GetDCEx(target->hwnd, 0, style)))
            {
                WARN("GetDCEx(%p, %#lx) failed, error %lu.\n", target->hwnd, style, GetLastError());
                continue;
            }

            hr = do_composite(hdc, visual, &state);
            if (SUCCEEDED(hr))
                count++;
            else
                WARN("do_composite(%p) failed, hr %#lx.\n", target->hwnd, hr);
            ReleaseDC(target->hwnd, hdc);
        }

        if (!count)
        {
            TRACE("Composition thread exited.\n");
            device->thread_exited = TRUE;
            dcomp_unlock();
            break;
        }

        dcomp_unlock();

        if (device->exit_thread)
            break;

        Sleep(refresh_period);
    }

    return 0;
}

/* Start the composite thread of a device unless it is already running. Called from Commit() and
 * from the shared visual paths, whose producer never commits its device. */
void composite_thread_start(struct composition_device *device)
{
    dcomp_lock();
    TRACE("Starting composition thread for device %p, thread %p, exited %u.\n", device,
          device->thread, device->thread_exited);

    if (!device->thread || device->thread_exited)
    {
        if (device->thread)
        {
            device->exit_thread = TRUE;
            WaitForSingleObject(device->thread, INFINITE);
            CloseHandle(device->thread);
        }
        device->thread_exited = FALSE;
        device->exit_thread = FALSE;
        device->thread = CreateThread(NULL, 0, composite_thread_proc, device, 0, NULL);
    }

    dcomp_unlock();
}

static HRESULT STDMETHODCALLTYPE device_Commit(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    FIXME("iface %p semi-stub!\n", iface);

    composite_thread_start(device);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_WaitForCommitCompletion(IDCompositionDevice *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_GetFrameStatistics(IDCompositionDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTargetForHwnd(IDCompositionDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p\n", iface, hwnd, topmost, target);

    return create_target(device, hwnd, topmost, target);
}

static HRESULT STDMETHODCALLTYPE device_CreateVisual(IDCompositionDevice *iface,
        IDCompositionVisual **visual)
{
    TRACE("iface %p, visual %p\n", iface, visual);

    return create_visual(1, &IID_IDCompositionVisual, (void **)visual);
}

static HRESULT STDMETHODCALLTYPE device_CreateSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateVirtualSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHandle(IDCompositionDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHwnd(IDCompositionDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    IDCompositionSurface *dcomp_surface;
    HRESULT hr;

    TRACE("iface %p, hwnd %p, surface %p.\n", iface, hwnd, surface);

    if (!surface)
        return E_INVALIDARG;
    *surface = NULL;

    if (FAILED(hr = create_surface_from_hwnd(hwnd, &dcomp_surface)))
        return hr;

    *surface = (IUnknown *)dcomp_surface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_CreateTranslateTransform(IDCompositionDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    TRACE("iface %p, transform %p.\n", iface, transform);

    if (!transform)
        return E_INVALIDARG;

    return composition_transform_create_translate(transform);
}

static HRESULT STDMETHODCALLTYPE device_CreateScaleTransform(IDCompositionDevice *iface,
        IDCompositionScaleTransform **transform)
{
    TRACE("iface %p, transform %p.\n", iface, transform);

    if (!transform)
        return E_INVALIDARG;

    return composition_transform_create_scale(transform);
}

static HRESULT STDMETHODCALLTYPE device_CreateRotateTransform(IDCompositionDevice *iface,
        IDCompositionRotateTransform **transform)
{
    TRACE("iface %p, transform %p.\n", iface, transform);

    if (!transform)
        return E_INVALIDARG;

    return composition_transform_create_rotate(transform);
}

static HRESULT STDMETHODCALLTYPE device_CreateSkewTransform(IDCompositionDevice *iface,
        IDCompositionSkewTransform **transform)
{
    TRACE("iface %p, transform %p.\n", iface, transform);

    if (!transform)
        return E_INVALIDARG;

    return composition_transform_create_skew(transform);
}

static HRESULT STDMETHODCALLTYPE device_CreateMatrixTransform(IDCompositionDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    TRACE("iface %p, transform %p.\n", iface, transform);

    if (!transform)
        return E_INVALIDARG;

    return composition_transform_create_matrix(transform);
}

static HRESULT STDMETHODCALLTYPE device_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT elements,
        IDCompositionTransform **transform_group)
{
    TRACE("iface %p, transforms %p, elements %u, transform_group %p.\n", iface, transforms,
            elements, transform_group);

    if (!transform_group)
        return E_INVALIDARG;

    return composition_transform_create_group(transforms, elements, transform_group);
}

static HRESULT STDMETHODCALLTYPE device_CreateTranslateTransform3D(IDCompositionDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, translate_transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateScaleTransform3D(IDCompositionDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRotateTransform3D(IDCompositionDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateMatrixTransform3D(IDCompositionDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", iface,
            transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateAnimation(IDCompositionDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CheckDeviceState(IDCompositionDevice *iface,
        BOOL *valid)
{
    FIXME("iface %p, valid %p stub!\n", iface, valid);
    return E_NOTIMPL;
}

static const struct IDCompositionDeviceVtbl device_vtbl =
{
    /* IUnknown methods */
    device_QueryInterface,
    device_AddRef,
    device_Release,
    /* IDCompositionDevice methods */
    device_Commit,
    device_WaitForCommitCompletion,
    device_GetFrameStatistics,
    device_CreateTargetForHwnd,
    device_CreateVisual,
    device_CreateSurface,
    device_CreateVirtualSurface,
    device_CreateSurfaceFromHandle,
    device_CreateSurfaceFromHwnd,
    device_CreateTranslateTransform,
    device_CreateScaleTransform,
    device_CreateRotateTransform,
    device_CreateSkewTransform,
    device_CreateMatrixTransform,
    device_CreateTransformGroup,
    device_CreateTranslateTransform3D,
    device_CreateScaleTransform3D,
    device_CreateRotateTransform3D,
    device_CreateMatrixTransform3D,
    device_CreateTransform3DGroup,
    device_CreateEffectGroup,
    device_CreateRectangleClip,
    device_CreateAnimation,
    device_CheckDeviceState,
};

static HRESULT STDMETHODCALLTYPE desktop_device_QueryInterface(IDCompositionDeviceUnknown *iface,
        REFIID iid, void **out)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE desktop_device_AddRef(IDCompositionDeviceUnknown *iface)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE desktop_device_Release(IDCompositionDeviceUnknown *iface)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_Commit(IDCompositionDeviceUnknown *iface)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_Commit(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_WaitForCommitCompletion(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_GetFrameStatistics(IDCompositionDeviceUnknown *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateVisual(IDCompositionDeviceUnknown *iface,
        IDCompositionVisual2 **visual)
{
    TRACE("iface %p, visual %p\n", iface, visual);

    return create_visual(2, &IID_IDCompositionVisual2, (void **)visual);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFactory(IDCompositionDeviceUnknown *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p, rendering_device %p, surface_factory %p.\n", iface, rendering_device, surface_factory);

    return create_surface_factory(device, rendering_device, surface_factory);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurface(IDCompositionDeviceUnknown *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateVirtualSurface(IDCompositionDeviceUnknown *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTranslateTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateScaleTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRotateTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSkewTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateMatrixTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTransformGroup(IDCompositionDeviceUnknown *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n", iface, transforms,
            elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTranslateTransform3D(IDCompositionDeviceUnknown *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, translate_transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateScaleTransform3D(IDCompositionDeviceUnknown *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRotateTransform3D(IDCompositionDeviceUnknown *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateMatrixTransform3D(IDCompositionDeviceUnknown *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTransform3DGroup(IDCompositionDeviceUnknown *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", iface,
            transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateEffectGroup(IDCompositionDeviceUnknown *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRectangleClip(IDCompositionDeviceUnknown *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateAnimation(IDCompositionDeviceUnknown *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTargetForHwnd(IDCompositionDeviceUnknown *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p\n", iface, hwnd, topmost, target);

    return IDCompositionDevice_CreateTargetForHwnd(&device->IDCompositionDevice_iface, hwnd,
            topmost, target);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFromHandle(IDCompositionDeviceUnknown *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFromHwnd(IDCompositionDeviceUnknown *iface,
        HWND hwnd, IUnknown **surface)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p, hwnd %p, surface %p.\n", iface, hwnd, surface);

    return IDCompositionDevice_CreateSurfaceFromHwnd(&device->IDCompositionDevice_iface, hwnd,
            surface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSharedResource(IDCompositionDeviceUnknown *iface,
        REFIID riid, void **obj)
{
    HANDLE handle;
    HRESULT hr;

    TRACE("iface %p, riid %s, obj %p\n", iface, debugstr_guid(riid), obj);

    if (!obj)
        return E_POINTER;
    *obj = NULL;

    if (!IsEqualGUID(riid, &IID_IDCompositionVisual))
    {
        FIXME("Unsupported resource iid %s.\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    if (FAILED(hr = DCompositionCreateSharedVisualHandle(&handle)))
        return hr;

    hr = create_visual_from_shared_visual_handle(handle, obj);
    CloseHandle(handle);
    return hr;
}

static HRESULT STDMETHODCALLTYPE desktop_device_OpenSharedResourceHandle(IDCompositionDeviceUnknown *iface,
        IUnknown *unknown, HANDLE *handle)
{
    IDCompositionVisual *visual_iface;
    struct composition_visual *visual;
    HANDLE duplicated;

    TRACE("iface %p, unknown %p, handle %p\n", iface, unknown, handle);

    if (!unknown || !handle)
        return E_POINTER;

    if (FAILED(IUnknown_QueryInterface(unknown, &IID_IDCompositionVisual, (void **)&visual_iface)))
        return E_NOINTERFACE;
    visual = impl_from_IDCompositionVisual(visual_iface);

    if (!visual->shared_visual_handle)
    {
        IDCompositionVisual_Release(visual_iface);
        WARN("Visual %p was not created from a shared resource.\n", unknown);
        return E_INVALIDARG;
    }

    if (!DuplicateHandle(GetCurrentProcess(), visual->shared_visual_handle, GetCurrentProcess(),
                         &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        ERR("Cannot duplicate handle, last error %lu.\n", GetLastError());
        IDCompositionVisual_Release(visual_iface);
        return E_FAIL;
    }

    *handle = duplicated;
    IDCompositionVisual_Release(visual_iface);
    return S_OK;
}


static HRESULT STDMETHODCALLTYPE desktop_device_CreateFromSharedVisualHandle(IDCompositionDeviceUnknown *iface,
     HANDLE visual_handle, REFIID iid, void **out)
{
    TRACE("iface %p, visual_handle %p, iid %s, out %p.\n", iface, visual_handle, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;
    *out = NULL;

    if (IsEqualGUID(iid, &IID_IDCompositionVisual))
        return create_visual_from_shared_visual_handle(visual_handle, out);
    if (IsEqualGUID(iid, &IID_IDCompositionTarget))
        return create_target_from_shared_visual_handle(impl_from_IDCompositionDeviceUnknown(iface),
                                                       visual_handle, out);

    FIXME("Unsupported iid %s.\n", debugstr_guid(iid));
    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown1(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown2(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown3(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown4(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown5(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown6(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown7(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown8(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown9(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown10(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown11(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown12(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown13(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown14(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown15(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown16(IDCompositionDeviceUnknown *iface, void **parameter1)
{
    FIXME("iface %p parameter1 %p stub!\n", iface, parameter1);

    if (!parameter1)
        return E_INVALIDARG;

    *parameter1 = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown17(IDCompositionDeviceUnknown *iface, void **parameter1)
{
    FIXME("iface %p parameter1 %p stub!\n", iface, parameter1);

    if (!parameter1)
        return E_INVALIDARG;

    *parameter1 = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown18(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown19(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown20(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown21(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown22(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown23(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown24(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown25(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown26(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown27(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown28(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown29(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown30(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown31(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown32(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown33(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown34(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown35(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown36(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown37(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown38(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown39(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown40(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown41(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown42(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown43(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown44(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown45(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown46(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown47(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown48(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown49(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown50(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown51(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown52(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown53(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown54(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown55(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown56(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown57(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown58(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown59(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown60(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown61(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown62(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown63(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown64(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown65(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown66(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown67(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown68(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown69(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown70(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown71(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown72(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown73(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown74(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown75(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown76(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown77(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown78(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown79(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown80(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown81(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown82(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown83(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown84(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown85(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown86(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown87(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown88(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown89(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown90(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown91(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown92(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown93(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown94(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown95(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown96(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown97(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown98(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown99(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static const struct IDCompositionDeviceUnknownVtbl desktop_device_vtbl =
{
    /* IUnknown methods */
    desktop_device_QueryInterface,
    desktop_device_AddRef,
    desktop_device_Release,
    /* IDCompositionDevice2 methods */
    desktop_device_Commit,
    desktop_device_WaitForCommitCompletion,
    desktop_device_GetFrameStatistics,
    desktop_device_CreateVisual,
    desktop_device_CreateSurfaceFactory,
    desktop_device_CreateSurface,
    desktop_device_CreateVirtualSurface,
    desktop_device_CreateTranslateTransform,
    desktop_device_CreateScaleTransform,
    desktop_device_CreateRotateTransform,
    desktop_device_CreateSkewTransform,
    desktop_device_CreateMatrixTransform,
    desktop_device_CreateTransformGroup,
    desktop_device_CreateTranslateTransform3D,
    desktop_device_CreateScaleTransform3D,
    desktop_device_CreateRotateTransform3D,
    desktop_device_CreateMatrixTransform3D,
    desktop_device_CreateTransform3DGroup,
    desktop_device_CreateEffectGroup,
    desktop_device_CreateRectangleClip,
    desktop_device_CreateAnimation,
    /* IDCompositionDesktopDevice methods */
    desktop_device_CreateTargetForHwnd,
    desktop_device_CreateSurfaceFromHandle,
    desktop_device_CreateSurfaceFromHwnd,
    /* IDCompositionDesktopDevicePartner methods */
    desktop_device_CreateSharedResource,
    desktop_device_OpenSharedResourceHandle,
    desktop_device_CreateFromSharedVisualHandle,
    /* IDCompositionDeviceUnknown methods */
    desktop_device_Unknown1,
    desktop_device_Unknown2,
    desktop_device_Unknown3,
    desktop_device_Unknown4,
    desktop_device_Unknown5,
    desktop_device_Unknown6,
    desktop_device_Unknown7,
    desktop_device_Unknown8,
    desktop_device_Unknown9,
    desktop_device_Unknown10,
    desktop_device_Unknown11,
    desktop_device_Unknown12,
    desktop_device_Unknown13,
    desktop_device_Unknown14,
    desktop_device_Unknown15,
    desktop_device_Unknown16,
    desktop_device_Unknown17,
    desktop_device_Unknown18,
    desktop_device_Unknown19,
    desktop_device_Unknown20,
    desktop_device_Unknown21,
    desktop_device_Unknown22,
    desktop_device_Unknown23,
    desktop_device_Unknown24,
    desktop_device_Unknown25,
    desktop_device_Unknown26,
    desktop_device_Unknown27,
    desktop_device_Unknown28,
    desktop_device_Unknown29,
    desktop_device_Unknown30,
    desktop_device_Unknown31,
    desktop_device_Unknown32,
    desktop_device_Unknown33,
    desktop_device_Unknown34,
    desktop_device_Unknown35,
    desktop_device_Unknown36,
    desktop_device_Unknown37,
    desktop_device_Unknown38,
    desktop_device_Unknown39,
    desktop_device_Unknown40,
    desktop_device_Unknown41,
    desktop_device_Unknown42,
    desktop_device_Unknown43,
    desktop_device_Unknown44,
    desktop_device_Unknown45,
    desktop_device_Unknown46,
    desktop_device_Unknown47,
    desktop_device_Unknown48,
    desktop_device_Unknown49,
    desktop_device_Unknown50,
    desktop_device_Unknown51,
    desktop_device_Unknown52,
    desktop_device_Unknown53,
    desktop_device_Unknown54,
    desktop_device_Unknown55,
    desktop_device_Unknown56,
    desktop_device_Unknown57,
    desktop_device_Unknown58,
    desktop_device_Unknown59,
    desktop_device_Unknown60,
    desktop_device_Unknown61,
    desktop_device_Unknown62,
    desktop_device_Unknown63,
    desktop_device_Unknown64,
    desktop_device_Unknown65,
    desktop_device_Unknown66,
    desktop_device_Unknown67,
    desktop_device_Unknown68,
    desktop_device_Unknown69,
    desktop_device_Unknown70,
    desktop_device_Unknown71,
    desktop_device_Unknown72,
    desktop_device_Unknown73,
    desktop_device_Unknown74,
    desktop_device_Unknown75,
    desktop_device_Unknown76,
    desktop_device_Unknown77,
    desktop_device_Unknown78,
    desktop_device_Unknown79,
    desktop_device_Unknown80,
    desktop_device_Unknown81,
    desktop_device_Unknown82,
    desktop_device_Unknown83,
    desktop_device_Unknown84,
    desktop_device_Unknown85,
    desktop_device_Unknown86,
    desktop_device_Unknown87,
    desktop_device_Unknown88,
    desktop_device_Unknown89,
    desktop_device_Unknown90,
    desktop_device_Unknown91,
    desktop_device_Unknown92,
    desktop_device_Unknown93,
    desktop_device_Unknown94,
    desktop_device_Unknown95,
    desktop_device_Unknown96,
    desktop_device_Unknown97,
    desktop_device_Unknown98,
    desktop_device_Unknown99,
};

static HRESULT create_device(int version, REFIID iid, void **out)
{
    struct composition_device *device;
    HRESULT hr;

    if (!out)
        return E_INVALIDARG;

    device = calloc(1, sizeof(*device));
    if (!device)
        return E_OUTOFMEMORY;

    list_init(&device->targets);
    device->IDCompositionDevice_iface.lpVtbl = &device_vtbl;
    device->IDCompositionDeviceUnknown_iface.lpVtbl = &desktop_device_vtbl;
    device->version = version;
    device->ref = 1;
    hr = IDCompositionDevice_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
    IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
    return hr;
}

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p\n", dxgi_device, debugstr_guid(iid), device);

    if (!IsEqualIID(iid, &IID_IDCompositionDevice))
        return E_NOINTERFACE;

    return create_device(1, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p\n", rendering_device, debugstr_guid(iid), device);

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    return create_device(2, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    FIXME("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    return create_device(3, iid, device);
}

HRESULT WINAPI DCompositionCreateSharedVisualHandle(HANDLE *handle)
{
    NTSTATUS status;

    FIXME("handle %p stub!\n", handle);

    if (!handle)
        return STATUS_INVALID_PARAMETER;

    SERVER_START_REQ( dcomp_create_shared_visual )
    {
        if (!(status = wine_server_call( req )))
            *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    return (HRESULT)status;
}
