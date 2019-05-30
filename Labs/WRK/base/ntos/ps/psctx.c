/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    psctx.c

Abstract:

    This procedure implements Get/Set Context Thread

--*/

#include "psp.h"

VOID
PspQueueApcSpecialApc(
    IN PKAPC Apc,
    IN PKNORMAL_ROUTINE *NormalRoutine,
    IN PVOID *NormalContext,
    IN PVOID *SystemArgument1,
    IN PVOID *SystemArgument2
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, NtGetContextThread)
#pragma alloc_text(PAGE, NtSetContextThread)
#pragma alloc_text(PAGE, PsGetContextThread)
#pragma alloc_text(PAGE, PsSetContextThread)
#pragma alloc_text(PAGE, NtQueueApcThread)
#pragma alloc_text(PAGE, PspQueueApcSpecialApc)
#endif

VOID
PspQueueApcSpecialApc(
    IN PKAPC Apc,
    IN PKNORMAL_ROUTINE *NormalRoutine,
    IN PVOID *NormalContext,
    IN PVOID *SystemArgument1,
    IN PVOID *SystemArgument2
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER (NormalRoutine);
    UNREFERENCED_PARAMETER (NormalContext);
    UNREFERENCED_PARAMETER (SystemArgument1);
    UNREFERENCED_PARAMETER (SystemArgument2);

    ExFreePool(Apc);
}

NTSYSAPI
NTSTATUS
NTAPI
NtQueueApcThread(
    __in HANDLE ThreadHandle,
    __in PPS_APC_ROUTINE ApcRoutine,
    __in_opt PVOID ApcArgument1,
    __in_opt PVOID ApcArgument2,
    __in_opt PVOID ApcArgument3
    )

/*++

Routine Description:

    This function is used to queue a user-mode APC to the specified thread. The APC
    will fire when the specified thread does an alertable wait

Arguments:

    ThreadHandle - Supplies a handle to a thread object.  The caller
        must have THREAD_SET_CONTEXT access to the thread.

    ApcRoutine - Supplies the address of the APC routine to execute when the
        APC fires.

    ApcArgument1 - Supplies the first PVOID passed to the APC

    ApcArgument2 - Supplies the second PVOID passed to the APC

    ApcArgument3 - Supplies the third PVOID passed to the APC

Return Value:

    Returns an NT Status code indicating success or failure of the API

--*/

{
    PETHREAD Thread;
    NTSTATUS st;
    KPROCESSOR_MODE Mode;
    PKAPC Apc;

    PAGED_CODE();

    Mode = KeGetPreviousMode ();

    st = ObReferenceObjectByHandle (ThreadHandle,
                                    THREAD_SET_CONTEXT,
                                    PsThreadType,
                                    Mode,
                                    &Thread,
                                    NULL);
    if (NT_SUCCESS (st)) {
        st = STATUS_SUCCESS;
        if (IS_SYSTEM_THREAD (Thread)) {
            st = STATUS_INVALID_HANDLE;
        } else {
            Apc = ExAllocatePoolWithQuotaTag (NonPagedPool | POOL_QUOTA_FAIL_INSTEAD_OF_RAISE,
                                              sizeof(*Apc),
                                              'pasP');

            if (Apc == NULL) {
                st = STATUS_NO_MEMORY;
            } else {
                KeInitializeApc (Apc,
                                 &Thread->Tcb,
                                 OriginalApcEnvironment,
                                 PspQueueApcSpecialApc,
                                 NULL,
                                 (PKNORMAL_ROUTINE)ApcRoutine,
                                 UserMode,
                                 ApcArgument1);

                if (!KeInsertQueueApc (Apc, ApcArgument2, ApcArgument3, 0)) {
                    ExFreePool (Apc);
                    st = STATUS_UNSUCCESSFUL;
                }
            }
        }
        ObDereferenceObject (Thread);
    }

    return st;
}


NTSTATUS
PsGetContextThread(
    __in PETHREAD Thread,
    __inout PCONTEXT ThreadContext,
    __in KPROCESSOR_MODE Mode
    )

/*++

Routine Description:

    This function returns the usermode context of the specified thread. This
    function will fail if the specified thread is a system thread. It will
    return the wrong answer if the thread is a non-system thread that does
    not execute in user-mode.

Arguments:

    Thread -       Supplies a pointer to the thread object from
                   which to retrieve context information.

    ThreadContext - Supplies the address of a buffer that will receive
                    the context of the specified thread.

    Mode          - Mode to use for validation checks.

Return Value:

    None.

--*/

{

    ULONG ContextFlags=0;
    GETSETCONTEXT ContextFrame = {0};
    ULONG ContextLength=0;
    NTSTATUS Status;
    PETHREAD CurrentThread;

    PAGED_CODE();

    Status = STATUS_SUCCESS;

    //
    // Get previous mode and reference specified thread.
    //

    CurrentThread = PsGetCurrentThread ();

    //
    // Attempt to get the context of the specified thread.
    //

    try {

        //
        // Set the default alignment, capture the context flags,
        // and set the default size of the context record.
        //

        if (Mode != KernelMode) {
            ProbeForReadSmallStructure (ThreadContext,
                                        FIELD_OFFSET (CONTEXT, ContextFlags) + sizeof (ThreadContext->ContextFlags),
                                        CONTEXT_ALIGN);
        }

        ContextFlags = ThreadContext->ContextFlags;

        //
        // We don't need to re-probe here so long as the structure is smaller
        // than the guard region
        //
        ContextLength = sizeof(CONTEXT);
        ASSERT (ContextLength < 0x10000);

#if defined(_X86_)
        //
        // CONTEXT_EXTENDED_REGISTERS is SET, then we want sizeof(CONTEXT) set above
        // otherwise (not set) we only want the old part of the context record.
        //
        if ((ContextFlags & CONTEXT_EXTENDED_REGISTERS) != CONTEXT_EXTENDED_REGISTERS) {
            ContextLength = FIELD_OFFSET(CONTEXT, ExtendedRegisters);
        }
#endif

    } except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode ();
    }

    KeInitializeEvent (&ContextFrame.OperationComplete,
                       NotificationEvent,
                       FALSE);

    ContextFrame.Context.ContextFlags = ContextFlags;

    ContextFrame.Mode = Mode;
    if (Thread == CurrentThread) {
        ContextFrame.Apc.SystemArgument1 = NULL;
        ContextFrame.Apc.SystemArgument2 = Thread;
        KeEnterGuardedRegionThread (&CurrentThread->Tcb);
        PspGetSetContextSpecialApc (&ContextFrame.Apc,
                                    NULL,
                                    NULL,
                                    &ContextFrame.Apc.SystemArgument1,
                                    &ContextFrame.Apc.SystemArgument2);

        KeLeaveGuardedRegionThread (&CurrentThread->Tcb);

        //
        // Move context to specfied context record. If an exception
        // occurs, then return the error.
        //

        try {
            RtlCopyMemory (ThreadContext,
                           &ContextFrame.Context,
                           ContextLength);

        } except (EXCEPTION_EXECUTE_HANDLER) {
            Status = GetExceptionCode ();
        }

    } else {
        KeInitializeApc (&ContextFrame.Apc,
                         &Thread->Tcb,
                         OriginalApcEnvironment,
                         PspGetSetContextSpecialApc,
                         NULL,
                         NULL,
                         KernelMode,
                         NULL);

        if (!KeInsertQueueApc (&ContextFrame.Apc, NULL, Thread, 2)) {
            Status = STATUS_UNSUCCESSFUL;

        } else {
            KeWaitForSingleObject (&ContextFrame.OperationComplete,
                                   Executive,
                                   KernelMode,
                                   FALSE,
                                   NULL);
            //
            // Move context to specfied context record. If an
            // exception occurs, then silently handle it and
            // return success.
            //

            try {
                RtlCopyMemory (ThreadContext,
                               &ContextFrame.Context,
                               ContextLength);

            } except (EXCEPTION_EXECUTE_HANDLER) {
                Status = GetExceptionCode ();
            }
        }
    }

    return Status;
}

NTSTATUS
NtGetContextThread(
    __in HANDLE ThreadHandle,
    __inout PCONTEXT ThreadContext
    )

/*++

Routine Description:

    This function returns the usermode context of the specified thread. This
    function will fail if the specified thread is a system thread. It will
    return the wrong answer if the thread is a non-system thread that does
    not execute in user-mode.

Arguments:

    ThreadHandle - Supplies an open handle to the thread object from
                   which to retrieve context information.  The handle
                   must allow THREAD_GET_CONTEXT access to the thread.

    ThreadContext - Supplies the address of a buffer that will receive
                    the context of the specified thread.

Return Value:

    None.

--*/

{

    KPROCESSOR_MODE Mode;
    NTSTATUS Status;
    PETHREAD Thread;
    PETHREAD CurrentThread;

    PAGED_CODE();

    //
    // Get previous mode and reference specified thread.
    //

    CurrentThread = PsGetCurrentThread ();
    Mode = KeGetPreviousModeByThread (&CurrentThread->Tcb);

    Status = ObReferenceObjectByHandle (ThreadHandle,
                                        THREAD_GET_CONTEXT,
                                        PsThreadType,
                                        Mode,
                                        &Thread,
                                        NULL);

    //
    // If the reference was successful, the check if the specified thread
    // is a system thread.
    //

    if (NT_SUCCESS (Status)) {

        //
        // If the thread is not a system thread, then attempt to get the
        // context of the thread.
        //

        if (IS_SYSTEM_THREAD (Thread) == FALSE) {

            Status = PsGetContextThread (Thread, ThreadContext, Mode);

        } else {
            Status = STATUS_INVALID_HANDLE;
        }

        ObDereferenceObject (Thread);
    }

    return Status;
}


NTSTATUS
PsSetContextThread(
    __in PETHREAD Thread,
    __in PCONTEXT ThreadContext,
    __in KPROCESSOR_MODE Mode
    )

/*++

Routine Description:

    This function sets the usermode context of the specified thread. This
    function will fail if the specified thread is a system thread. It will
    return the wrong answer if the thread is a non-system thread that does
    not execute in user-mode.

Arguments:

    Thread       - Supplies the thread object from
                   which to retrieve context information.

    ThreadContext - Supplies the address of a buffer that contains new
                    context for the specified thread.

    Mode          - Mode to use for validation checks.

Return Value:

    None.

--*/

{
    ULONG ContextFlags=0;
    GETSETCONTEXT ContextFrame;
    ULONG ContextLength=0;
    NTSTATUS Status;
    PETHREAD CurrentThread;

    PAGED_CODE();

    Status = STATUS_SUCCESS;

    //
    // Get previous mode and reference specified thread.
    //

    CurrentThread = PsGetCurrentThread ();

    //
    // Attempt to get the context of the specified thread.
    //

    try {

        //
        // Capture the context flags,
        // and set the default size of the context record.
        //

        if (Mode != KernelMode) {
            ProbeForReadSmallStructure (ThreadContext,
                                        FIELD_OFFSET (CONTEXT, ContextFlags) + sizeof (ThreadContext->ContextFlags),
                                        CONTEXT_ALIGN);
        }

        //
        // We don't need to re-probe here so long as the structure is small
        // enough not to cross the guard region.
        //
        ContextFlags = ThreadContext->ContextFlags;
        ContextLength = sizeof (CONTEXT);
        ASSERT (ContextLength < 0x10000);

#if defined(_X86_)
        //
        // CONTEXT_EXTENDED_REGISTERS is SET, then we want sizeof(CONTEXT) set above
        // otherwise (not set) we only want the old part of the context record.
        //
        if ((ContextFlags & CONTEXT_EXTENDED_REGISTERS) != CONTEXT_EXTENDED_REGISTERS) {
            ContextLength = FIELD_OFFSET(CONTEXT, ExtendedRegisters);
        } 
#endif

        RtlCopyMemory (&ContextFrame.Context, ThreadContext, ContextLength);

    } except (EXCEPTION_EXECUTE_HANDLER) {

        return GetExceptionCode ();
    }

    //
    // set the context of the target thread.
    //

    KeInitializeEvent (&ContextFrame.OperationComplete,
                       NotificationEvent,
                       FALSE);

    ContextFrame.Context.ContextFlags = ContextFlags;

    ContextFrame.Mode = Mode;
    if (Thread == CurrentThread) {
        ContextFrame.Apc.SystemArgument1 = (PVOID)1;
        ContextFrame.Apc.SystemArgument2 = Thread;
        KeEnterGuardedRegionThread (&CurrentThread->Tcb);
        PspGetSetContextSpecialApc (&ContextFrame.Apc,
                                    NULL,
                                    NULL,
                                    &ContextFrame.Apc.SystemArgument1,
                                    &ContextFrame.Apc.SystemArgument2);

        KeLeaveGuardedRegionThread (&CurrentThread->Tcb);

    } else {
        KeInitializeApc (&ContextFrame.Apc,
                         &Thread->Tcb,
                         OriginalApcEnvironment,
                         PspGetSetContextSpecialApc,
                         NULL,
                         NULL,
                         KernelMode,
                         NULL);

        if (!KeInsertQueueApc (&ContextFrame.Apc, (PVOID)1, Thread, 2)) {
            Status = STATUS_UNSUCCESSFUL;

        } else {
            KeWaitForSingleObject (&ContextFrame.OperationComplete,
                                   Executive,
                                   KernelMode,
                                   FALSE,
                                   NULL);
        }
    }

    return Status;
}


NTSTATUS
NtSetContextThread(
    __in HANDLE ThreadHandle,
    __in PCONTEXT ThreadContext
    )
    
/*++

Routine Description:

    This function sets the usermode context of the specified thread. This
    function will fail if the specified thread is a system thread. It will
    return the wrong answer if the thread is a non-system thread that does
    not execute in user-mode.

Arguments:

    ThreadHandle - Supplies an open handle to the thread object from
                   which to retrieve context information.  The handle
                   must allow THREAD_SET_CONTEXT access to the thread.

    ThreadContext - Supplies the address of a buffer that contains new
                    context for the specified thread.

Return Value:

    None.

--*/

{
    KPROCESSOR_MODE Mode;
    NTSTATUS Status;
    PETHREAD Thread;
    PETHREAD CurrentThread;

    PAGED_CODE();

    //
    // Get previous mode and reference specified thread.
    //

    CurrentThread = PsGetCurrentThread ();
    Mode = KeGetPreviousModeByThread (&CurrentThread->Tcb);

    Status = ObReferenceObjectByHandle (ThreadHandle,
                                        THREAD_SET_CONTEXT,
                                        PsThreadType,
                                        Mode,
                                        &Thread,
                                        NULL);

    //
    // If the reference was successful, the check if the specified thread
    // is a system thread.
    //

    if (NT_SUCCESS (Status)) {

        //
        // If the thread is not a system thread, then attempt to get the
        // context of the thread.
        //

        if (IS_SYSTEM_THREAD (Thread) == FALSE) {

            Status = PsSetContextThread (Thread, ThreadContext, Mode);

        } else {
            Status = STATUS_INVALID_HANDLE;
        }

        ObDereferenceObject (Thread);
    }

    return Status;
}

NTSTATUS
PsWrapApcWow64Thread (
    __inout PVOID *ApcContext,
    __inout PVOID *ApcRoutine)

/*++

Routine Description:

    This routine is used by kernel mode callers to queue APCs to a 32-bit thread
    running inside a Wow64 process. It wraps the original APC routine
    with a wrapper routine inside Wow64. The target Apc routine must be
    inside 32-bit code. This routine must be executed in the context of the
    Wow64 thread where the target APC is going to run in. The resulting ApcContext
    and ApcRoutine from this routine are used when initializing, using KeInitializeApc,
    to be queued later to a Wow64 thread.
    
    The API does nothing and succeeds on :
        - 32-bit systems.
        - native 64-bit processes on 64-bit systems.

Environment:

    Kernel mode only.

Arguments:

    ApcContext - Pointer to the original ApcContext parameter.

    ApcRoutine - Pointer to the original ApcRoutine that is targeted to run
        32-bit code.

Return:

    NTSTATUS.

--*/

{

    NTSTATUS NtStatus;

    NtStatus = STATUS_SUCCESS;

#if defined(_WIN64)
    
    if (PsGetCurrentProcess ()->Wow64Process != NULL) {
        
        try {
            Wow64KiWrapApcProc (ApcContext, ApcRoutine);

        } except (EXCEPTION_EXECUTE_HANDLER) {
            NtStatus = GetExceptionCode ();
        }
    }

#else

    UNREFERENCED_PARAMETER(ApcContext);
    UNREFERENCED_PARAMETER(ApcRoutine);

#endif

    return NtStatus;
}

