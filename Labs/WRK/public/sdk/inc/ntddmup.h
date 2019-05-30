/*++ BUILD Version: 0001    // Increment this if a change has global effects

Copyright (c) Microsoft Corporation. All rights reserved.

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    ntddmup.h

Abstract:

    This is the include file that defines all constants and types for
    accessing the Multiple UNC provider system device.

--*/

#ifndef _NTDDMUP_
#define _NTDDMUP_

#if _MSC_VER > 1000
#pragma once
#endif

#ifdef __cplusplus
extern "C" {
#endif

//
// Device Name - this string is the name of the device.  It is the name
// that should be passed to NtOpenFile when accessing the device.
//
//

#define DD_MUP_DEVICE_NAME L"\\Device\\Mup"    // ntifs

//
// NtFsControlFile FsControlCode values for this device.
//

#define FSCTL_MUP_REGISTER_UNC_PROVIDER     CTL_CODE(FILE_DEVICE_MULTI_UNC_PROVIDER, 1, METHOD_BUFFERED, FILE_ANY_ACCESS)

//
// Fs control parameter blocks.
//

typedef struct _REDIRECTOR_REGISTRATION {
    ULONG DeviceNameOffset;
    ULONG DeviceNameLength;
    ULONG ShortNameOffset;
    ULONG ShortNameLength;
    BOOLEAN MailslotsSupported;
} REDIRECTOR_REGISTRATION, *PREDIRECTOR_REGISTRATION;

#ifndef _NTIFS_


// begin_ntifs

#define IOCTL_REDIR_QUERY_PATH              CTL_CODE(FILE_DEVICE_NETWORK_FILE_SYSTEM, 99, METHOD_NEITHER, FILE_ANY_ACCESS)

typedef struct _QUERY_PATH_REQUEST {
    ULONG PathNameLength;
    PIO_SECURITY_CONTEXT SecurityContext;
    WCHAR FilePathName[1];
} QUERY_PATH_REQUEST, *PQUERY_PATH_REQUEST;

typedef struct _QUERY_PATH_RESPONSE {
    ULONG LengthAccepted;
} QUERY_PATH_RESPONSE, *PQUERY_PATH_RESPONSE;

// end_ntifs

#endif // _NTIFS_
#ifdef __cplusplus
}
#endif

#endif  // _NTDDMUP_

