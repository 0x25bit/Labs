/*++ BUILD Version: 0001    // Increment this if a change has global effects

Copyright (c) Microsoft Corporation. All rights reserved.

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    ntddnull.h

Abstract:

    This is the include file that defines all constants and types for
    accessing the Null device.

--*/

#ifndef _NTDDNULL_
#define _NTDDNULL_

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
// Note:  For devices that support multiple units, it should be suffixed
//        with the Ascii representation of the unit number.
//

#define DD_NULL_DEVICE_NAME "\\Device\\Null"


//
// NtDeviceIoControlFile IoControlCode values for this device.
//
// Warning:  Remember that the low two bits of the code specify how the
//           buffers are passed to the driver!
//

#define IOCTL_NULL_BASE                 FILE_DEVICE_NULL


//
// NtDeviceIoControlFile InputBuffer/OutputBuffer record structures for
// this device.
//

#ifdef __cplusplus
}
#endif

#endif  // _NTDDNULL_

