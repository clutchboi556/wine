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

#ifndef _WINUSB_H_
#define _WINUSB_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef PVOID WINUSB_INTERFACE_HANDLE, *PWINUSB_INTERFACE_HANDLE;

#define DEVICE_SPEED 0x01
#define LowSpeed     0x01
#define FullSpeed    0x02
#define HighSpeed    0x03

typedef struct _WINUSB_PIPE_INFORMATION
{
    ULONG  PipeType;    /* USBD_PIPE_TYPE */
    UCHAR  PipeId;
    USHORT MaximumPacketSize;
    UCHAR  Interval;
} WINUSB_PIPE_INFORMATION, *PWINUSB_PIPE_INFORMATION;

struct _USB_INTERFACE_DESCRIPTOR;

BOOL WINAPI WinUsb_Free(WINUSB_INTERFACE_HANDLE);
BOOL WINAPI WinUsb_Initialize(HANDLE,WINUSB_INTERFACE_HANDLE*);
BOOL WINAPI WinUsb_QueryDeviceInformation(WINUSB_INTERFACE_HANDLE,ULONG,ULONG*,void*);
BOOL WINAPI WinUsb_QueryInterfaceSettings(WINUSB_INTERFACE_HANDLE,UCHAR,struct _USB_INTERFACE_DESCRIPTOR*);
BOOL WINAPI WinUsb_QueryPipe(WINUSB_INTERFACE_HANDLE,UCHAR,UCHAR,WINUSB_PIPE_INFORMATION*);
BOOL WINAPI WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE,UCHAR,UCHAR*,ULONG,ULONG*,OVERLAPPED*);

#ifdef __cplusplus
}
#endif

#endif /* _WINUSB_H_ */
