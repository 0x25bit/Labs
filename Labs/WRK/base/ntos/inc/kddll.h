/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.

    
Module Name:

    kddll.h

Abstract:
    
    Kernel Debugger HW Extension DLL definitions

--*/

#ifndef __KDDLL_H__
#define __KDDLL_H__


//
// This Kernel Debugger Context structure is used to share
// information between the Kernel Debugger and the Kernel
// Debugger HW extension DLL
//
typedef struct _KD_CONTEXT {
    ULONG KdpDefaultRetries;
    BOOLEAN KdpControlCPending;
} KD_CONTEXT, *PKD_CONTEXT;

//
// Kernel Debugger HW Extension DLL exported functions
//
NTSTATUS
KdD0Transition(
    VOID
    );

NTSTATUS
KdD3Transition(
    VOID
    );

NTSTATUS
KdDebuggerInitialize0(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock
    );

NTSTATUS
KdDebuggerInitialize1(
    IN PLOADER_PARAMETER_BLOCK LoaderBlock
    );

ULONG
KdReceivePacket(
    IN ULONG PacketType,
    OUT PSTRING MessageHeader,
    OUT PSTRING MessageData,
    OUT PULONG DataLength,
    IN OUT PKD_CONTEXT KdContext
    );

NTSTATUS
KdRestore(
    IN BOOLEAN KdSleepTransition
    );

NTSTATUS
KdSave(
    IN BOOLEAN KdSleepTransition
    );

//
// status Constants for Packet waiting
//

#define KDP_PACKET_RECEIVED 0
#define KDP_PACKET_TIMEOUT 1
#define KDP_PACKET_RESEND 2

VOID
KdSendPacket(
    IN ULONG PacketType,
    IN PSTRING MessageHeader,
    IN PSTRING MessageData OPTIONAL,
    IN OUT PKD_CONTEXT KdContext
    );

#endif // __KDDLL_H__
