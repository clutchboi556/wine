/*
 * Copyright (C) 2022 Mohamad Al-Jaf
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

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winioctl.h"
#include "ddk/usb100.h"
#include "winusb.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winusb);

/* Private interface to wineusb.sys, which answers these on the device itself;
 * there is no winusb.sys function driver. Keep in sync with wineusb.c. */
#define IOCTL_WINEUSB_GET_INFO       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WINEUSB_GET_DESCRIPTOR CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WINEUSB_WRITE_PIPE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

struct wineusb_info
{
    LONG interface_index;   /* -1 for a whole device */
};

struct wineusb_get_descriptor
{
    UCHAR type, index;
    USHORT langid;
};

struct winusb_interface
{
    HANDLE file;
    BYTE *config;
    DWORD config_size;
    const USB_INTERFACE_DESCRIPTOR *desc;
};

/* The file is normally opened for overlapped I/O, so always pass an
 * OVERLAPPED and wait: that is correct for a synchronous handle too. */
static BOOL sync_ioctl( HANDLE file, DWORD code, void *in, DWORD in_size, void *out, DWORD out_size, DWORD *ret_size )
{
    OVERLAPPED ovl = {0};
    DWORD size = 0;
    HANDLE event;
    BOOL ret;

    if (!(event = CreateEventW( NULL, TRUE, FALSE, NULL ))) return FALSE;
    /* the low bit keeps a completion port, if any, from seeing our requests */
    ovl.hEvent = (HANDLE)((ULONG_PTR)event | 1);

    ret = DeviceIoControl( file, code, in, in_size, out, out_size, &size, &ovl );
    if (!ret && GetLastError() == ERROR_IO_PENDING)
        ret = GetOverlappedResult( file, &ovl, &size, TRUE );

    CloseHandle( event );
    if (ret_size) *ret_size = size;
    return ret;
}

static const USB_INTERFACE_DESCRIPTOR *find_interface( const BYTE *config, DWORD size, LONG index )
{
    DWORD pos = 0;

    while (pos + 2 <= size && config[pos] >= 2 && pos + config[pos] <= size)
    {
        const USB_INTERFACE_DESCRIPTOR *desc = (const USB_INTERFACE_DESCRIPTOR *)(config + pos);

        if (desc->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE && desc->bLength >= sizeof(*desc)
            && !desc->bAlternateSetting && (index < 0 || desc->bInterfaceNumber == index))
            return desc;
        pos += config[pos];
    }
    return NULL;
}

/***********************************************************************
 *           WinUsb_Initialize (winusb.@)
 */
BOOL WINAPI WinUsb_Initialize( HANDLE file, WINUSB_INTERFACE_HANDLE *handle )
{
    struct wineusb_get_descriptor params = {.type = USB_CONFIGURATION_DESCRIPTOR_TYPE};
    USB_CONFIGURATION_DESCRIPTOR header;
    struct winusb_interface *iface;
    struct wineusb_info info;
    DWORD size;

    TRACE( "file %p, handle %p.\n", file, handle );

    if (!handle)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    if (!sync_ioctl( file, IOCTL_WINEUSB_GET_INFO, NULL, 0, &info, sizeof(info), &size ) || size < sizeof(info))
    {
        WARN( "Not a wineusb device, error %lu.\n", GetLastError() );
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }

    if (!sync_ioctl( file, IOCTL_WINEUSB_GET_DESCRIPTOR, &params, sizeof(params), &header, sizeof(header), &size )
        || size < sizeof(header))
    {
        SetLastError( ERROR_GEN_FAILURE );
        return FALSE;
    }

    if (!(iface = calloc( 1, sizeof(*iface) ))) goto oom;
    if (!(iface->config = malloc( header.wTotalLength ))) goto oom;

    if (!sync_ioctl( file, IOCTL_WINEUSB_GET_DESCRIPTOR, &params, sizeof(params), iface->config,
                     header.wTotalLength, &iface->config_size )
        || !(iface->desc = find_interface( iface->config, iface->config_size, info.interface_index )))
    {
        free( iface->config );
        free( iface );
        SetLastError( ERROR_GEN_FAILURE );
        return FALSE;
    }

    iface->file = file;
    *handle = iface;
    return TRUE;

oom:
    free( iface );
    SetLastError( ERROR_NOT_ENOUGH_MEMORY );
    return FALSE;
}

/***********************************************************************
 *           WinUsb_Free (winusb.@)
 */
BOOL WINAPI WinUsb_Free( WINUSB_INTERFACE_HANDLE handle )
{
    struct winusb_interface *iface = handle;

    TRACE( "handle %p.\n", handle );

    if (!iface) return TRUE;
    free( iface->config );
    free( iface );
    return TRUE;
}

/***********************************************************************
 *           WinUsb_QueryDeviceInformation (winusb.@)
 */
BOOL WINAPI WinUsb_QueryDeviceInformation( WINUSB_INTERFACE_HANDLE handle, ULONG type, ULONG *size, void *buffer )
{
    TRACE( "handle %p, type %#lx, size %p, buffer %p.\n", handle, type, size, buffer );

    if (type != DEVICE_SPEED || !size || !buffer || *size < sizeof(UCHAR))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    /* wineusb does not report the speed; WinUSB itself says "low" for
     * anything below high speed. */
    FIXME( "Returning LowSpeed.\n" );
    *(UCHAR *)buffer = LowSpeed;
    *size = sizeof(UCHAR);
    return TRUE;
}

/***********************************************************************
 *           WinUsb_QueryInterfaceSettings (winusb.@)
 */
BOOL WINAPI WinUsb_QueryInterfaceSettings( WINUSB_INTERFACE_HANDLE handle, UCHAR alternate,
                                           USB_INTERFACE_DESCRIPTOR *desc )
{
    struct winusb_interface *iface = handle;

    TRACE( "handle %p, alternate %u, desc %p.\n", handle, alternate, desc );

    if (!iface || !desc)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    if (alternate)
    {
        SetLastError( ERROR_NO_MORE_ITEMS );
        return FALSE;
    }
    *desc = *iface->desc;
    return TRUE;
}

/***********************************************************************
 *           WinUsb_QueryPipe (winusb.@)
 */
BOOL WINAPI WinUsb_QueryPipe( WINUSB_INTERFACE_HANDLE handle, UCHAR alternate, UCHAR index,
                              WINUSB_PIPE_INFORMATION *info )
{
    struct winusb_interface *iface = handle;
    const BYTE *end, *ptr;

    TRACE( "handle %p, alternate %u, index %u, info %p.\n", handle, alternate, index, info );

    if (!iface || !info)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    if (alternate || index >= iface->desc->bNumEndpoints)
    {
        SetLastError( ERROR_NO_MORE_ITEMS );
        return FALSE;
    }

    end = iface->config + iface->config_size;
    ptr = (const BYTE *)iface->desc + iface->desc->bLength;
    while (ptr + 2 <= end && ptr[0] >= 2 && ptr + ptr[0] <= end && ptr[1] != USB_INTERFACE_DESCRIPTOR_TYPE)
    {
        const USB_ENDPOINT_DESCRIPTOR *ep = (const USB_ENDPOINT_DESCRIPTOR *)ptr;

        if (ep->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE && ep->bLength >= sizeof(*ep) && !index--)
        {
            info->PipeType = ep->bmAttributes & USB_ENDPOINT_TYPE_MASK;  /* same values as USBD_PIPE_TYPE */
            info->PipeId = ep->bEndpointAddress;
            info->MaximumPacketSize = ep->wMaxPacketSize;
            info->Interval = ep->bInterval;
            return TRUE;
        }
        ptr += ptr[0];
    }

    SetLastError( ERROR_NO_MORE_ITEMS );
    return FALSE;
}

/***********************************************************************
 *           WinUsb_WritePipe (winusb.@)
 */
BOOL WINAPI WinUsb_WritePipe( WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, UCHAR *buffer, ULONG size,
                              ULONG *written, OVERLAPPED *ovl )
{
    struct winusb_interface *iface = handle;
    ULONG count = 0;
    BYTE *request;
    DWORD ret_size;
    BOOL ret;

    TRACE( "handle %p, pipe %#x, buffer %p, size %lu, written %p, ovl %p.\n", handle, pipe, buffer, size, written, ovl );

    if (!iface || (size && !buffer))
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    if (!(request = malloc( sizeof(ULONG) + size )))
    {
        SetLastError( ERROR_NOT_ENOUGH_MEMORY );
        return FALSE;
    }
    *(ULONG *)request = pipe;
    memcpy( request + sizeof(ULONG), buffer, size );

    ret = sync_ioctl( iface->file, IOCTL_WINEUSB_WRITE_PIPE, request, sizeof(ULONG) + size,
                      &count, sizeof(count), &ret_size );
    free( request );
    if (!ret) return FALSE;

    if (written) *written = count;
    if (ovl)
    {
        /* The write is done; report it the way a finished overlapped one would be. */
        ovl->Internal = 0; /* STATUS_SUCCESS */
        ovl->InternalHigh = count;
        if (ovl->hEvent) SetEvent( ovl->hEvent );
    }
    return TRUE;
}
