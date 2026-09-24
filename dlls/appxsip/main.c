/*
 * Subject Interface Package for AppX/MSIX signature files (AppxSignature.p7x)
 *
 * Copyright 2026 adsk-linux contributors
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

#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "mssip.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsip);

/* An AppxSignature.p7x file is the four bytes "PKCX" followed by a DER
 * PKCS #7 SignedData whose content is an SPC_INDIRECT_DATA with the package
 * digests. */
static GUID p7x_subject = { 0x5598cff1, 0x68db, 0x4340, { 0xb5, 0x7f, 0x1c, 0xac, 0xf8, 0x8c, 0x9a, 0x51 } };
static const BYTE p7x_magic[] = { 'P', 'K', 'C', 'X' };

/* Returns the whole subject in a heap buffer. */
static BYTE *read_subject(SIP_SUBJECTINFO *info, DWORD *size)
{
    HANDLE file = info->hFile;
    LARGE_INTEGER file_size;
    BOOL close = FALSE;
    BYTE *data = NULL;
    DWORD read;

    if (info->dwUnionChoice == MSSIP_ADDINFO_BLOB)
    {
        if (!info->psBlob || !info->psBlob->pbMemObject)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        if ((data = malloc(info->psBlob->cbMemObject)))
        {
            memcpy(data, info->psBlob->pbMemObject, info->psBlob->cbMemObject);
            *size = info->psBlob->cbMemObject;
        }
        else SetLastError(ERROR_OUTOFMEMORY);
        return data;
    }

    if (!file || file == INVALID_HANDLE_VALUE)
    {
        if (!info->pwsFileName)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        file = CreateFileW(info->pwsFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE) return NULL;
        close = TRUE;
    }

    if (!GetFileSizeEx(file, &file_size)) goto done;
    if (file_size.QuadPart > 0x1000000)
    {
        SetLastError(TRUST_E_SUBJECT_FORM_UNKNOWN);
        goto done;
    }
    if (!(data = malloc(file_size.LowPart)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        goto done;
    }
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    if (!ReadFile(file, data, file_size.LowPart, &read, NULL) || read != file_size.LowPart)
    {
        free(data);
        data = NULL;
        goto done;
    }
    *size = read;

done:
    if (close) CloseHandle(file);
    return data;
}

/***********************************************************************
 *      P7xSipGetSignedDataMsg (APPXSIP.@)
 */
BOOL WINAPI P7xSipGetSignedDataMsg(SIP_SUBJECTINFO *info, DWORD *encoding, DWORD index,
                                   DWORD *size, BYTE *msg)
{
    DWORD data_size;
    BYTE *data;
    BOOL ret = FALSE;

    TRACE("(%p %p %ld %p %p)\n", info, encoding, index, size, msg);

    if (!info || !encoding || !size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (index)
    {
        SetLastError(TRUST_E_NOSIGNATURE);
        return FALSE;
    }
    if (!(data = read_subject(info, &data_size))) return FALSE;

    if (data_size <= sizeof(p7x_magic) || memcmp(data, p7x_magic, sizeof(p7x_magic)))
    {
        SetLastError(TRUST_E_SUBJECT_FORM_UNKNOWN);
        goto done;
    }

    data_size -= sizeof(p7x_magic);
    *encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    if (!msg)
        ret = TRUE;
    else if (*size < data_size)
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
    else
    {
        memcpy(msg, data + sizeof(p7x_magic), data_size);
        ret = TRUE;
    }
    *size = data_size;

done:
    free(data);
    return ret;
}

/***********************************************************************
 *      P7xSipPutSignedDataMsg (APPXSIP.@)
 */
BOOL WINAPI P7xSipPutSignedDataMsg(SIP_SUBJECTINFO *info, DWORD encoding, DWORD *index,
                                   DWORD size, BYTE *msg)
{
    FIXME("(%p %#lx %p %ld %p): stub\n", info, encoding, index, size, msg);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *      P7xSipRemoveSignedDataMsg (APPXSIP.@)
 */
BOOL WINAPI P7xSipRemoveSignedDataMsg(SIP_SUBJECTINFO *info, DWORD index)
{
    FIXME("(%p %ld): stub\n", info, index);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *      P7xSipCreateIndirectData (APPXSIP.@)
 */
BOOL WINAPI P7xSipCreateIndirectData(SIP_SUBJECTINFO *info, DWORD *size, SIP_INDIRECT_DATA *data)
{
    FIXME("(%p %p %p): stub\n", info, size, data);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *      P7xSipVerifyIndirectData (APPXSIP.@)
 *
 * The digests in the indirect data cover the package contents, which a
 * signature file on its own does not have; package readers verify them.
 */
BOOL WINAPI P7xSipVerifyIndirectData(SIP_SUBJECTINFO *info, SIP_INDIRECT_DATA *data)
{
    FIXME("(%p %p): stub\n", info, data);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *      P7xSipIsFileSupportedName (APPXSIP.@)
 *
 * The IsMyFileType2 entry point: recognizes signature files by extension.
 */
BOOL WINAPI P7xSipIsFileSupportedName(WCHAR *name, GUID *subject)
{
    SIZE_T len;

    TRACE("(%s %p)\n", debugstr_w(name), subject);

    if (!name || !subject || (len = wcslen(name)) < 4 || wcsicmp(name + len - 4, L".p7x"))
        return FALSE;
    *subject = p7x_subject;
    return TRUE;
}

/***********************************************************************
 *      DllRegisterServer (APPXSIP.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    SIP_ADD_NEWPROVIDER prov;

    TRACE("()\n");

    memset(&prov, 0, sizeof(prov));
    prov.cbStruct = sizeof(prov);
    prov.pgSubject = &p7x_subject;
    prov.pwszDLLFileName = (WCHAR *)L"appxsip.dll";
    prov.pwszIsFunctionNameFmt2 = (WCHAR *)L"P7xSipIsFileSupportedName";
    prov.pwszGetFuncName = (WCHAR *)L"P7xSipGetSignedDataMsg";
    prov.pwszPutFuncName = (WCHAR *)L"P7xSipPutSignedDataMsg";
    prov.pwszCreateFuncName = (WCHAR *)L"P7xSipCreateIndirectData";
    prov.pwszVerifyFuncName = (WCHAR *)L"P7xSipVerifyIndirectData";
    prov.pwszRemoveFuncName = (WCHAR *)L"P7xSipRemoveSignedDataMsg";

    /* start from a clean slate so that no stale entry points stay registered */
    CryptSIPRemoveProvider(&p7x_subject);
    if (!CryptSIPAddProvider(&prov)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

/***********************************************************************
 *      DllUnregisterServer (APPXSIP.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    TRACE("()\n");

    CryptSIPRemoveProvider(&p7x_subject);
    return S_OK;
}
