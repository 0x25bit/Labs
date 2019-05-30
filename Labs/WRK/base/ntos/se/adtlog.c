/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    adtlog.c

Abstract:

    Auditing - Audit Record Queuing and Logging Routines

    This file contains functions that construct Audit Records in self-
    relative form from supplied information, enqueue/dequeue them and
    write them to the log.

--*/

#include "pch.h"

#pragma hdrstop

#include <msaudite.h>


#ifdef ALLOC_PRAGMA

#pragma alloc_text(PAGE,SepAdtLogAuditRecord)
#pragma alloc_text(PAGE,SepAuditFailed)
#pragma alloc_text(PAGE,SepAdtMarshallAuditRecord)
#pragma alloc_text(PAGE,SepAdtCopyToLsaSharedMemory)
#pragma alloc_text(PAGE,SepQueueWorkItem)
#pragma alloc_text(PAGE,SepDequeueWorkItem)

#endif

VOID
SepAdtLogAuditRecord(
    IN PSE_ADT_PARAMETER_ARRAY AuditParameters
    )

/*++

Routine Description:

    This function manages the logging of Audit Records.  It provides the
    single interface to the Audit Logging component from the Audit/Alarm
    generation routines.  The function constructs an Audit Record in
    self-relative format from the information provided and appends it to
    the Audit Record Queue, a doubly-linked list of Audit Records awaiting
    output to the Audit Log.  A dedicated thread reads this queue, writing
    Audit Records to the Audit Log and removing them from the Audit Queue.

Arguments:

    AuditEventType - Specifies the type of the Audit Event described by
        the audit information provided.

    AuditInformation - Pointer to buffer containing captured auditing
        information related to an Audit Event of type AuditEventType.

Return Value:

    STATUS_SUCCESS
    STATUS_UNSUCCESSFUL - Audit record was not queued
    STATUS_INSUFFICIENT_RESOURCES - unable to allocate heap

--*/

{
    NTSTATUS Status;
    BOOLEAN ForceQueue;
    PSEP_LSA_WORK_ITEM AuditWorkItem;

    PAGED_CODE();

    AuditWorkItem = ExAllocatePoolWithTag( PagedPool, sizeof( SEP_LSA_WORK_ITEM ), 'iAeS' );

    if ( AuditWorkItem == NULL ) {

        SepAuditFailed( STATUS_INSUFFICIENT_RESOURCES );
        return;
    }

    AuditWorkItem->Tag = SepAuditRecord;
    AuditWorkItem->CommandNumber = LsapWriteAuditMessageCommand;
    AuditWorkItem->ReplyBuffer = NULL;
    AuditWorkItem->ReplyBufferLength = 0;
    AuditWorkItem->CleanupFunction = NULL;

    //
    // Build an Audit record in self-relative format from the supplied
    // Audit Information.
    //

    Status = SepAdtMarshallAuditRecord(
                 AuditParameters,
                 (PSE_ADT_PARAMETER_ARRAY *) &AuditWorkItem->CommandParams.BaseAddress,
                 &AuditWorkItem->CommandParamsMemoryType
                 );

    if (NT_SUCCESS(Status)) {

        //
        // Extract the length of the Audit Record.  Store it as the length
        // of the Command Parameters buffer.
        //

        AuditWorkItem->CommandParamsLength =
            ((PSE_ADT_PARAMETER_ARRAY) AuditWorkItem->CommandParams.BaseAddress)->Length;

        //
        // If we're going to crash on a discarded audit, ignore the queue bounds
        // check and force the item onto the queue.
        //

        if (SepCrashOnAuditFail || AuditParameters->AuditId == SE_AUDITID_AUDITS_DISCARDED) {
            ForceQueue = TRUE;
        } else {
            ForceQueue = FALSE;
        }

        if (!SepQueueWorkItem( AuditWorkItem, ForceQueue )) {

            ExFreePool( AuditWorkItem->CommandParams.BaseAddress );
            ExFreePool( AuditWorkItem );

            //
            // We failed to put the record on the queue.  Take whatever action is
            // appropriate.
            //

            SepAuditFailed( STATUS_UNSUCCESSFUL );
        }

    } else {

        ExFreePool( AuditWorkItem );
        SepAuditFailed( Status );
    }
}



VOID
SepAuditFailed(
    IN NTSTATUS AuditStatus
    )

/*++

Routine Description:

    Bugchecks the system due to a missed audit (optional requirement
    for C2 compliance).

Arguments:

    None.

Return Value:

    None.

--*/

{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES Obja;
    HANDLE KeyHandle;
    UNICODE_STRING KeyName;
    UNICODE_STRING ValueName;
    ULONG NewValue;

    ASSERT(sizeof(UCHAR) == sizeof(BOOLEAN));

    if (!SepCrashOnAuditFail) {
        return;
    }

    //
    // Turn off flag in the registry that controls crashing on audit failure
    //

    RtlInitUnicodeString( &KeyName, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Lsa");

    InitializeObjectAttributes( &Obja,
                                &KeyName,
                                OBJ_CASE_INSENSITIVE | 
                                    OBJ_KERNEL_HANDLE,
                                NULL,
                                NULL
                                );
    do {

        Status = ZwOpenKey(
                     &KeyHandle,
                     KEY_SET_VALUE,
                     &Obja
                     );

    } while ((Status == STATUS_INSUFFICIENT_RESOURCES) || (Status == STATUS_NO_MEMORY));

    //
    // If the LSA key isn't there, he's got big problems.  But don't crash.
    //

    if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {
        SepCrashOnAuditFail = FALSE;
        return;
    }

    if (!NT_SUCCESS( Status )) {
        goto bugcheck;
    }

    RtlInitUnicodeString( &ValueName, CRASH_ON_AUDIT_FAIL_VALUE );

    NewValue = LSAP_ALLOW_ADIMIN_LOGONS_ONLY;

    do {

        Status = ZwSetValueKey( KeyHandle,
                                &ValueName,
                                0,
                                REG_DWORD,
                                &NewValue,
                                sizeof(ULONG)
                                );

    } while ((Status == STATUS_INSUFFICIENT_RESOURCES) || (Status == STATUS_NO_MEMORY));
    ASSERT(NT_SUCCESS(Status));

    if (!NT_SUCCESS( Status )) {
        goto bugcheck;
    }

    do {

        Status = ZwFlushKey( KeyHandle );

    } while ((Status == STATUS_INSUFFICIENT_RESOURCES) || (Status == STATUS_NO_MEMORY));
    ASSERT(NT_SUCCESS(Status));

    //
    // go boom.
    //

bugcheck:

    KeBugCheckEx(STATUS_AUDIT_FAILED, AuditStatus, 0, 0, 0);
}



NTSTATUS
SepAdtMarshallAuditRecord(
    IN PSE_ADT_PARAMETER_ARRAY AuditParameters,
    OUT PSE_ADT_PARAMETER_ARRAY *MarshalledAuditParameters,
    OUT PSEP_RM_LSA_MEMORY_TYPE RecordMemoryType
    )

/*++

Routine Description:

    This routine will take an AuditParameters structure and create
    a new AuditParameters structure that is suitable for sending
    to LSA.  It will be in self-relative form and allocated as
    a single chunk of memory.

Arguments:


    AuditParameters - A filled in set of AuditParameters to be marshalled.

    MarshalledAuditParameters - Returns a pointer to a block of heap memory
        containing the passed AuditParameters in self-relative form suitable
        for passing to LSA.

    RecordMemoryType -- type of memory returned. currently always uses
                        paged pool (returns SepRmPagedPoolMemory)

Return Value:

    NTSTATUS code

--*/

{
    ULONG i;
    ULONG TotalSize = sizeof( SE_ADT_PARAMETER_ARRAY );
    PUNICODE_STRING TargetString;
    PCHAR Base;
    ULONG BaseIncr;
    ULONG Size;
    PSE_ADT_PARAMETER_ARRAY_ENTRY pInParam, pOutParam;

    PAGED_CODE();

    ASSERT( AuditParameters );

    ASSERT(IsValidParameterCount(AuditParameters->ParameterCount));
    
    //
    // Calculate the total size required for the passed AuditParameters
    // block.  This calculation will probably be an overestimate of the
    // amount of space needed, because data smaller that 2 dwords will
    // be stored directly in the parameters structure, but their length
    // will be counted here anyway.  The overestimate can't be more than
    // 24 dwords, and will never even approach that amount, so it isn't
    // worth the time it would take to avoid it.
    //

    for (i=0; i<AuditParameters->ParameterCount; i++) {
        Size = AuditParameters->Parameters[i].Length;
        TotalSize += PtrAlignSize( Size );
    }

    //
    // Allocate a big enough block of memory to hold everything.
    // If it fails, quietly abort, since there isn't much else we
    // can do.
    //

    *MarshalledAuditParameters = ExAllocatePoolWithTag( PagedPool, TotalSize, 'pAeS' );

    if (*MarshalledAuditParameters == NULL) {

        *RecordMemoryType = SepRmNoMemory;
        return(STATUS_INSUFFICIENT_RESOURCES);
    }

    *RecordMemoryType = SepRmPagedPoolMemory;

    RtlCopyMemory (
       *MarshalledAuditParameters,
       AuditParameters,
       sizeof( SE_ADT_PARAMETER_ARRAY )
       );

   (*MarshalledAuditParameters)->Length = TotalSize;
   (*MarshalledAuditParameters)->Flags  = SE_ADT_PARAMETERS_SELF_RELATIVE;

    pInParam  = &AuditParameters->Parameters[0];
    pOutParam = &((*MarshalledAuditParameters)->Parameters[0]);
   
    //
    // Start walking down the list of parameters and marshall them
    // into the target buffer.
    //

    Base = (PCHAR) ((PCHAR)(*MarshalledAuditParameters) + sizeof( SE_ADT_PARAMETER_ARRAY ));

    for (i=0; i<AuditParameters->ParameterCount; i++, pInParam++, pOutParam++) {


        switch (AuditParameters->Parameters[i].Type) {
            case SeAdtParmTypeNone:
            case SeAdtParmTypeUlong:
            case SeAdtParmTypeHexUlong:
            case SeAdtParmTypeLogonId:
            case SeAdtParmTypeLuid:
            case SeAdtParmTypeNoLogonId:
            case SeAdtParmTypeTime:
            case SeAdtParmTypeAccessMask:
            case SeAdtParmTypePtr:
            case SeAdtParmTypeDuration:
            case SeAdtParmTypeHexInt64:
            case SeAdtParmTypeDateTime:
            case SeAdtParmTypeMessage:
                {
                    //
                    // Nothing to do for this
                    //

                    break;

                }
            case SeAdtParmTypeString:
            case SeAdtParmTypeFileSpec:
                {
                    PUNICODE_STRING SourceString;
                    //
                    // We must copy the body of the unicode string
                    // and then copy the body of the string.  Pointers
                    // must be turned into offsets.

                    TargetString = (PUNICODE_STRING)Base;

                    SourceString = pInParam->Address;

                    *TargetString = *SourceString;

                    //
                    // Reset the data pointer in the output parameters to
                    // 'point' to the new string structure.
                    //

                    pOutParam->Address = Base - (ULONG_PTR)(*MarshalledAuditParameters);

                    Base += sizeof( UNICODE_STRING );

                    RtlCopyMemory( Base, SourceString->Buffer, SourceString->Length );

                    //
                    // Make the string buffer in the target string point to where we
                    // just copied the data.
                    //

                    TargetString->Buffer = (PWSTR)(Base - (ULONG_PTR)(*MarshalledAuditParameters));

                    BaseIncr = PtrAlignSize(SourceString->Length);

                    Base += BaseIncr;

                    ASSERT( (ULONG_PTR)Base <= (ULONG_PTR)(*MarshalledAuditParameters) + TotalSize );
                    break;
                }

            //
            // Handle types where we simply copy the buffer.
            //
            case SeAdtParmTypePrivs:
            case SeAdtParmTypeSid:
            case SeAdtParmTypeObjectTypes:
            case SeAdtParmTypeSockAddr:
            case SeAdtParmTypeGuid:

                {
                    //
                    // Copy the data into the output buffer
                    //

                    RtlCopyMemory( Base, pInParam->Address, pInParam->Length );

                    //
                    // Reset the 'address' of the data to be its offset in the
                    // buffer.
                    //

                    pOutParam->Address = Base - (ULONG_PTR)(*MarshalledAuditParameters);

                    Base +=  PtrAlignSize( pInParam->Length );


                    ASSERT( (ULONG_PTR)Base <= (ULONG_PTR)(*MarshalledAuditParameters) + TotalSize );
                    break;
                }

            default:
                {
                    //
                    // We got passed junk, complain.
                    //

                    ASSERT( FALSE );
                    break;
                }
        }
    }

    return( STATUS_SUCCESS );
}


NTSTATUS
SepAdtCopyToLsaSharedMemory(
    IN HANDLE LsaProcessHandle,
    IN PVOID Buffer,
    IN ULONG BufferLength,
    OUT PVOID *LsaBufferAddress
    )

/*++

Routine Description:

    This function allocates memory shared with the LSA and optionally copies
    a given buffer to it.

Arguments:

    LsaProcessHandle - Specifies a handle to the Lsa Process.

    Buffer - Pointer to the buffer to be copied.

    BufferLength - Length of buffer.

    LsaBufferAddress - Receives the address of the buffer valid in the
        Lsa process context.

Return Value:

    NTSTATUS - Standard Nt Result Code

        Result codes returned by called routines.
--*/

{
    NTSTATUS Status, SecondaryStatus;
    PVOID OutputLsaBufferAddress = NULL;
    SIZE_T RegionSize = BufferLength;
    SIZE_T NumberOfBytesWritten = 0;
    BOOLEAN VmAllocated = FALSE;
    
    PAGED_CODE();

    Status = ZwAllocateVirtualMemory(
                 LsaProcessHandle,
                 &OutputLsaBufferAddress,
                 0,             // do not apply zero bits constraint
                 &RegionSize,
                 MEM_COMMIT,
                 PAGE_READWRITE
                 );

    if (!NT_SUCCESS(Status)) {

        goto CopyToLsaSharedMemoryError;
    }

    VmAllocated = TRUE;

    Status = ZwWriteVirtualMemory(
                 LsaProcessHandle,
                 OutputLsaBufferAddress,
                 Buffer,
                 BufferLength,
                 &NumberOfBytesWritten
                 );

    if (!NT_SUCCESS(Status)) {

        goto CopyToLsaSharedMemoryError;
    }

    *LsaBufferAddress = OutputLsaBufferAddress;
    return(Status);

CopyToLsaSharedMemoryError:

    //
    // If we allocated memory, free it.
    //

    if ( VmAllocated ) {

        RegionSize = 0;

        SecondaryStatus = ZwFreeVirtualMemory(
                              LsaProcessHandle,
                              &OutputLsaBufferAddress,
                              &RegionSize,
                              MEM_RELEASE
                              );

        ASSERT(NT_SUCCESS(SecondaryStatus));
    }

    return(Status);
}


BOOLEAN
SepQueueWorkItem(
    IN PSEP_LSA_WORK_ITEM LsaWorkItem,
    IN BOOLEAN ForceQueue
    )

/*++

Routine Description:

    Puts the passed work item on the queue to be passed to LSA,
    and returns the state of the queue upon arrival.

Arguments:

    LsaWorkItem - Pointer to the work item to be queued.

    ForceQueue - Indicate that this item is not to be discarded
        because of a full queue.

Return Value:

    TRUE - The item was successfully queued.

    FALSE - The item was not queued and must be discarded.

--*/

{
    BOOLEAN rc = TRUE;
    BOOLEAN StartExThread = FALSE ;

    PAGED_CODE();

    SepLockLsaQueue();

    //
    // See if LSA has died. If it has then just return with an error.
    //
    if (SepAdtLsaDeadEvent != NULL) {
        rc = FALSE;
        goto Exit;
    }

    if (SepAdtDiscardingAudits && !ForceQueue) {

        if (SepAdtCurrentListLength < SepAdtMinListLength) {

            //
            // We need to generate an audit saying how many audits we've
            // discarded.
            //
            // Since we have the mutex protecting the Audit queue, we don't
            // have to worry about anyone coming along and logging an
            // audit.  But *we* can, since a mutex may be acquired recursively.
            //
            // Since we are so protected, turn off the SepAdtDiscardingAudits
            // flag here so that we don't come through this path again.
            //

            SepAdtDiscardingAudits = FALSE;

            SepAdtGenerateDiscardAudit();

            //
            // We must assume that that worked, so clear the discard count.
            //

            SepAdtCountEventsDiscarded = 0;

            //
            // Our 'audits discarded' audit is now on the queue,
            // continue logging the one we started with.
            //

        } else {

            //
            // We are not yet below our low water mark.  Toss
            // this audit and increment the discard count.
            //

            SepAdtCountEventsDiscarded++;
            rc = FALSE;
            goto Exit;
        }
    }

    if (SepAdtCurrentListLength < SepAdtMaxListLength || ForceQueue) {

        InsertTailList(&SepLsaQueue, &LsaWorkItem->List);

        if (++SepAdtCurrentListLength == 1) {

            StartExThread = TRUE ;
        }

    } else {

        //
        // There is no room for this audit on the queue,
        // so change our state to 'discarding' and tell
        // the caller to toss this audit.
        //

        SepAdtDiscardingAudits = TRUE;

        rc = FALSE;
    }

Exit:

    SepUnlockLsaQueue();

    if ( StartExThread )
    {
        ExInitializeWorkItem( &SepExWorkItem.WorkItem,
                              (PWORKER_THREAD_ROUTINE) SepRmCallLsa,
                              &SepExWorkItem
                              );

        ExQueueWorkItem( &SepExWorkItem.WorkItem, DelayedWorkQueue );
    }

    return( rc );
}



PSEP_LSA_WORK_ITEM
SepDequeueWorkItem(
    VOID
    )

/*++

Routine Description:

    Removes the top element of the SepLsaQueue and returns the
    next element if there is one, NULL otherwise.

Arguments:

    None.

Return Value:

    A pointer to the next SEP_LSA_WORK_ITEM, or NULL.

--*/

{
    PSEP_LSA_WORK_ITEM OldWorkQueueItem;

    PAGED_CODE();

    SepLockLsaQueue();

    OldWorkQueueItem = (PSEP_LSA_WORK_ITEM)RemoveHeadList(&SepLsaQueue);
    OldWorkQueueItem->List.Flink = NULL;

    SepAdtCurrentListLength--;

    if (IsListEmpty( &SepLsaQueue )) {
        //
        // If LSA has died and the RM thread is waiting till we finish up. Notify it that we are all done
        //
        if (SepAdtLsaDeadEvent != NULL) {
            KeSetEvent (SepAdtLsaDeadEvent, 0, FALSE);
        }
        SepUnlockLsaQueue();

        ExFreePool( OldWorkQueueItem );
        return( NULL );
    }

    //
    // We know there's something on the queue now, so we
    // can unlock it.
    //

    SepUnlockLsaQueue();

    ExFreePool( OldWorkQueueItem );

    return((PSEP_LSA_WORK_ITEM)(&SepLsaQueue)->Flink);
}

