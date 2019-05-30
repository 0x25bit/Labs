/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    internal.c

Abstract:

    This module contains the internal subroutines used by the I/O system.

--*/

#include "iomgr.h"
#pragma hdrstop
#include <ioevent.h>
#include <hdlsblk.h>
#include <hdlsterm.h>

#pragma warning(disable:4221)   // cannot be initialized using address of automatic variable
#pragma warning(disable:4204)   // non-constant aggregate initializer

#if defined(_X86_)

VOID
RtlAssert(
    IN PVOID FailedAssertion,
    IN PVOID FileName,
    IN ULONG LineNumber,
    IN PCHAR Message OPTIONAL
    );

#endif

#define IsFileLocal( FileObject ) ( !((FileObject)->DeviceObject->Characteristics & FILE_REMOTE_DEVICE) )

#define IO_MAX_ALLOCATE_IRP_TRIES   30*60    // Try for 7 minutes
#define IO_INFINITE_RETRIES         -1       // Try for ever

#ifdef _WIN64
#define IopKernelPointerBit 0x8000000000000000i64
#else
#define IopKernelPointerBit 0x80000000
#endif

typedef LINK_TRACKING_INFORMATION FILE_VOLUMEID_WITH_TYPE, *PFILE_VOLUMEID_WITH_TYPE;

typedef struct _TRACKING_BUFFER {
    FILE_TRACKING_INFORMATION TrackingInformation;
    UCHAR Buffer[256];
} TRACKING_BUFFER, *PTRACKING_BUFFER;

typedef struct _REMOTE_LINK_BUFFER {
    REMOTE_LINK_TRACKING_INFORMATION TrackingInformation;
    UCHAR Buffer[256];
} REMOTE_LINK_BUFFER, *PREMOTE_LINK_BUFFER;

PIRP IopDeadIrp;

NTSTATUS
IopResurrectDriver(
    PDRIVER_OBJECT DriverObject
    );

VOID
IopUserRundown(
    IN PKAPC Apc
    );

VOID
IopMarshalIds(
    OUT PTRACKING_BUFFER TrackingBuffer,
    IN  PFILE_VOLUMEID_WITH_TYPE  TargetVolumeId,
    IN  PFILE_OBJECTID_BUFFER  TargetObjectId,
    IN  PFILE_TRACKING_INFORMATION TrackingInfo
    );

VOID
IopUnMarshalIds(
    IN  FILE_TRACKING_INFORMATION * TrackingInformation,
    OUT FILE_VOLUMEID_WITH_TYPE * TargetVolumeId,
    OUT GUID * TargetObjectId,
    OUT GUID * TargetMachineId
    );

NTSTATUS
IopBootLogToFile(
    PUNICODE_STRING String
    );

VOID
IopCopyBootLogRegistryToFile(
    VOID
    );

VOID
IopRaiseHardError(
    IN PVOID NormalContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    );
VOID
IopApcHardError(
    IN PVOID StartContext
    );

PVPB
IopMountInitializeVpb(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PDEVICE_OBJECT  AttachedDevice,
    IN  ULONG           RawMountOnly
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, IopAbortRequest)
#pragma alloc_text(PAGE, IopAcquireFileObjectLock)
#pragma alloc_text(PAGE, IopAllocateIrpCleanup)
#pragma alloc_text(PAGE, IopCallDriverReinitializationRoutines)
#pragma alloc_text(PAGE, IopCancelAlertedRequest)
#pragma alloc_text(PAGE, IopCheckGetQuotaBufferValidity)
#pragma alloc_text(PAGE, IopConnectLinkTrackingPort)
#pragma alloc_text(PAGE, IopDeallocateApc)
#pragma alloc_text(PAGE, IopExceptionCleanup)
#pragma alloc_text(PAGE, IopGetDriverNameFromKeyNode)
#pragma alloc_text(PAGE, IopGetFileInformation)
#pragma alloc_text(PAGE, IopGetRegistryKeyInformation)
#pragma alloc_text(PAGE, IopGetRegistryValue)
#pragma alloc_text(PAGE, IopGetRegistryValues)
#pragma alloc_text(PAGE, IopGetSetObjectId)
#pragma alloc_text(PAGE, IopGetVolumeId)
#pragma alloc_text(PAGE, IopInvalidateVolumesForDevice)
#pragma alloc_text(PAGE, IopIsSameMachine)
#pragma alloc_text(PAGE, IopLoadDriver)
#pragma alloc_text(PAGE, IopLoadFileSystemDriver)
#pragma alloc_text(PAGE, IopLoadUnloadDriver)
#pragma alloc_text(PAGE, IopMountVolume)
#pragma alloc_text(PAGE, IopMarshalIds)
#pragma alloc_text(PAGE, IopOpenLinkOrRenameTarget)
#pragma alloc_text(PAGE, IopOpenRegistryKey)
#pragma alloc_text(PAGE, IopQueryXxxInformation)
#pragma alloc_text(PAGE, IopRaiseHardError)
#pragma alloc_text(PAGE, IopApcHardError)
#pragma alloc_text(PAGE, IopRaiseInformationalHardError)
#pragma alloc_text(PAGE, IopReadyDeviceObjects)
#pragma alloc_text(PAGE, IopReferenceDriverObjectByName)
#pragma alloc_text(PAGE, IopUnMarshalIds)
#pragma alloc_text(PAGE, IopSendMessageToTrackService)
#pragma alloc_text(PAGE, IopSetEaOrQuotaInformationFile)
#pragma alloc_text(PAGE, IopSetRemoteLink)
#pragma alloc_text(PAGE, IopStartApcHardError)
#pragma alloc_text(PAGE, IopSynchronousApiServiceTail)
#pragma alloc_text(PAGE, IopSynchronousServiceTail)
#pragma alloc_text(PAGE, IopTrackLink)
#pragma alloc_text(PAGE, IopUserCompletion)
#pragma alloc_text(PAGE, IopUserRundown)
#pragma alloc_text(PAGE, IopXxxControlFile)
#pragma alloc_text(PAGE, IopLookupBusStringFromID)
#pragma alloc_text(PAGE, IopSafebootDriverLoad)
#pragma alloc_text(PAGE, IopInitializeBootLogging)
#pragma alloc_text(PAGE, IopBootLog)
#pragma alloc_text(PAGE, IopCopyBootLogRegistryToFile)
#pragma alloc_text(PAGE, IopBootLogToFile)
#pragma alloc_text(PAGE, IopHardErrorThread)
#pragma alloc_text(PAGE, IopGetBasicInformationFile)
#pragma alloc_text(PAGE, IopBuildFullDriverPath)
#pragma alloc_text(PAGE, IopInitializeIrpStackProfiler)
#if defined(_WIN64)
#pragma alloc_text(PAGE, IopIsNotNativeDriverImage)
#pragma alloc_text(PAGE, IopCheckIfNotNativeDriver)
#pragma alloc_text(PAGE, IopLogBlockedDriverEvent)
#endif
#endif






VOID
IopAbortRequest(
    IN PKAPC Apc
    )

/*++

Routine Description:

    This routine is invoked to abort an I/O request.  It is invoked during the
    rundown of a thread.

Arguments:

    Apc - Pointer to the kernel APC structure.  This structure is contained
        within the I/O Request Packet (IRP) itself.

Return Value:

    None.

--*/

{
    PAGED_CODE();

    //
    // Invoke the normal special kernel APC routine.
    //

    IopCompleteRequest( Apc,
                        &Apc->NormalRoutine,
                        &Apc->NormalContext,
                        &Apc->SystemArgument1,
                        &Apc->SystemArgument2 );
}

NTSTATUS
IopAcquireFileObjectLock(
    IN PFILE_OBJECT FileObject,
    IN KPROCESSOR_MODE RequestorMode,
    IN BOOLEAN Alertable,
    OUT PBOOLEAN Interrupted
    )

/*++

Routine Description:

    This routine is invoked to acquire the lock for a file object whenever
    there is contention and obtaining the fast lock for the file failed.

Arguments:

    FileObject - Pointer to the file object whose lock is to be acquired.

    RequestorMode - Processor access mode of the caller.

    Alertable - Indicates whether or not the lock should be obtained in an
        alertable manner.

    Interrupted - A variable to receive a BOOLEAN that indicates whether or
        not the attempt to acquire the lock was interrupted by an alert or
        an APC.

Return Value:

    The function status is the final status of the operation.

--*/
{
    NTSTATUS status;

    PAGED_CODE();

    //
    // Assume that the function will not be interrupted by an alert or an
    // APC while attempting to acquire the lock.
    //

    *Interrupted = FALSE;

    //
    // Loop attempting to acquire the lock for the file object.
    //

    InterlockedIncrement ((PLONG) &FileObject->Waiters);

    for (;;) {
        if (!FileObject->Busy) {

            //
            // The file object appears to be un-owned, try to acquire it
            //

            if ( InterlockedExchange( (PLONG) &FileObject->Busy, (ULONG) TRUE ) == FALSE ) {

                //
                // Object was acquired. Remove our count and return success
                //

                ObReferenceObject(FileObject);
                InterlockedDecrement ((PLONG) &FileObject->Waiters);
                return STATUS_SUCCESS;
            }
        }

        //
        // Wait for the event that indicates that the thread that currently
        // owns the file object has released it.
        //

        status = KeWaitForSingleObject( &FileObject->Lock,
                                        Executive,
                                        RequestorMode,
                                        Alertable,
                                        (PLARGE_INTEGER) NULL );

        //
        // If the above wait was interrupted, then indicate so and return.
        // Before returning, however, check the state of the ownership of
        // the file object itself.  If it is not currently owned (the busy
        // flag is clear), then check to see whether or not there are any
        // other waiters.  If so, then set the event to the signaled state
        // again so that they wake up and check the state of the busy flag.
        //

        if (status == STATUS_USER_APC || status == STATUS_ALERTED) {
            InterlockedDecrement ((PLONG) &FileObject->Waiters);

            if (!FileObject->Busy  &&  FileObject->Waiters) {
                KeSetEvent( &FileObject->Lock, 0, FALSE );

            }
            *Interrupted = TRUE;
            return status;
        }
    }
}


VOID
IopAllocateIrpCleanup(
    IN PFILE_OBJECT FileObject,
    IN PKEVENT EventObject OPTIONAL
    )

/*++

Routine Description:

    This routine is invoked internally by those system services that attempt
    to allocate an IRP and fail.  This routine cleans up the file object
    and any event object that has been references and releases any locks
    that were taken out.

Arguments:

    FileObject - Pointer to the file object being worked on.

    EventObject - Optional pointer to a referenced event to be dereferenced.

Return Value:

    None.

--*/

{
    PAGED_CODE();

    //
    // Begin by dereferencing the event, if one was specified.
    //

    if (ARGUMENT_PRESENT( EventObject )) {
        ObDereferenceObject( EventObject );
    }

    //
    // Release the synchronization semaphore if it is currently held and
    // dereference the file object.
    //

    if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
        IopReleaseFileObjectLock( FileObject );
    }

    ObDereferenceObject( FileObject );

    return;
}

PIRP
IopAllocateIrpMustSucceed(
    IN CCHAR StackSize
    )

/*++

Routine Description:

    This routine is invoked to allocate an IRP when there are no appropriate
    packets remaining on the look-aside list, and no memory was available
    from the general non-paged pool, and yet, the code path requiring the
    packet has no way of backing out and simply returning an error.  There-
    fore, it must allocate an IRP.  Hence, this routine is called to allocate
    that packet.

Arguments:

    StackSize - Supplies the number of IRP I/O stack locations that the
        packet must have when allocated.

Return Value:

    A pointer to the allocated I/O Request Packet.

--*/

{
    PIRP irp;
    LONG  numTries;
    LARGE_INTEGER interval;

    //
    // Attempt to allocate the IRP normally and failing that,
    // wait a second and try again.
    //

    numTries = IO_INFINITE_RETRIES;

    irp = IoAllocateIrp(StackSize, FALSE);

    while (!irp && numTries)  {

        interval.QuadPart = -1000 * 1000 * 10; // 10 Msec.
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        irp = IoAllocateIrp(StackSize, FALSE);
        if (numTries != IO_INFINITE_RETRIES) {
            numTries--;
        }
    }


    return irp;
}

VOID
IopApcHardError(
    IN PVOID StartContext
    )

/*++

Routine Description:

    This function is invoked when we need to do a hard error pop-up, but the
    Irp's originating thread is at APC level, ie. IoPageRead.  We in a special
    purpose thread that will go away when the user responds to the pop-up.

Arguments:

    StartContext - Startup context, contains a IOP_APC_HARD_ERROR_PACKET.

Return Value:

    None.

--*/

{
    PIOP_APC_HARD_ERROR_PACKET packet;

    packet = StartContext;

    IopRaiseHardError( packet->Irp, packet->Vpb, packet->RealDeviceObject );
        
    ExFreePool( packet );
}


VOID
IopCancelAlertedRequest(
    IN PKEVENT Event,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine is invoked when a synchronous I/O operation that is blocked in
    the I/O system needs to be canceled because the thread making the request has
    either been alerted because it is going away or because of a CTRL/C.  This
    routine carefully attempts to work its way out of the current operation so
    that local events or other local data will not be accessed once the service
    being interrupted returns.

Arguments:

    Event - The address of a kernel event that will be set to the Signaled state
        by I/O completion when the request is complete.

    Irp - Pointer to the I/O Request Packet (IRP) representing the current request.

Return Value:

    None.

--*/

{
    KIRQL irql;
    LARGE_INTEGER deltaTime;
    BOOLEAN canceled;

    PAGED_CODE();

    //
    // Begin by blocking special kernel APCs so that the request cannot
    // complete.
    //

    KeRaiseIrql( APC_LEVEL, &irql );

    //
    // Check the state of the event to determine whether or not the
    // packet has already been completed.
    //

    if (KeReadStateEvent( Event ) == 0) {

        //
        // The packet has not been completed, so attempt to cancel it.
        //

        canceled = IoCancelIrp( Irp );

        KeLowerIrql( irql );

        if (canceled) {

            //
            // The packet had a cancel routine, so it was canceled.  Loop,
            // waiting for the packet to complete.  This should occur almost
            // immediately.
            //

            deltaTime.QuadPart = - 10 * 1000 * 10;

            while (KeReadStateEvent( Event ) == 0) {

                KeDelayExecutionThread( KernelMode, FALSE, &deltaTime );

            }

        } else {

            //
            // The packet did not have a cancel routine, so simply wait for
            // the event to be set to the Signaled state.  This will save
            // CPU time by not looping, since it is not known when the packet
            // will actually complete.  Note, however, that the cancel flag
            // is set in the packet, so should a driver examine the flag
            // at some point in the future, it will immediately stop
            // processing the request.
            //

            (VOID) KeWaitForSingleObject( Event,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          (PLARGE_INTEGER) NULL );

        }

    } else {

        //
        // The packet has already been completed, so simply lower the
        // IRQL back to its original value and exit.
        //

        KeLowerIrql( irql );

    }
}

NTSTATUS
IopCheckGetQuotaBufferValidity(
    IN PFILE_GET_QUOTA_INFORMATION QuotaBuffer,
    IN ULONG QuotaLength,
    OUT PULONG_PTR ErrorOffset
    )

/*++

Routine Description:

    This routine checks the validity of the specified get quota buffer to
    guarantee that its format is proper, no fields hang over, that it is
    not recursive, etc.

Arguments:

    QuotaBuffer - Pointer to the buffer containing the get quota structure
        array to be checked.

    QuotaLength - Specifies the length of the quota buffer.

    ErrorOffset - A variable to receive the offset of the offending entry
        in the quota buffer if an error is incurred.  This variable is only
        valid if an error occurs.

Return Value:

    The function value is STATUS_SUCCESS if the get quota buffer contains a
    valid, properly formed list, otherwise STATUS_QUOTA_LIST_INCONSISTENT.

--*/

{

#define GET_OFFSET_LENGTH( CurrentSid, SidBase ) ( (ULONG) ((PCHAR) CurrentSid - (PCHAR) SidBase) )

    LONG tempLength;
    LONG entrySize;
    PFILE_GET_QUOTA_INFORMATION sids;

    PAGED_CODE();

    //
    // Walk the buffer and ensure that its format is valid.  That is, ensure
    // that it does not walk off the end of the buffer, is not recursive, etc.
    //

    sids = QuotaBuffer;
    tempLength = QuotaLength;

    for (;;) {

        //
        // Ensure that the current entry is valid.
        //

        if ((tempLength < (LONG) (FIELD_OFFSET(FILE_GET_QUOTA_INFORMATION, Sid.SubAuthority) +
                                  sizeof (sids->Sid.SubAuthority))) ||
            !RtlValidSid( &sids->Sid)) {

            *ErrorOffset = GET_OFFSET_LENGTH( sids, QuotaBuffer );
            return STATUS_QUOTA_LIST_INCONSISTENT;
        }

        //
        // Get the size of the current entry in the buffer.
        //

        entrySize = FIELD_OFFSET( FILE_GET_QUOTA_INFORMATION, Sid ) + RtlLengthSid( (&sids->Sid) );

        if (sids->NextEntryOffset) {

            //
            // There is another entry in the buffer and it must be longword
            // aligned.  Ensure that the offset indicates that it is.  If it
            // isn't, return an invalid parameter status.
            //

            if (entrySize > (LONG) sids->NextEntryOffset ||
                sids->NextEntryOffset & (sizeof( ULONG ) - 1)) {
                *ErrorOffset = GET_OFFSET_LENGTH( sids, QuotaBuffer );
                return STATUS_QUOTA_LIST_INCONSISTENT;

            } else {

                //
                // There is another entry in the buffer, so account for the
                // size of the current entry in the length and get a pointer
                // to the next entry.
                //

                tempLength -= sids->NextEntryOffset;
                if (tempLength < 0) {
                    *ErrorOffset = GET_OFFSET_LENGTH( sids, QuotaBuffer );
                    return STATUS_QUOTA_LIST_INCONSISTENT;
                }
                sids = (PFILE_GET_QUOTA_INFORMATION) ((PCHAR) sids + sids->NextEntryOffset);
            }

        } else {

            //
            // There are no other entries in the buffer.  Simply account for
            // the overall buffer length according to the size of the current
            // entry and exit the loop.
            //

            tempLength -= entrySize;
            break;
        }
    }

    //
    // All of the entries in the buffer have been processed.  Check to see
    // whether the overall buffer length went negative.  If so, return an
    // error.
    //

    if (tempLength < 0) {
        *ErrorOffset = GET_OFFSET_LENGTH( sids, QuotaBuffer );
        return STATUS_QUOTA_LIST_INCONSISTENT;
    }

    //
    // The format of the get quota buffer was correct, so simply return a
    // success status code.
    //

    return STATUS_SUCCESS;
}


VOID
IopCompleteUnloadOrDelete(
    IN PDEVICE_OBJECT DeviceObject,
    IN BOOLEAN OnCleanStack,
    IN KIRQL Irql
    )

/*++

Routine Description:

    This routine is invoked when the reference count on a device object
    transitions to a zero and the driver is mark for unload or device has
    been marked for delete. This means that it may be possible to actually
    unload the driver or delete the device object.  If all
    of the devices have a reference count of zero, then the driver is
    actually unloaded.  Note that in order to ensure that this routine is
    not invoked twice, at the same time, on two different processors, the
    I/O database spin lock is still held at this point.

Arguments:

    DeviceObject - Supplies a pointer to one of the driver's device objects,
        namely the one whose reference count just went to zero.

    OnCleanStack - Indicates whether the current thread is in the middle a
                   driver operation.

    Irql - Specifies the IRQL of the processor at the time that the I/O
        database lock was acquired.

Return Value:

    None.

--*/

{
    PDRIVER_OBJECT driverObject;
    PDEVICE_OBJECT deviceObject;
    PDEVICE_OBJECT baseDeviceObject;
    PDEVICE_OBJECT attachedDeviceObject;
    PDEVOBJ_EXTENSION deviceExtension;

    BOOLEAN unload = TRUE;

    driverObject = DeviceObject->DriverObject;

    if (DeviceObject->DeviceObjectExtension->ExtensionFlags & DOE_REMOVE_PENDING) {

        //
        // Run some tests to determine if it is an appropriate time to notify
        // PnP that all file objects in the attachment chain have gone away.
        //

        baseDeviceObject = IopGetDeviceAttachmentBase( DeviceObject );
        deviceExtension = baseDeviceObject->DeviceObjectExtension;

        ASSERT(deviceExtension->DeviceNode != NULL);

        //
        // baseDeviceObject is a PDO, this is a PnP stack.  See if
        // an IRP_MN_REMOVE_DEVICE is pending.
        //

        //
        // PnP wants to be notified as soon as all refcounts on all devices in
        // this attachment chain go away.
        //

        attachedDeviceObject = baseDeviceObject;
        while (attachedDeviceObject != NULL) {

            if (attachedDeviceObject->ReferenceCount != 0) {

                //
                // At least one device object in the attachment chain has
                // an outstanding open.
                //

                KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, Irql );

                return;
            }
            attachedDeviceObject = attachedDeviceObject->AttachedDevice;
        }

        //
        // Now one more time changing DOE_REMOVE_PENDING to
        // DOE_REMOVE_PROCESSED.
        //

        attachedDeviceObject = baseDeviceObject;
        while (attachedDeviceObject != NULL) {

            deviceExtension = attachedDeviceObject->DeviceObjectExtension;

            deviceExtension->ExtensionFlags &= ~DOE_REMOVE_PENDING;
            deviceExtension->ExtensionFlags |= DOE_REMOVE_PROCESSED;

            attachedDeviceObject = attachedDeviceObject->AttachedDevice;
        }

        //
        // It is time to give PnP the notification it was waiting for.  We have
        // to release the spinlock before doing so.
        //

        KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, Irql );

        IopChainDereferenceComplete( baseDeviceObject, OnCleanStack );

        return;
    }

    if (DeviceObject->DeviceObjectExtension->ExtensionFlags & DOE_DELETE_PENDING) {

        if ((DeviceObject->DeviceObjectExtension->ExtensionFlags &
            DOE_UNLOAD_PENDING) == 0 ||
            driverObject->Flags & DRVO_UNLOAD_INVOKED) {

            unload = FALSE;
        }

        //
        // If another device is attached to this device, inform the former's
        // driver that the device is being deleted.
        //

        if (DeviceObject->AttachedDevice) {
            PFAST_IO_DISPATCH fastIoDispatch = DeviceObject->AttachedDevice->DriverObject->FastIoDispatch;
            PDEVICE_OBJECT attachedDevice = DeviceObject->AttachedDevice;

            //
            // Increment the device reference count so the detach routine
            // does not recurse back to here.
            //

            DeviceObject->ReferenceCount++;

            KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, Irql );

            if (fastIoDispatch &&
                fastIoDispatch->SizeOfFastIoDispatch > FIELD_OFFSET( FAST_IO_DISPATCH, FastIoDetachDevice ) &&
                fastIoDispatch->FastIoDetachDevice) {
                (fastIoDispatch->FastIoDetachDevice)( attachedDevice, DeviceObject );
            }

            Irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );

            //
            // Restore the reference count value.
            //

            DeviceObject->ReferenceCount--;

            if (DeviceObject->AttachedDevice ||
                DeviceObject->ReferenceCount != 0) {


                KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, Irql );
                return;
            }
        }

        KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, Irql );

        //
        // Deallocate the memory for the security descriptor that was allocated
        // for this device object.
        //

        if (DeviceObject->SecurityDescriptor != (PSECURITY_DESCRIPTOR) NULL) {
            ObDereferenceSecurityDescriptor( DeviceObject->SecurityDescriptor, 1 );
        }

        //
        // Remove this device object from the driver object's list.
        //

        IopInsertRemoveDevice( DeviceObject->DriverObject, DeviceObject, FALSE );

        //
        // Finally, dereference the object so it is deleted.
        //

        ObDereferenceObject( DeviceObject );

        //
        // Return if the unload does not need to be done.
        //

        if (!unload) {
            return;
        }

        //
        // Reacquire the spin lock make sure the unload routine does has
        // not been called.
        //

        Irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );

        if (driverObject->Flags & DRVO_UNLOAD_INVOKED) {

            //
            // Some other thread is doing the unload, release the lock and return.
            //

            KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, Irql );
            return;
        }
    }

    //
    // Scan the list of device objects for this driver, looking for a
    // non-zero reference count.  If any reference count is non-zero, then
    // the driver may not be unloaded.
    //

    deviceObject = driverObject->DeviceObject;

    while (deviceObject) {
        if (deviceObject->ReferenceCount || deviceObject->AttachedDevice ||
            deviceObject->DeviceObjectExtension->ExtensionFlags & (DOE_DELETE_PENDING | DOE_REMOVE_PENDING)) {
            unload = FALSE;
            break;
        }
        deviceObject = deviceObject->NextDevice;
    }

    //
    // If this is a base filesystem driver and we still have device objects
    // skip the unload.
    //

    if (driverObject->Flags & DRVO_BASE_FILESYSTEM_DRIVER && driverObject->DeviceObject) {
        unload = FALSE;
    }

    if (unload) {
        driverObject->Flags |= DRVO_UNLOAD_INVOKED;
    }

    KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, Irql );

    //
    // If the reference counts for all of the devices is zero, then this
    // driver can now be unloaded.
    //

    if (unload) {
        LOAD_PACKET loadPacket;

        KeInitializeEvent( &loadPacket.Event, NotificationEvent, FALSE );
        loadPacket.DriverObject = driverObject;

        if (OnCleanStack) {

             IopLoadUnloadDriver(&loadPacket);

        } else {

            ExInitializeWorkItem( &loadPacket.WorkQueueItem,
                                  IopLoadUnloadDriver,
                                  &loadPacket );
            ExQueueWorkItem( &loadPacket.WorkQueueItem, DelayedWorkQueue );
            (VOID) KeWaitForSingleObject( &loadPacket.Event,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          (PLARGE_INTEGER) NULL );
        }

        ObMakeTemporaryObject( driverObject );
        ObDereferenceObject( driverObject );
    }
}

VOID
IopCompletePageWrite(
    IN PKAPC Apc,
    IN PKNORMAL_ROUTINE *NormalRoutine,
    IN PVOID *NormalContext,
    IN PVOID *SystemArgument1,
    IN PVOID *SystemArgument2
    )

/*++

Routine Description:

    This routine executes as a special kernel APC routine in the context of
    the Modified Page Writer (MPW) system thread when an out-page operation
    has completed.

    This routine performs the following tasks:

        o   The I/O status is copied.

        o   The Modified Page Writer's APC routine is invoked.

Arguments:

    Apc - Supplies a pointer to kernel APC structure.

    NormalRoutine - Supplies a pointer to a pointer to the normal function
        that was specified when the APC was initialized.

    NormalContext - Supplies a pointer to a pointer to an arbitrary data
        structure that was specified when the APC was initialized.

    SystemArgument1 - Supplies a pointer to an argument that contains an
        argument that is unused by this routine.

    SystemArgument2 - Supplies a pointer to an argument that contains an
        argument that is unused by this routine.

Return Value:

    None.

--*/

{
    PIRP irp;
    PIO_APC_ROUTINE apcRoutine;
    PVOID apcContext;
    PIO_STATUS_BLOCK ioStatus;

    UNREFERENCED_PARAMETER( NormalRoutine );
    UNREFERENCED_PARAMETER( NormalContext );
    UNREFERENCED_PARAMETER( SystemArgument1 );
    UNREFERENCED_PARAMETER( SystemArgument2 );

    //
    // Begin by getting the address of the I/O Request Packet from the APC.
    //

    irp = CONTAINING_RECORD( Apc, IRP, Tail.Apc );

    //
    // If this I/O operation did not complete successfully through the
    // dispatch routine of the driver, then drop everything on the floor
    // now and return to the original call point in the MPW.
    //

    if (!irp->PendingReturned && NT_ERROR( irp->IoStatus.Status )) {
        IoFreeIrp( irp );
        return;
    }

    //
    // Copy the I/O status from the IRP into the caller's I/O status block.
    //

    *irp->UserIosb = irp->IoStatus;

    //
    // Copy the pertinent information from the I/O Request Packet into locals
    // and free it.
    //

    apcRoutine = irp->Overlay.AsynchronousParameters.UserApcRoutine;
    apcContext = irp->Overlay.AsynchronousParameters.UserApcContext;
    ioStatus = irp->UserIosb;

    IoFreeIrp( irp );

    //
    // Finally, invoke the MPW's APC routine.
    //

    apcRoutine( apcContext, ioStatus, 0 );

    return;
}


VOID
IopCompleteRequest(
    IN PKAPC Apc,
    IN PKNORMAL_ROUTINE *NormalRoutine,
    IN PVOID *NormalContext,
    IN PVOID *SystemArgument1,
    IN PVOID *SystemArgument2
    )

/*++

Routine Description:

    This routine executes as a special kernel APC routine in the context of
    the thread which originally requested the I/O operation which is now
    being completed.

    This routine performs the following tasks:

        o   A check is made to determine whether the specified request ended
            with an error status.  If so, and the error code qualifies as one
            which should be reported to an error port, then an error port is
            looked for in the thread/process.   If one exists, then this routine
            will attempt to set up an LPC to it.  Otherwise, it will attempt to
            set up an LPC to the system error port.

        o   Copy buffers.

        o   Free MDLs.

        o   Copy I/O status.

        o   Set event, if any and dereference if appropriate.

        o   Dequeue the IRP from the thread queue as pending I/O request.

        o   Queue APC to thread, if any.

        o   If no APC is to be queued, then free the packet now.


Arguments:

    Apc - Supplies a pointer to kernel APC structure.

    NormalRoutine - Supplies a pointer to a pointer to the normal function
        that was specified when the APC was initialized.

    NormalContext - Supplies a pointer to a pointer to an arbitrary data
        structure that was specified when the APC was initialized.

    SystemArgument1 - Supplies a pointer to an argument that contains the
        address of the original file object for this I/O operation.

    SystemArgument2 - Supplies a pointer to an argument that contains an
        argument that is used by this routine only in the case of STATUS_REPARSE.

Return Value:

    None.

--*/
{
#define SynchronousIo( Irp, FileObject ) (  \
    (Irp->Flags & IRP_SYNCHRONOUS_API) ||   \
    (FileObject == NULL ? 0 : FileObject->Flags & FO_SYNCHRONOUS_IO) )

    PIRP irp;
    PMDL mdl, nextMdl;
    PETHREAD thread;
    PFILE_OBJECT fileObject;
    NTSTATUS    status;

    UNREFERENCED_PARAMETER( NormalRoutine );
    UNREFERENCED_PARAMETER( NormalContext );

    //
    // Begin by getting the address of the I/O Request Packet.  Also, get
    // the address of the current thread and the address of the original file
    // object for this I/O operation.
    //

    irp = CONTAINING_RECORD( Apc, IRP, Tail.Apc );
    thread = PsGetCurrentThread();
    fileObject = (PFILE_OBJECT) *SystemArgument1;

    IOVP_COMPLETE_REQUEST(Apc, SystemArgument1, SystemArgument2);

    //
    // Ensure that the packet is not being completed with a minus one.  This
    // is apparently a common problem in some drivers, and has no meaning
    // as a status code.
    //

    ASSERT( irp->IoStatus.Status != 0xffffffff );

    //
    // See if we need to do the name transmogrify work.
    //

    if ( *SystemArgument2 != NULL ) {

        PREPARSE_DATA_BUFFER reparseBuffer = NULL;

        //
        // The IO_REPARSE_TAG_MOUNT_POINT tag needs attention.
        //

        if ( irp->IoStatus.Status == STATUS_REPARSE &&
             irp->IoStatus.Information == IO_REPARSE_TAG_MOUNT_POINT ) {

            reparseBuffer = (PREPARSE_DATA_BUFFER) *SystemArgument2;

            ASSERT( reparseBuffer->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT );
            ASSERT( reparseBuffer->ReparseDataLength < MAXIMUM_REPARSE_DATA_BUFFER_SIZE );
            ASSERT( reparseBuffer->Reserved < MAXIMUM_REPARSE_DATA_BUFFER_SIZE );

            IopDoNameTransmogrify( irp,
                                   fileObject,
                                   reparseBuffer );
        }
    }

    //
    // Check to see whether there is any data in a system buffer which needs
    // to be copied to the caller's buffer.  If so, copy the data and then
    // free the system buffer if necessary.
    //

    if (irp->Flags & IRP_BUFFERED_IO) {

        //
        // Copy the data if this was an input operation.  Note that no copy
        // is performed if the status indicates that a verify operation is
        // required, or if the final status was an error-level severity.
        //

        if (irp->Flags & IRP_INPUT_OPERATION  &&
            irp->IoStatus.Status != STATUS_VERIFY_REQUIRED &&
            !NT_ERROR( irp->IoStatus.Status )) {

            //
            // Copy the information from the system buffer to the caller's
            // buffer.  This is done with an exception handler in case
            // the operation fails because the caller's address space
            // has gone away, or it's protection has been changed while
            // the service was executing.
            //

            status = STATUS_SUCCESS;

            try {
                RtlCopyMemory( irp->UserBuffer,
                               irp->AssociatedIrp.SystemBuffer,
                               irp->IoStatus.Information );
            } except(IopExceptionFilter(GetExceptionInformation(), &status)) {

                //
                // An exception occurred while attempting to copy the
                // system buffer contents to the caller's buffer.  Set
                // a new I/O completion status.
                // If the status is a special one set by Mm then we need to
                // return here and the operation will be retried in
                // IoRetryIrpCompletions.
                //

                if (status == STATUS_MULTIPLE_FAULT_VIOLATION) {
                    irp->Tail.Overlay.OriginalFileObject = fileObject;  /* Wiped out by APC  overlay */
                    irp->Flags |= IRP_RETRY_IO_COMPLETION;
                    return;
                }
                irp->IoStatus.Status = GetExceptionCode();
            }
        }

        //
        // Free the buffer if needed.
        //

        if (irp->Flags & IRP_DEALLOCATE_BUFFER) {
            ExFreePool( irp->AssociatedIrp.SystemBuffer );
        }
    }

    irp->Flags &= ~(IRP_DEALLOCATE_BUFFER|IRP_BUFFERED_IO);

    //
    // If there is an MDL (or MDLs) associated with this I/O request,
    // Free it (them) here.  This is accomplished by walking the MDL list
    // hanging off of the IRP and deallocating each MDL encountered.
    //

    if (irp->MdlAddress) {
        for (mdl = irp->MdlAddress; mdl != NULL; mdl = nextMdl) {
            nextMdl = mdl->Next;
            IoFreeMdl( mdl );
        }
    }

    irp->MdlAddress = NULL;

    //
    // Check to see whether or not the I/O operation actually completed.  If
    // it did, then proceed normally.  Otherwise, cleanup everything and get
    // out of here.
    //

    if (!NT_ERROR( irp->IoStatus.Status ) ||
        (NT_ERROR( irp->IoStatus.Status ) &&
        irp->PendingReturned &&
        !SynchronousIo( irp, fileObject ))) {

        PVOID port = NULL;
        PVOID key = NULL;
        BOOLEAN createOperation = FALSE;

        //
        // If there is an I/O completion port object associated w/this request,
        // save it here so that the file object can be dereferenced.
        //

        if (fileObject && fileObject->CompletionContext) {
            port = fileObject->CompletionContext->Port;
            key = fileObject->CompletionContext->Key;
        }

        //
        // Copy the I/O status from the IRP into the caller's I/O status
        // block. This is done using an exception handler in case the caller's
        // virtual address space for the I/O status block was deleted or
        // its protection was changed to readonly.  Note that if the I/O
        // status block cannot be written, the error is simply ignored since
        // there is no way to tell the caller that something went wrong.
        // This is, of course, by definition, since the I/O status block
        // is where the caller will attempt to look for errors in the first
        // place!
        //

        status = STATUS_SUCCESS;

        try {

            //
            // Since HasOverlappedIoCompleted and GetOverlappedResult only
            // look at the Status field of the UserIosb to determine if the
            // IRP has completed, the Information field must be written
            // before the Status field.
            //

#if defined(_WIN64)
            PIO_STATUS_BLOCK32    UserIosb32;

            //
            // If the caller passes a 32 bit IOSB the ApcRoutine has the LSB set to 1
            //
            if (IopIsIosb32(irp->Overlay.AsynchronousParameters.UserApcRoutine)) {
                UserIosb32 = (PIO_STATUS_BLOCK32)irp->UserIosb;

                UserIosb32->Information = (ULONG)irp->IoStatus.Information;
                KeMemoryBarrierWithoutFence ();
                *((volatile NTSTATUS *) &UserIosb32->Status) = irp->IoStatus.Status;
            } else {
                irp->UserIosb->Information = irp->IoStatus.Information;
                KeMemoryBarrierWithoutFence ();
                *((volatile NTSTATUS *) &irp->UserIosb->Status) = irp->IoStatus.Status;
            }
#else
            irp->UserIosb->Information = irp->IoStatus.Information;
            KeMemoryBarrierWithoutFence ();
            *((volatile NTSTATUS *) &irp->UserIosb->Status) = irp->IoStatus.Status;
#endif  /*_WIN64 */

        } except(IopExceptionFilter(GetExceptionInformation(), &status)) {

            //
            // An exception was incurred attempting to write the caller's
            // I/O status block.  Simply continue executing as if nothing
            // ever happened since nothing can be done about it anyway.
            // If the status is a multiple fault status, this is a special
            // status sent by the Memory manager. Mark the IRP and return from
            // this routine. Mm will call us back later and we will retry this
            // operation (IoRetryIrpCompletions)
            //
            if (status == STATUS_MULTIPLE_FAULT_VIOLATION) {
                irp->Tail.Overlay.OriginalFileObject = fileObject;  /* Wiped out by APC  overlay */
                irp->Flags |= IRP_RETRY_IO_COMPLETION;
                return;
            }
        }


        //
        // Determine whether the caller supplied an event that needs to be set
        // to the Signaled state.  If so, then set it; otherwise, set the event
        // in the file object to the Signaled state.
        //
        // It is possible for the event to have been specified as a PKEVENT if
        // this was an I/O operation hand-built for an FSP or an FSD, or
        // some other types of operations such as synchronous I/O APIs.  In
        // any of these cases, the event was not referenced since it is not an
        // object manager event, so it should not be dereferenced.
        //
        // Also, it is possible for there not to be a file object for this IRP.
        // This occurs when an FSP is doing I/O operations to a device driver on
        // behalf of a process doing I/O to a file.  The file object cannot be
        // dereferenced if this is the case.  If this operation was a create
        // operation then the object should not be dereferenced either.  This
        // is because the reference count must be one or it will go away for
        // the caller (not much point in making an object that just got created
        // go away).
        //

        if (irp->UserEvent) {
            (VOID) KeSetEvent( irp->UserEvent, 0, FALSE );
            if (fileObject) {
                if (!(irp->Flags & IRP_SYNCHRONOUS_API)) {
                    ObDereferenceObject( irp->UserEvent );
                }
                if (fileObject->Flags & FO_SYNCHRONOUS_IO && !(irp->Flags & IRP_OB_QUERY_NAME)) {
                    (VOID) KeSetEvent( &fileObject->Event, 0, FALSE );
                    fileObject->FinalStatus = irp->IoStatus.Status;
                }
                if (irp->Flags & IRP_CREATE_OPERATION) {
                    createOperation = TRUE;
                    irp->Overlay.AsynchronousParameters.UserApcRoutine = (PIO_APC_ROUTINE) NULL;
                }
            }
        } else if (fileObject) {
            (VOID) KeSetEvent( &fileObject->Event, 0, FALSE );
            fileObject->FinalStatus = irp->IoStatus.Status;
            if (irp->Flags & IRP_CREATE_OPERATION) {
                createOperation = TRUE;
                irp->Overlay.AsynchronousParameters.UserApcRoutine = (PIO_APC_ROUTINE) NULL;
            }
        }

        //
        // If this is normal I/O, update the transfer count for this process.
        //

        if (!(irp->Flags & IRP_CREATE_OPERATION)) {
            if (irp->Flags & IRP_READ_OPERATION) {
                IopUpdateReadTransferCount( (ULONG) irp->IoStatus.Information );
            } else if (irp->Flags & IRP_WRITE_OPERATION) {
                IopUpdateWriteTransferCount( (ULONG) irp->IoStatus.Information );
            } else {
                //
                // If the information field contains a pointer then skip the update.
                // Some PNP IRPs contain this.
                //

                if (!((ULONG_PTR) irp->IoStatus.Information & IopKernelPointerBit)) {
                    IopUpdateOtherTransferCount( (ULONG) irp->IoStatus.Information );
                }
            }
        }

        //
        // Dequeue the packet from the thread's pending I/O request list.
        //

        IopDequeueThreadIrp( irp );

        //
        // If the caller requested an APC, queue it to the thread.  If not, then
        // simply free the packet now.
        //

#ifdef  _WIN64
        //
        // For 64 bit systems clear the LSB field of the ApcRoutine that indicates whether
        // the IOSB is a 32 bit IOSB or a 64 bit IOSB.
        //
        irp->Overlay.AsynchronousParameters.UserApcRoutine =
          (PIO_APC_ROUTINE)((LONG_PTR)(irp->Overlay.AsynchronousParameters.UserApcRoutine) & ~1);
#endif

        if (irp->Overlay.AsynchronousParameters.UserApcRoutine) {
            KeInitializeApc( &irp->Tail.Apc,
                             &thread->Tcb,
                             CurrentApcEnvironment,
                             IopUserCompletion,
                             (PKRUNDOWN_ROUTINE) IopUserRundown,
                             (PKNORMAL_ROUTINE) irp->Overlay.AsynchronousParameters.UserApcRoutine,
                             irp->RequestorMode,
                             irp->Overlay.AsynchronousParameters.UserApcContext );

            KeInsertQueueApc( &irp->Tail.Apc,
                              irp->UserIosb,
                              NULL,
                              2 );

        } else if (port && irp->Overlay.AsynchronousParameters.UserApcContext) {

            //
            // If there is a completion context associated w/this I/O operation,
            // send the message to the port. Tag completion packet as an Irp.
            //

            irp->Tail.CompletionKey = key;
            irp->Tail.Overlay.PacketType = IopCompletionPacketIrp;

            KeInsertQueue( (PKQUEUE) port,
                           &irp->Tail.Overlay.ListEntry );

        } else {

            //
            // Free the IRP now since it is no longer needed.
            //

            IoFreeIrp( irp );
        }

        if (fileObject && !createOperation) {

            //
            // Dereference the file object now.
            //

            ObDereferenceObjectDeferDelete( fileObject );
        }

    } else {

        if (irp->PendingReturned && fileObject) {

            //
            // This is an I/O operation that completed as an error for
            // which a pending status was returned and the I/O operation
            // is synchronous.  For this case, the I/O system is waiting
            // on behalf of the caller.  If the reason that the I/O was
            // synchronous is that the file object was opened for synchronous
            // I/O, then the event associated with the file object is set
            // to the signaled state.  If the I/O operation was synchronous
            // because this is a synchronous API, then the event is set to
            // the signaled state.
            //
            // Note also that the status must be returned for both types
            // of synchronous I/O.  If this is a synchronous API, then the
            // I/O system supplies its own status block so it can simply
            // be written;  otherwise, the I/O system will obtain the final
            // status from the file object itself.
            //

            if (irp->Flags & IRP_SYNCHRONOUS_API) {
                *irp->UserIosb = irp->IoStatus;
                if (irp->UserEvent) {
                    (VOID) KeSetEvent( irp->UserEvent, 0, FALSE );
                } else {
                    (VOID) KeSetEvent( &fileObject->Event, 0, FALSE );
                }
            } else {
                fileObject->FinalStatus = irp->IoStatus.Status;
                (VOID) KeSetEvent( &fileObject->Event, 0, FALSE );
            }
        }

        //
        // The operation was incomplete.  Perform the general cleanup.  Note
        // that everything is basically dropped on the floor without doing
        // anything.  That is:
        //
        //     IoStatusBlock - Do nothing.
        //     Event - Dereference without setting to Signaled state.
        //     FileObject - Dereference without setting to Signaled state.
        //     ApcRoutine - Do nothing.
        //

        if (fileObject) {
            if (!(irp->Flags & IRP_CREATE_OPERATION)) {
                ObDereferenceObjectDeferDelete( fileObject );
            }
        }

        if (irp->UserEvent &&
            fileObject &&
            !(irp->Flags & IRP_SYNCHRONOUS_API)) {
            ObDereferenceObject( irp->UserEvent );
        }

        IopDequeueThreadIrp( irp );
        IoFreeIrp( irp );
    }
}

VOID
IopConnectLinkTrackingPort(
    IN PVOID Parameter
    )

/*++

Routine Description:

    This routine is invoked to connect to the user-mode link tracking service's
    LPC port.  It makes a connection which establishes a handle to the port,
    and then creates a referenced object pointer to the port.

Arguments:

    Parameter - Pointer to the link tracking packet.

Return Value:

    None.


--*/

{
    #define MESSAGE_SIZE    ( (2 * sizeof( FILE_VOLUMEID_WITH_TYPE )) + \
                            sizeof( FILE_OBJECTID_BUFFER ) +              \
                            sizeof( GUID ) + \
                            sizeof( NTSTATUS ) + \
                            sizeof( ULONG ) )

    PLINK_TRACKING_PACKET ltp;
    HANDLE serviceHandle;
    NTSTATUS status;

    PAGED_CODE();
    //
    // Begin by getting a pointer to the link tracking packet.
    //

    ltp = (PLINK_TRACKING_PACKET) Parameter;


    //
    // Ensure that the port has not already been opened.
    //

    status = STATUS_SUCCESS;
    if (!IopLinkTrackingServiceObject) {

        UNICODE_STRING portName;
        ULONG maxMessageLength;
        SECURITY_QUALITY_OF_SERVICE dynamicQos;

        if (KeReadStateEvent( IopLinkTrackingServiceEvent )) {

            //
            // Attempt to open a handle to the port.
            //

            //
            // Set up the security quality of service parameters to use over the
            // port.  Use the most efficient (least overhead) which is dynamic
            // rather than static tracking.
            //

            dynamicQos.ImpersonationLevel = SecurityImpersonation;
            dynamicQos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
            dynamicQos.EffectiveOnly = TRUE;

            //
            // Generate the string structure for describing the port.
            //

            RtlInitUnicodeString( &portName, L"\\Security\\TRKWKS_PORT" );

            status = NtConnectPort( &serviceHandle,
                                    &portName,
                                    &dynamicQos,
                                    (PPORT_VIEW) NULL,
                                    (PREMOTE_PORT_VIEW) NULL,
                                    &maxMessageLength,
                                    (PVOID) NULL,
                                    (PULONG) NULL );
            if (NT_SUCCESS( status )) {
                if (maxMessageLength >= MESSAGE_SIZE) {
                    status = ObReferenceObjectByHandle( serviceHandle,
                                                        0,
                                                        LpcPortObjectType,
                                                        KernelMode,
                                                        &IopLinkTrackingServiceObject,
                                                        NULL );
                    NtClose( serviceHandle );
                } else {
                    NtClose( serviceHandle );
                    status = STATUS_INVALID_PARAMETER;
                }
            }

        } else {

            //
            // The service has not been started so the port does not exist.
            //

            status = STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }


    //
    // Return final status and wake the caller up.
    //
    ltp->FinalStatus = status;
    KeSetEvent( &ltp->Event, 0, FALSE );
}

VOID
IopDisassociateThreadIrp(
    VOID
    )

/*++

Routine Description:

    This routine is invoked when the I/O requests for a thread are being
    cancelled, but there is a packet at the end of the thread's queue that
    has not been completed for such a long period of time that it has timed
    out.  It is this routine's responsibility to try to disassociate that
    IRP with this thread.

Arguments:

    None.

Return Value:

    None.

--*/

{
    KIRQL irql;
    KIRQL spIrql;
    PIRP irp;
    PETHREAD thread;
    PLIST_ENTRY entry;
    PIO_STACK_LOCATION irpSp;
    PDEVICE_OBJECT deviceObject;
    PIO_ERROR_LOG_PACKET errorLogEntry;

    //
    // Begin by ensuring that the packet has not already been removed from
    // the thread's queue.
    //

    KeRaiseIrql( APC_LEVEL, &irql );

    thread = PsGetCurrentThread();

    //
    // If there are no packets on the IRP list, then simply return now.
    // All of the packets have been fully completed, so the caller will also
    // simply return to its caller.
    //

    if (IsListEmpty( &thread->IrpList )) {
        KeLowerIrql( irql );
        return;
    }

    //
    // Get a pointer to the first packet on the queue, and begin examining
    // it.  Note that because the processor is at raised IRQL, and because
    // the packet can only be removed in the context of the currently
    // executing thread, that it is not possible for the packet to be removed
    // from the list.  On the other hand, it IS possible for the packet to
    // be queued to the thread's APC list at this point, and this must be
    // blocked/synchronized in order to examine the request.
    //
    // Begin, therefore, by acquiring the I/O completion spinlock, so that
    // the packet can be safely examined.
    //

    spIrql = KeAcquireQueuedSpinLock( LockQueueIoCompletionLock );

    //
    // Check to see whether or not the packet has been completed (that is,
    // queued to the current thread).  If not, change threads.
    //

    entry = thread->IrpList.Flink;
    irp = CONTAINING_RECORD( entry, IRP, ThreadListEntry );

    if (irp->CurrentLocation == irp->StackCount + 2) {

        //
        // The request has just gone through enough of completion that
        // queueing it to the thread is inevitable.  Simply release the
        // lock and return.
        //

        KeReleaseQueuedSpinLock( LockQueueIoCompletionLock, spIrql );
        KeLowerIrql( irql );
        return;
    }

    //
    // The packet has been located, and it is not going through completion
    // at this point.  Switch threads, so that it will not complete through
    // this thread, remove the request from this thread's queue, and release
    // the spinlock.  Final processing of the IRP will occur when I/O
    // completion notices that there is no thread associated with the
    // request.  It will essentially drop the I/O on the floor.
    //
    // Also, while the request is still held, attempt to determine on which
    // device object the operation is being performed.
    //

    IopDeadIrp = irp;

    irp->Tail.Overlay.Thread = (PETHREAD) NULL;
    entry = RemoveHeadList( &thread->IrpList );

    // Initialize the thread entry. Otherwise the assertion in IoFreeIrp
    // called via IopDeadIrp will fail.
    InitializeListHead (&(irp)->ThreadListEntry);

    irpSp = IoGetCurrentIrpStackLocation( irp );
    if (irp->CurrentLocation <= irp->StackCount) {
        deviceObject = irpSp->DeviceObject;
    } else {
        deviceObject = (PDEVICE_OBJECT) NULL;
    }
    KeReleaseQueuedSpinLock( LockQueueIoCompletionLock, spIrql );
    KeLowerIrql( irql );

    //
    // If a device object could be identified then try to write to the event log about this
    // device object.
    //

    if (deviceObject) {
        errorLogEntry = IoAllocateErrorLogEntry(deviceObject, sizeof(IO_ERROR_LOG_PACKET));
        if (errorLogEntry) {
            errorLogEntry->ErrorCode = IO_DRIVER_CANCEL_TIMEOUT;
            IoWriteErrorLogEntry(errorLogEntry);
        }
    }

    return;
}

VOID
IopDeallocateApc(
    IN PKAPC Apc,
    IN PKNORMAL_ROUTINE *NormalRoutine,
    IN PVOID *NormalContext,
    IN PVOID *SystemArgument1,
    IN PVOID *SystemArgument2
    )

/*++

Routine Description:

    This routine is invoked to deallocate an APC that was used to queue a
    request to a target thread.  It simple deallocates the APC.

Arguments:

    Apc - Supplies a pointer to kernel APC structure.

    NormalRoutine - Supplies a pointer to a pointer to the normal function
        that was specified when the APC was initialized.

    NormalContext - Supplies a pointer to a pointer to an arbitrary data
        structure that was specified when the APC was initialized.

    SystemArgument1, SystemArgument2 - Supplies a set of two pointers to
        two arguments that contain untyped data.

Return Value:

    None.

--*/

{
    UNREFERENCED_PARAMETER( NormalRoutine );
    UNREFERENCED_PARAMETER( NormalContext );
    UNREFERENCED_PARAMETER( SystemArgument1 );
    UNREFERENCED_PARAMETER( SystemArgument2 );

    PAGED_CODE();

    //
    // Free the APC.
    //

    ExFreePool( Apc );
}

VOID
IopDropIrp(
    IN PIRP Irp,
    IN PFILE_OBJECT FileObject
    )

/*++

Routine Description:

    This routine attempts to drop everything about the specified IRP on the
    floor.

Arguments:

    Irp - Supplies the I/O Request Packet to be completed to the bit bucket.

    FileObject - Supplies the file object for which the I/O Request Packet was
        bound.

Return Value:

    None.

--*/

{
    PMDL mdl;
    PMDL nextMdl;

    //
    // Free the resources associated with the IRP.
    //

    if (Irp->Flags & IRP_DEALLOCATE_BUFFER) {
        ExFreePool( Irp->AssociatedIrp.SystemBuffer );
    }

    if (Irp->MdlAddress) {
        for (mdl = Irp->MdlAddress; mdl; mdl = nextMdl) {
            nextMdl = mdl->Next;
            IoFreeMdl( mdl );
        }
    }

    if (Irp->UserEvent &&
        FileObject &&
        !(Irp->Flags & IRP_SYNCHRONOUS_API)) {
        ObDereferenceObject( Irp->UserEvent );
    }

    if (FileObject && !(Irp->Flags & IRP_CREATE_OPERATION)) {
        ObDereferenceObjectEx( FileObject, 1 );
    }

    //
    // Finally, free the IRP itself.
    //

    IoFreeIrp( Irp );
}

LONG
IopExceptionFilter(
    IN PEXCEPTION_POINTERS ExceptionPointer,
    OUT PNTSTATUS ExceptionCode
    )

/*++

Routine Description:

    This routine is invoked when an exception occurs to determine whether or
    not the exception was due to an error that caused an in-page error status
    code exception to be raised.  If so, then this routine changes the code
    in the exception record to the actual error code that was originally
    raised.

Arguments:

    ExceptionPointer - Pointer to the exception record.

    ExceptionCode - Variable to receive actual exception code.

Return Value:

    The function value indicates that the exception handler is to be executed.

--*/

{
    //
    // Simply check for an in-page error status code and, if the conditions
    // are right, replace it with the actual status code.
    //

    *ExceptionCode = ExceptionPointer->ExceptionRecord->ExceptionCode;
    if (*ExceptionCode == STATUS_IN_PAGE_ERROR &&
        ExceptionPointer->ExceptionRecord->NumberParameters >= 3) {
        *ExceptionCode = (LONG) ExceptionPointer->ExceptionRecord->ExceptionInformation[2];
    }

    //
    // Translate alignment warnings into alignment errors.
    //

    if (*ExceptionCode == STATUS_DATATYPE_MISALIGNMENT) {
        *ExceptionCode = STATUS_DATATYPE_MISALIGNMENT_ERROR;
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

VOID
IopExceptionCleanup(
    IN PFILE_OBJECT FileObject,
    IN PIRP Irp,
    IN PKEVENT EventObject OPTIONAL,
    IN PKEVENT KernelEvent OPTIONAL
    )

/*++

Routine Description:

    This routine performs generalized cleanup for the I/O system services when
    an exception occurs during caller parameter processing.  This routine
    performs the following steps:

        o   If a system buffer was allocated it is freed.

        o   If an MDL was allocated it is freed.

        o   The IRP is freed.

        o   If the file object is opened for synchronous I/O, the semaphore
            is released.

        o   If an event object was referenced it is dereferenced.

        o   If a kernel event was allocated, free it.

        o   The file object is dereferenced.

Arguments:

    FileObject - Pointer to the file object currently being worked on.

    Irp - Pointer to the IRP allocated to handle the I/O request.

    EventObject - Optional pointer to a referenced event object.

    KernelEvent - Optional pointer to an allocated kernel event.

Return Value:

    None.

--*/

{
    PAGED_CODE();

    //
    // If a system buffer was allocated from nonpaged pool, free it.
    //

    if (Irp->AssociatedIrp.SystemBuffer != NULL) {
        ExFreePool( Irp->AssociatedIrp.SystemBuffer );
    }

    //
    // If an MDL was allocated, free it.
    //

    if (Irp->MdlAddress != NULL) {
        IoFreeMdl( Irp->MdlAddress );
    }

    //
    // Free the I/O Request Packet.
    //

    IoFreeIrp( Irp );

    //
    // Finally, release the synchronization semaphore if it is currently
    // held, dereference the event if one was specified, free the kernel
    // event if one was allocated, and dereference the file object.
    //

    if (FileObject->Flags & FO_SYNCHRONOUS_IO) {
        IopReleaseFileObjectLock( FileObject );
    }

    if (ARGUMENT_PRESENT( EventObject )) {
        ObDereferenceObject( EventObject );
    }

    if (ARGUMENT_PRESENT( KernelEvent )) {
        ExFreePool( KernelEvent );
    }

    ObDereferenceObject( FileObject );

    return;
}

VOID
IopFreeIrpAndMdls(
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine frees the specified I/O Request Packet and all of its Memory
    Descriptor Lists.

Arguments:

    Irp - Pointer to the I/O Request Packet to be freed.

Return Value:

    None.

--*/

{
    PMDL mdl;
    PMDL nextMdl;

    //
    // If there are any MDLs that need to be freed, free them now.
    //

    for (mdl = Irp->MdlAddress; mdl != (PMDL) NULL; mdl = nextMdl) {
        nextMdl = mdl->Next;
        IoFreeMdl( mdl );
    }

    //
    // Free the IRP.
    //

    IoFreeIrp( Irp );
    return;
}

NTSTATUS
IopGetDriverNameFromKeyNode(
    IN HANDLE KeyHandle,
    OUT PUNICODE_STRING DriverName
    )

/*++

Routine Description:

    Given a handle to a driver service list key in the registry, return the
    name that represents the Object Manager name space string that should
    be used to locate/create the driver object.

Arguments:

    KeyHandle - Supplies a handle to driver service entry in the registry.

    DriverName - Supplies a Unicode string descriptor variable in which the
        name of the driver is returned.

Return Value:

    The function value is the final status of the operation.

--*/

{
    PKEY_VALUE_FULL_INFORMATION keyValueInformation;
    PKEY_BASIC_INFORMATION keyBasicInformation;
    ULONG keyBasicLength;
    NTSTATUS status;

    PAGED_CODE();

    //
    // Get the optional object name for this driver from the value for this
    // key.  If one exists, then its name overrides the default name of the
    // driver.
    //

    status = IopGetRegistryValue( KeyHandle,
                                  L"ObjectName",
                                  &keyValueInformation );

    if (NT_SUCCESS( status )) {

        PWSTR src, dst;
        ULONG i, nameLength;

        //
        // The driver entry specifies an object name.  This overrides the
        // default name for the driver.  Use this name to open the driver
        // object.
        //

        if ((keyValueInformation->DataLength == 0) || (keyValueInformation->DataLength == 1)) {
            ExFreePool( keyValueInformation );
            return STATUS_ILL_FORMED_SERVICE_ENTRY;
        }

        DriverName->Length = (USHORT) (keyValueInformation->DataLength - sizeof( WCHAR ));
        DriverName->MaximumLength = (USHORT) keyValueInformation->DataLength;

        src = (PWSTR) ((PUCHAR) keyValueInformation + keyValueInformation->DataOffset);
        dst = (PWSTR) keyValueInformation;
        nameLength = DriverName->Length / sizeof( WCHAR );

        for (i = nameLength; i; i--) {
            *dst++ = *src++;
        }

        DriverName->Buffer = (PWSTR) keyValueInformation;

    } else {

        PULONG driverType;
        PWSTR baseObjectName;
        UNICODE_STRING remainderName;

        //
        // The driver node does not specify an object name, so determine
        // what the default name for the driver object should be based on
        // the information in the key.
        //

        status = IopGetRegistryValue( KeyHandle,
                                      L"Type",
                                      &keyValueInformation );
        if (!NT_SUCCESS( status ) || !keyValueInformation->DataLength) {

            //
            // There must be some type of "Type" associated with this driver,
            // either DRIVER or FILE_SYSTEM.  Otherwise, this node is ill-
            // formed.
            //

            if (NT_SUCCESS( status )) {
                ExFreePool( keyValueInformation );
            }

            return STATUS_ILL_FORMED_SERVICE_ENTRY;
        }

        //
        // Now determine whether the type of this entry is a driver or a
        // file system.  Begin by assuming that it is a device driver.
        //

        baseObjectName = L"\\Driver\\";
        DriverName->Length = 8*2;

        driverType = (PULONG) ((PUCHAR) keyValueInformation + keyValueInformation->DataOffset);

        if (*driverType == FileSystemType ||
            *driverType == RecognizerType) {
            baseObjectName = L"\\FileSystem\\";
            DriverName->Length = 12*2;
        }

        //
        // Get the name of the key that is being used to describe this
        // driver.  This will return just the last component of the name
        // string, which can be used to formulate the name of the driver.
        //

        status = ZwQueryKey( KeyHandle,
                             KeyBasicInformation,
                             (PVOID) NULL,
                             0,
                             &keyBasicLength );

        keyBasicInformation = ExAllocatePool( NonPagedPool, keyBasicLength );
        if (!keyBasicInformation) {
            ExFreePool( keyValueInformation );
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        status = ZwQueryKey( KeyHandle,
                             KeyBasicInformation,
                             keyBasicInformation,
                             keyBasicLength,
                             &keyBasicLength );
        if (!NT_SUCCESS( status )) {
            ExFreePool( keyBasicInformation );
            ExFreePool( keyValueInformation );
            return status;
        }

        //
        // Allocate a buffer from pool that is large enough to contain the
        // entire name string of the driver object.
        //

        DriverName->MaximumLength = (USHORT) (DriverName->Length + keyBasicInformation->NameLength);
        DriverName->Buffer = ExAllocatePool( NonPagedPool,
                                            DriverName->MaximumLength );
        if (!DriverName->Buffer) {
            ExFreePool( keyBasicInformation );
            ExFreePool( keyValueInformation );
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        //
        // Now form the name of the object to be opened.
        //

        DriverName->Length = 0;
        RtlAppendUnicodeToString( DriverName, baseObjectName );
        remainderName.Length = (USHORT) keyBasicInformation->NameLength;
        remainderName.MaximumLength = remainderName.Length;
        remainderName.Buffer = &keyBasicInformation->Name[0];
        RtlAppendUnicodeStringToString( DriverName, &remainderName );
        ExFreePool( keyBasicInformation );
        ExFreePool( keyValueInformation );
    }

    //
    // Finally, simply return to the caller with the name filled in.  Note
    // that the caller must free the buffer pointed to by the Buffer field
    // of the Unicode string descriptor.
    //

    return STATUS_SUCCESS;
}



NTSTATUS
IopGetFileInformation(
    IN PFILE_OBJECT FileObject,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass,
    OUT PVOID FileInformation,
    OUT PULONG ReturnedLength
    )

/*++

Routine Description:

    This routine is invoked to asynchronously obtain the name or other information
    of a file object when the file was opened for synchronous I/O, and the previous mode of the
    caller was kernel mode, and the query was done through the Object Manager.
    In this case, the situation is likely that the Lazy Writer has incurred a
    write error, and it is attempting to obtain the name of the file so that it
    can output a popup.  In doing so, a deadlock can occur because another
    thread has locked the file object synchronous I/O lock.  Hence, this routine
    obtains the name of the file w/o acquiring that lock.

Arguments:

    FileObject - A pointer to the file object whose name is to be queried.

    Length - Supplies the length of the buffer to receive the name.

    FileInformation - A pointer to the buffer to receive the name.

    ReturnedLength - A variable to receive the length of the name returned.

Return Value:

    The status returned is the final completion status of the operation.

--*/

{

    PIRP irp;
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject;
    KEVENT event;
    PIO_STACK_LOCATION irpSp;
    IO_STATUS_BLOCK localIoStatus;

    PAGED_CODE();

    //
    // Reference the file object here so that no special checks need be made
    // in I/O completion to determine whether or not to dereference the file
    // object.
    //

    ObReferenceObject( FileObject );

    //
    // Initialize an event that will be used to synchronize the completion of
    // the query operation.  Note that this is the only way to synchronize this
    // since the file object itself cannot be used since it was opened for
    // synchronous I/O and may be busy.
    //

    KeInitializeEvent( &event, SynchronizationEvent, FALSE );

    //
    // Get the address of the target device object.
    //

    deviceObject = IoGetRelatedDeviceObject( FileObject );

    //
    // Allocate and initialize the I/O Request Packet (IRP) for this operation.
    //

    irp = IoAllocateIrp( deviceObject->StackSize, FALSE );
    if (!irp) {

        //
        // An IRP could not be allocated.  Cleanup and return an appropriate
        // error status code.
        //

        ObDereferenceObject( FileObject );
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->Tail.Overlay.OriginalFileObject = FileObject;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    irp->RequestorMode = KernelMode;

    //
    // Fill in the service independent parameters in the IRP.  Note that the
    // setting of the special query name flag in the packet guarantees that the
    // standard completion for a synchronous file object will not occur because
    // this flag communicates to the I/O completion that it should not do so.
    //

    irp->UserEvent = &event;
    irp->Flags = IRP_SYNCHRONOUS_API | IRP_OB_QUERY_NAME;
    irp->UserIosb = &localIoStatus;
    irp->Overlay.AsynchronousParameters.UserApcRoutine = (PIO_APC_ROUTINE) NULL;

    //
    // Get a pointer to the stack location for the first driver.  This will be
    // used to pass the original function codes and parameters.
    //

    irpSp = IoGetNextIrpStackLocation( irp );
    irpSp->MajorFunction = IRP_MJ_QUERY_INFORMATION;
    irpSp->FileObject = FileObject;

    //
    // Set the system buffer address to the address of the caller's buffer and
    // set the flags so that the buffer is not deallocated.
    //

    irp->AssociatedIrp.SystemBuffer = FileInformation;
    irp->Flags |= IRP_BUFFERED_IO;

    //
    // Copy the caller's parameters to the service-specific portion of the
    // IRP.
    //

    irpSp->Parameters.QueryFile.Length = Length;
    irpSp->Parameters.QueryFile.FileInformationClass = FileInformationClass;

    //
    // Insert the packet at the head of the IRP list for the thread.
    //

    IopQueueThreadIrp( irp );

    //
    // Now simply invoke the driver at its dispatch entry with the IRP.
    //

    status = IoCallDriver( deviceObject, irp );

    //
    // Now get the final status of the operation once the request completes
    // and return the length of the buffer written.
    //

    if (status == STATUS_PENDING) {
        (VOID) KeWaitForSingleObject( &event,
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      (PLARGE_INTEGER) NULL );
        status = localIoStatus.Status;
    }

    *ReturnedLength = (ULONG) localIoStatus.Information;
    return status;
}

BOOLEAN
IopGetMountFlag(
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    This routine is invoked to determine whether or not the specified device
    is mounted.

Arguments:

    DeviceObject - Supplies a pointer to the device object for which the mount
        flag is tested.

Return Value:

    The function value is TRUE if the specified device is mounted, otherwise
    FALSE.


--*/

{
    KIRQL irql;
    BOOLEAN deviceMounted = FALSE;

    //
    // Check to see whether or not the device is mounted.  Note that the caller
    // has probably already looked to see whether or not the device has a VPB
    // outside of owning the lock, so simply get the lock and check it again
    // to start with, rather than checking to see whether or not the device
    // still has a VPB without holding the lock.
    //

    irql = KeAcquireQueuedSpinLock( LockQueueIoVpbLock );
    if (DeviceObject->Vpb) {
        if (DeviceObject->Vpb->Flags & VPB_MOUNTED) {
            deviceMounted = TRUE;
        }
    }
    KeReleaseQueuedSpinLock( LockQueueIoVpbLock, irql );

    return deviceMounted;
}

NTSTATUS
IopGetRegistryKeyInformation(
    IN HANDLE KeyHandle,
    OUT PKEY_FULL_INFORMATION *Information
    )

/*++

Routine Description:

    This routine is invoked to retrieve the full key information for a
    registry key.  This is done by querying the full key information
    of the key with a zero-length buffer to determine the size of the data,
    and then allocating a buffer and actually querying the data into the buffer.

    It is the responsibility of the caller to free the buffer.

Arguments:

    KeyHandle - Supplies the key handle whose full key information is to
        be queried

    Information - Returns a pointer to the allocated data buffer.

Return Value:

    The function value is the final status of the query operation.

--*/

{
    NTSTATUS status;
    PKEY_FULL_INFORMATION infoBuffer;
    ULONG keyInfoLength;

    PAGED_CODE();

    //
    // Figure out how big the data value is so that a buffer of the
    // appropriate size can be allocated.
    //

    status = ZwQueryKey( KeyHandle,
                         KeyFullInformation,
                         (PVOID) NULL,
                         0,
                         &keyInfoLength );
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL) {
        return status;
    }

    //
    // Allocate a buffer large enough to contain the entire key data.
    //

    infoBuffer = ExAllocatePool( NonPagedPool, keyInfoLength );
    if (!infoBuffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Query the full key data for the key.
    //

    status = ZwQueryKey( KeyHandle,
                         KeyFullInformation,
                         infoBuffer,
                         keyInfoLength,
                         &keyInfoLength );
    if (!NT_SUCCESS( status )) {
        ExFreePool( infoBuffer );
        return status;
    }

    //
    // Everything worked, so simply return the address of the allocated
    // buffer to the caller, who is now responsible for freeing it.
    //

    *Information = infoBuffer;
    return STATUS_SUCCESS;
}

NTSTATUS
IopGetRegistryValue(
    IN HANDLE KeyHandle,
    IN PWSTR  ValueName,
    OUT PKEY_VALUE_FULL_INFORMATION *Information
    )

/*++

Routine Description:

    This routine is invoked to retrieve the data for a registry key's value.
    This is done by querying the value of the key with a zero-length buffer
    to determine the size of the value, and then allocating a buffer and
    actually querying the value into the buffer.

    It is the responsibility of the caller to free the buffer.

Arguments:

    KeyHandle - Supplies the key handle whose value is to be queried

    ValueName - Supplies the null-terminated Unicode name of the value.

    Information - Returns a pointer to the allocated data buffer.

Return Value:

    The function value is the final status of the query operation.

--*/

{
    UNICODE_STRING unicodeString;
    NTSTATUS status;
    PKEY_VALUE_FULL_INFORMATION infoBuffer;
    ULONG keyValueLength, guessSize;

    PAGED_CODE();

    RtlInitUnicodeString( &unicodeString, ValueName );

    //
    // Set an initial size to try when loading a key. Note that
    // KeyValueFullInformation already comes with a single WCHAR of data.
    //
    guessSize = (ULONG)(sizeof(KEY_VALUE_FULL_INFORMATION) +
                wcslen(ValueName)*sizeof(WCHAR));

    //
    // Now round up to a natural alignment. This needs to be done because our
    // data member will naturally aligned as well.
    //
    guessSize = (ULONG) ALIGN_POINTER_OFFSET(guessSize);

    //
    // Set the data cache length to a ULONG's worth of data, because most data
    // we read via this function is type REG_DWORD.
    //
    guessSize += sizeof(ULONG);

    infoBuffer = ExAllocatePool(NonPagedPool, guessSize);
    if (!infoBuffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Figure out how big the data value is so that a buffer of the
    // appropriate size can be allocated.
    //

    status = ZwQueryValueKey( KeyHandle,
                              &unicodeString,
                              KeyValueFullInformation,
                              (PVOID) infoBuffer,
                              guessSize,
                              &keyValueLength );
    if (NT_SUCCESS(status)) {

        //
        // First guess worked, bail!
        //
        *Information = infoBuffer;
        return STATUS_SUCCESS;
    }

    ExFreePool(infoBuffer);
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL) {

        ASSERT(!NT_SUCCESS(status));
        return status;
    }

    //
    // Allocate a buffer large enough to contain the entire key data value.
    //

    infoBuffer = ExAllocatePool( NonPagedPool, keyValueLength );
    if (!infoBuffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Query the data for the key value.
    //

    status = ZwQueryValueKey( KeyHandle,
                              &unicodeString,
                              KeyValueFullInformation,
                              infoBuffer,
                              keyValueLength,
                              &keyValueLength );
    if (!NT_SUCCESS( status )) {
        ExFreePool( infoBuffer );
        return status;
    }

    //
    // Everything worked, so simply return the address of the allocated
    // buffer to the caller, who is now responsible for freeing it.
    //

    *Information = infoBuffer;
    return STATUS_SUCCESS;
}

NTSTATUS
IopGetRegistryValues(
    IN HANDLE KeyHandle,
    IN PKEY_VALUE_FULL_INFORMATION *ValueList
    )

/*++

Routine Description:

    This routine is invoked to retrieve the *three* types of data for a
    registry key's.  This is done by calling the IopGetRegistryValue function
    with the three valid key names.

    It is the responsibility of the caller to free the three buffers.

Arguments:

    KeyHandle - Supplies the key handle whose value is to be queried

    ValueList - Pointer to a buffer in which the three pointers to the value
        entries will be stored.

Return Value:

    The function value is the final status of the query operation.

Note:

    The values are stored in the order represented by the I/O query device
    data format.

--*/

{
    NTSTATUS status;

    PAGED_CODE();

    //
    // Zero out all entries initially.
    //

    *ValueList = NULL;
    *(ValueList + 1) = NULL;
    *(ValueList + 2) = NULL;

    //
    // Get the information for each of the three types of entries available.
    // Each time, check if an internal error occurred; If the object name was
    // not found, it only means not data was present, and this does not
    // constitute an error.
    //

    status = IopGetRegistryValue( KeyHandle,
                                  L"Identifier",
                                  ValueList );

    if (!NT_SUCCESS( status ) && (status != STATUS_OBJECT_NAME_NOT_FOUND)) {
        return status;
    }

    status = IopGetRegistryValue( KeyHandle,
                                  L"Configuration Data",
                                  ++ValueList );

    if (!NT_SUCCESS( status ) && (status != STATUS_OBJECT_NAME_NOT_FOUND)) {
        return status;
    }

    status = IopGetRegistryValue( KeyHandle,
                                  L"Component Information",
                                  ++ValueList );

    if (!NT_SUCCESS( status ) && (status != STATUS_OBJECT_NAME_NOT_FOUND)) {
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
IopGetSetObjectId(
    IN PFILE_OBJECT FileObject,
    IN OUT PVOID Buffer,
    IN ULONG Length,
    IN ULONG Function
    )

/*++

Routine Description:

    This routine is invoked to obtain or set the object ID for a file.  If
    one does not exist for the file, then one is created, provided that the
    underlying file system supports object IDs in the first place (query).

Arguments:

    FileObject - Supplies a pointer to the referenced file object whose ID is
        to be returned or set.

    Buffer - A variable to receive the object ID of the file (query) or that
        contains the object ID that is to be set on the file.

    Length - The length of the Buffer.

    Function - The FSCTL to send.
        FSCTL_LMR_GET_LINK_TRACKING_INFORMATION;
        FSCTL_CREATE_OR_GET_OBJECT_ID;
        FSCTL_GET_OBJECT_ID;
        FSCTL_SET_OBJECT_ID_EXTENDED;
        FSCTL_LMR_SET_LINK_TRACKING_INFORMATION;
        FSCTL_SET_OBJECT_ID_EXTENDED;
        FSCTL_SET_OBJECT_ID;
        FSCTL_DELETE_OBJECT_ID;

Return Value:

    The status returned is the final completion status of the operation.

--*/

{
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;
    PIRP irp;
    KEVENT event;
    PIO_STACK_LOCATION irpSp;
    PDEVICE_OBJECT deviceObject;

    PAGED_CODE();

    //
    // Initialize the event structure to synchronize completion of the I/O
    // request.
    //

    KeInitializeEvent( &event,
                       NotificationEvent,
                       FALSE );

    //
    // Build an I/O Request Packet to be sent to the file system driver to get
    // the object ID.
    //

    deviceObject = IoGetRelatedDeviceObject( FileObject );

    irp = IoBuildDeviceIoControlRequest( Function,
                                         deviceObject,
                                         NULL,
                                         0,
                                         NULL,
                                         0,
                                         FALSE,
                                         &event,
                                         &ioStatus );
    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Fill in the remainder of the IRP to retrieve the object ID for the
    // file.
    //

    irp->Flags |= IRP_SYNCHRONOUS_API;
    irp->UserBuffer = Buffer;
    irp->AssociatedIrp.SystemBuffer = Buffer;
    irp->Tail.Overlay.OriginalFileObject = FileObject;

    irpSp = IoGetNextIrpStackLocation( irp );
    irpSp->FileObject = FileObject;
    irpSp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
    irpSp->MinorFunction = IRP_MN_KERNEL_CALL;

    if (Function == FSCTL_LMR_GET_LINK_TRACKING_INFORMATION ||
        Function == FSCTL_CREATE_OR_GET_OBJECT_ID ||
        Function == FSCTL_GET_OBJECT_ID ) {
        irpSp->Parameters.FileSystemControl.OutputBufferLength = Length;
    } else {
        irpSp->Parameters.FileSystemControl.InputBufferLength = Length;
    }

    //
    // Take out another reference to the file object to guarantee that it does
    // not get deleted.
    //

    ObReferenceObject( FileObject );

    //
    // Call the driver to get the request.
    //

    status = IoCallDriver( deviceObject, irp );

    //
    // Synchronize completion of the request.
    //

    if (status == STATUS_PENDING) {
        status = KeWaitForSingleObject( &event,
                                        Executive,
                                        KernelMode,
                                        FALSE,
                                        (PLARGE_INTEGER) NULL );
        status = ioStatus.Status;
    }

    return status;
}

NTSTATUS
IopGetVolumeId(
    IN PFILE_OBJECT FileObject,
    IN OUT PFILE_VOLUMEID_WITH_TYPE ObjectId,
    IN ULONG Length
    )

/*++

Routine Description:

    This routine is invoked by the I/O System link tracking code to obtain the
    volume ID for a file that has been moved or is being moved between volumes
    and potentially between systems.

Arguments:

    FileObject - Supplies the file object for the file.

    ObjectId - A buffer to receive the volume object ID.

    Length - Length of the buffer.

Return Value:

    The final function value is the final completion status of the operation.

--*/

{
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;
    PIRP irp;
    KEVENT event;
    PIO_STACK_LOCATION irpSp;
    PDEVICE_OBJECT deviceObject;
    FILE_FS_OBJECTID_INFORMATION volumeId;

    PAGED_CODE();

#if !DBG
    UNREFERENCED_PARAMETER (Length);
#endif

    ASSERT (Length >= sizeof(GUID));

    //
    // Initialize the event structure to synchronize completion of the I/O
    // request.
    //

    KeInitializeEvent( &event,
                       NotificationEvent,
                       FALSE );

    //
    // Build an I/O Request Packet to be sent to the file system driver to get
    // the volume ID.
    //

    deviceObject = IoGetRelatedDeviceObject( FileObject );

    irp = IoBuildDeviceIoControlRequest( 0,
                                         deviceObject,
                                         NULL,
                                         0,
                                         NULL,
                                         0,
                                         FALSE,
                                         &event,
                                         &ioStatus );
    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Fill in the remainder of the IRP to retrieve the volume ID for the
    // file.
    //

    irp->Flags |= IRP_SYNCHRONOUS_API;
    irp->UserBuffer = &volumeId;
    irp->AssociatedIrp.SystemBuffer = &volumeId;
    irp->Tail.Overlay.OriginalFileObject = FileObject;

    irpSp = IoGetNextIrpStackLocation( irp );
    irpSp->FileObject = FileObject;
    irpSp->MajorFunction = IRP_MJ_QUERY_VOLUME_INFORMATION;
    irpSp->Parameters.QueryVolume.Length = sizeof( volumeId );
    irpSp->Parameters.QueryVolume.FsInformationClass = FileFsObjectIdInformation;

    //
    // Take out another reference to the file object to guarantee that it does
    // not get deleted.
    //

    ObReferenceObject( FileObject );

    //
    // Call the driver to get the request.
    //

    status = IoCallDriver( deviceObject, irp );

    //
    // Synchronize completion of the request.
    //

    if (status == STATUS_PENDING) {
        status = KeWaitForSingleObject( &event,
                                        Executive,
                                        KernelMode,
                                        FALSE,
                                        (PLARGE_INTEGER) NULL );
        status = ioStatus.Status;
    }

    //
    // If the file system returned the volume ID, copy it to the caller's
    // buffer and set the file system tracking type.
    //

    if (NT_SUCCESS( status )) {
        ObjectId->Type = NtfsLinkTrackingInformation;
        RtlCopyMemory( ObjectId->VolumeId,
                       &volumeId.ObjectId,
                       sizeof( GUID ) );
    }

    return status;
}

PIOP_HARD_ERROR_PACKET
IopRemoveHardErrorPacket(
    VOID
    )
{
    PIOP_HARD_ERROR_PACKET  hardErrorPacket;
    KIRQL oldIrql;
    PVOID entry;

#if !DBG && defined(NT_UP)
    UNREFERENCED_PARAMETER (oldIrql);
#endif

    ExAcquireFastLock( &IopHardError.WorkQueueSpinLock, &oldIrql );

    //
    // The work queue structures are now exclusively owned, so remove the
    // first packet from the head of the list.
    //

    entry = RemoveHeadList( &IopHardError.WorkQueue );

    hardErrorPacket = CONTAINING_RECORD( entry,
                                         IOP_HARD_ERROR_PACKET,
                                         WorkQueueLinks );

    IopCurrentHardError = hardErrorPacket;

    ExReleaseFastLock( &IopHardError.WorkQueueSpinLock, oldIrql );

    return hardErrorPacket;
}

BOOLEAN
IopCheckHardErrorEmpty(
    VOID
    )
{
    BOOLEAN MoreEntries;
    KIRQL   oldIrql;

#if !DBG && defined(NT_UP)
    UNREFERENCED_PARAMETER (oldIrql);
#endif

    MoreEntries = TRUE;

    ExAcquireFastLock( &IopHardError.WorkQueueSpinLock, &oldIrql );

    IopCurrentHardError = NULL;

    if ( IsListEmpty( &IopHardError.WorkQueue ) ) {
        IopHardError.ThreadStarted = FALSE;
        MoreEntries = FALSE;
    }

    ExReleaseFastLock( &IopHardError.WorkQueueSpinLock, oldIrql );

    return MoreEntries;
}

VOID
IopHardErrorThread(
    IN PVOID StartContext
    )

/*++

Routine Description:

    This function waits for work on the IopHardErrorQueue, and all calls
    IopRaiseInformationalHardError to actually perform the pop-ups.

Arguments:

    StartContext - Startup context; not used.

Return Value:

    None.

--*/

{
    ULONG parameterPresent;
    ULONG_PTR errorParameter;
    ULONG errorResponse;
    BOOLEAN MoreEntries;
    PIOP_HARD_ERROR_PACKET hardErrorPacket;

    UNREFERENCED_PARAMETER( StartContext );

    PAGED_CODE();
    //
    // Loop, waiting forever for a hard error packet to be sent to this thread.
    // When one is placed onto the queue, wake up, process it, and continue
    // the loop.
    //

    MoreEntries = TRUE;

    do {

        (VOID) KeWaitForSingleObject( &IopHardError.WorkQueueSemaphore,
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      (PLARGE_INTEGER) NULL );

        hardErrorPacket = IopRemoveHardErrorPacket();


        //
        // Simply raise the hard error if the system is ready to accept one.
        //

        errorParameter = (ULONG_PTR) &hardErrorPacket->String;
        parameterPresent = (hardErrorPacket->String.Buffer != NULL);

        if (ExReadyForErrors) {
            (VOID) ExRaiseHardError( hardErrorPacket->ErrorStatus,
                                     parameterPresent,
                                     parameterPresent,
                                     parameterPresent ? &errorParameter : NULL,
                                     OptionOkNoWait,
                                     &errorResponse );
        }

        //
        //  If this was the last entry, exit the thread and mark it as so.
        //

        MoreEntries = IopCheckHardErrorEmpty();

        //
        // Now free the packet and the buffer, if one was specified.
        //

        if (hardErrorPacket->String.Buffer) {
            ExFreePool( hardErrorPacket->String.Buffer );
        }

        ExFreePool( hardErrorPacket );

    } while ( MoreEntries );
}


NTSTATUS
IopInvalidDeviceRequest(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This function is the default dispatch routine for all driver entries
    not implemented by drivers that have been loaded into the system.  Its
    responsibility is simply to set the status in the packet to indicate
    that the operation requested is invalid for this device type, and then
    complete the packet.

Arguments:

    DeviceObject - Specifies the device object for which this request is
        bound.  Ignored by this routine.

    Irp - Specifies the address of the I/O Request Packet (IRP) for this
        request.

Return Value:

    The final status is always STATUS_INVALID_DEVICE_REQUEST.


--*/

{
    UNREFERENCED_PARAMETER( DeviceObject );

    //
    // Simply store the appropriate status, complete the request, and return
    // the same status stored in the packet.
    //

    if ((IoGetCurrentIrpStackLocation(Irp))->MajorFunction == IRP_MJ_POWER) {
        PoStartNextPowerIrp(Irp);
    }
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    IoCompleteRequest( Irp, IO_NO_INCREMENT );
    return STATUS_INVALID_DEVICE_REQUEST;
}

LOGICAL
IopIsSameMachine(
    IN PFILE_OBJECT SourceFile,
    IN HANDLE TargetFile
    )

/*++

Routine Description:

    This routine is invoked to determine whether two file objects that represent
    files on remote machines actually reside on the same physical system.

Arguments:

    SourceFile - Supplies the file object for the first file.

    TargetFile - Supplies the file object for the second file.

Return Value:

    The final function value is TRUE if the files reside on the same machine,
    otherwise FALSE is returned.

--*/

{
    PDEVICE_OBJECT deviceObject;
    PFAST_IO_DISPATCH fastIoDispatch;
    NTSTATUS status = STATUS_NOT_SAME_DEVICE;
    IO_STATUS_BLOCK ioStatus;
    HANDLE target = TargetFile;

    PAGED_CODE();

    //
    // Simply invoke the device I/O control function to determine whether or
    // not the two files are on the same server.  If the fast I/O path does
    // not exist, or the function fails for any reason, then the two files are
    // assumed to not be on the same machine.  Note that this simply means
    // that there will be a performance penalty on open of the target, but
    // the above will only fail if the two files really aren't on the same
    // machine in the first place, or if there's a filter that doesn't under-
    // stand what is being done here.
    //

    deviceObject = IoGetRelatedDeviceObject( SourceFile );

    fastIoDispatch = deviceObject->DriverObject->FastIoDispatch;
    if (fastIoDispatch && fastIoDispatch->FastIoDeviceControl) {
        if (fastIoDispatch->FastIoDeviceControl( SourceFile,
                                                 TRUE,
                                                 (PVOID) &target,
                                                 sizeof( target ),
                                                 (PVOID) NULL,
                                                 0,
                                                 IOCTL_LMR_ARE_FILE_OBJECTS_ON_SAME_SERVER,
                                                 &ioStatus,
                                                 deviceObject )) {
            status = ioStatus.Status;
        }
    }

    return status == STATUS_SUCCESS;
}

NTSTATUS
IopBuildFullDriverPath(
    IN PUNICODE_STRING KeyName,
    IN HANDLE KeyHandle,
    OUT PUNICODE_STRING FullPath
    )
/*++

Routine Description:

    This routine builds up the full path for the driver. If ImagePath is
    specified, use it or else prepend the standard drivers path.

Arguments:

    KeyHandle - Supplies a handle to the driver service node in the registry
        that describes the driver to be loaded.

    CheckForSafeBoot - If TRUE, the driver will be loaded only if it belongs
                       to the list of safe mode OK binaries.

    FullPath - Full driver path is returned in this.

Return Value:

    STATUS_SUCCESS or STATUS_INSUFFICIENT_RESOURCES.

--*/
{
    NTSTATUS                    status;
    PWCHAR                      path, name, ext;
    ULONG                       pathLength, nameLength, extLength;
    PKEY_VALUE_FULL_INFORMATION keyValueInformation;

    FullPath->Length = FullPath->MaximumLength = 0;
    FullPath->Buffer = NULL;
    extLength = nameLength = pathLength = 0;
    keyValueInformation = NULL;
    status = IopGetRegistryValue( KeyHandle,
                                  L"ImagePath",
                                  &keyValueInformation);
    if (NT_SUCCESS(status) && keyValueInformation->DataLength) {

        nameLength = keyValueInformation->DataLength - sizeof(WCHAR);
        name = (PWCHAR)KEY_VALUE_DATA(keyValueInformation);
        if (name[0] != L'\\') {

            path = L"\\SystemRoot\\";
            pathLength = sizeof(L"\\SystemRoot\\") - sizeof(UNICODE_NULL);
        }
        else {
            path = NULL;
        }
        ext = NULL;
    } else {

        nameLength = KeyName->Length;
        name = KeyName->Buffer;
        pathLength = sizeof(L"\\SystemRoot\\System32\\Drivers\\") - sizeof(UNICODE_NULL);
        path = L"\\SystemRoot\\System32\\Drivers\\";
        extLength = sizeof(L".SYS") - sizeof(UNICODE_NULL);
        ext = L".SYS";
    }
    //
    // Allocate storage for the full path.
    //
    FullPath->MaximumLength = (USHORT)(pathLength + nameLength + extLength + sizeof(UNICODE_NULL));
    FullPath->Buffer = ExAllocatePool(PagedPool, FullPath->MaximumLength);
    if (FullPath->Buffer) {

        FullPath->Length = FullPath->MaximumLength - sizeof(UNICODE_NULL);
        //
        // Create the full path by combining path, name and ext.
        //
        if (path) {

            RtlCopyMemory(FullPath->Buffer, path, pathLength);
        }
        if (nameLength) {

            RtlCopyMemory((PUCHAR)FullPath->Buffer + pathLength, name, nameLength);
        }
        if (extLength) {

            RtlCopyMemory((PUCHAR)FullPath->Buffer + pathLength + nameLength, ext, extLength);
        }
        //
        // NULL terminate the full path.
        //
        FullPath->Buffer[FullPath->Length / sizeof(WCHAR)] = UNICODE_NULL;
        status = STATUS_SUCCESS;

    } else {

        FullPath->MaximumLength = 0;
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    //
    // Clean up on the way out.
    //
    if (keyValueInformation) {

        ExFreePool(keyValueInformation);
    }

    return status;
}

#if defined(_WIN64)

#define SYSTEM32_DRIVERS_DIR        (L"\\System32\\drivers\\")
#define SYSTEM32_DRIVERS_DIR_LEN    ((sizeof (SYSTEM32_DRIVERS_DIR) / sizeof (WCHAR)) - 1)

BOOLEAN
IopIsNotNativeDriverImage(
    IN PUNICODE_STRING ImageFileName
    )

/*++

Routine Description:

    This routine reads checks if the file is not a native driver image.

Arguments:

    ImageFileName - Full qualified path name to the driver.

Return Value:

    BOOLEAN. Returns TRUE if the image is not native.
--*/
{
    HANDLE ImageFileHandle;
    IO_STATUS_BLOCK IoStatus;
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Section;
    PVOID ViewBase;
    SIZE_T ViewSize;
    KAPC_STATE ApcState;
    PIMAGE_NT_HEADERS NtHeaders;
    NTSTATUS Status;
    BOOLEAN RetValue;


    RetValue = FALSE;

    //
    // Attempt to open the driver image itself.  If this fails, then the
    // driver image cannot be located, so nothing else matters.
    //

    InitializeObjectAttributes (&ObjectAttributes,
                                ImageFileName,
                                (OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE),
                                NULL,
                                NULL);

    Status = ZwOpenFile (&ImageFileHandle,
                         FILE_EXECUTE,
                         &ObjectAttributes,
                         &IoStatus,
                         FILE_SHARE_READ | FILE_SHARE_DELETE,
                         0);

    if (!NT_SUCCESS (Status)) {
        return RetValue;
    }

    InitializeObjectAttributes (&ObjectAttributes,
                                NULL,
                                (OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE),
                                NULL,
                                NULL);

    Status = ZwCreateSection (&Section,
                              SECTION_MAP_EXECUTE,
                              &ObjectAttributes,
                              NULL,
                              PAGE_EXECUTE,
                              SEC_COMMIT,
                              ImageFileHandle);

    if (!NT_SUCCESS (Status)) {
        ZwClose (ImageFileHandle);
        return RetValue;
    }

    ViewBase = NULL;
    ViewSize = 0;

    //
    // Since callees are not always in the context of the system process,
    // attach here when necessary to guarantee the driver load occurs in a
    // known safe address space to prevent security holes.
    //

    KeStackAttachProcess (&PsInitialSystemProcess->Pcb, &ApcState);

    Status = ZwMapViewOfSection (Section,
                                 NtCurrentProcess (),
                                 (PVOID *)&ViewBase,
                                 0L,
                                 0L,
                                 NULL,
                                 &ViewSize,
                                 ViewShare,
                                 0L,
                                 PAGE_EXECUTE);

    if (!NT_SUCCESS(Status)) {
        KeUnstackDetachProcess (&ApcState);
        ZwClose (Section);
        ZwClose (ImageFileHandle);
        return RetValue;
    }

    try {
        NtHeaders = RtlImageNtHeader (ViewBase);
        if (NtHeaders != NULL) {
            if (NtHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_NATIVE) {
                RetValue = TRUE;
            }
        }
    } except (EXCEPTION_EXECUTE_HANDLER) {
        NOTHING;
    }


    ZwUnmapViewOfSection (NtCurrentProcess (), ViewBase);

    KeUnstackDetachProcess (&ApcState);

    ZwClose (Section);

    ZwClose (ImageFileHandle);

    return RetValue;
}

BOOLEAN
IopCheckIfNotNativeDriver(
    IN NTSTATUS InitialDriverLoadStatus,
    IN PUNICODE_STRING ImageFileName
    )

/*++

Routine Description:

    This routine reads checks to see if the specified file is not a native driver image.

Arguments:

    InitialDriverLoadStatus - NT Status code of the initial driver load failure.

    ImageFileName - Supplies the full path name (including the image name)
        of the image to load.

Return Value:

    BOOLEAN. Returns TRUE if the driver is not native.
--*/
{
    PWCHAR System32DriversPath;

    //
    // Check to see if the driver may exist in the \SystemRoot\SysWow64 directory
    //

    System32DriversPath = ImageFileName->Buffer;
    if (InitialDriverLoadStatus == STATUS_OBJECT_NAME_NOT_FOUND) {
        while (System32DriversPath != NULL) {
            if (_wcsnicmp (System32DriversPath, SYSTEM32_DRIVERS_DIR, SYSTEM32_DRIVERS_DIR_LEN) == 0) {
                wcsncpy (System32DriversPath + 1,
                         L"SysWow64",
                         ((sizeof (L"SysWow64") - sizeof (UNICODE_NULL)) / sizeof (WCHAR)));
                break;
            }
            System32DriversPath = wcsstr ((System32DriversPath + 1), L"\\");
        }

        if (System32DriversPath == NULL) {
            return FALSE;
        }
    }

    return IopIsNotNativeDriverImage (ImageFileName);
}

VOID
IopLogBlockedDriverEvent (
    IN PUNICODE_STRING ImageFileName,
    IN NTSTATUS NtMessageStatus,
    IN NTSTATUS NtErrorStatus)

/*++

Routine Description:

    This routine logs an event to the event log for the specified blocked driver.

Arguments:

    ImageFileName - Supplies the full path name (including the image name)
        of the image to load.
        
    NtMessageStatus - Specifies the status error code for the message to be displayed.
    
    NtErrorStatus - Supplies the status error code to block the driver.

Return Value:

    None.
--*/
{
    PWCHAR DriverName;
    PIO_ERROR_LOG_PACKET LogPacket;
    UCHAR PacketLength;

    //
    // Allocate a packet for the string including the driver name.
    //

    PacketLength =  sizeof (IO_ERROR_LOG_PACKET) + 128;

    LogPacket = (PIO_ERROR_LOG_PACKET) IoAllocateGenericErrorLogEntry (PacketLength);
    
    if (LogPacket != NULL) {

        LogPacket->DumpDataSize = 0;
        LogPacket->NumberOfStrings = 1;
        LogPacket->StringOffset = sizeof (IO_ERROR_LOG_PACKET);
        LogPacket->ErrorCode = NtMessageStatus;
        LogPacket->FinalStatus = NtErrorStatus;
    
        DriverName = (PWCHAR) (LogPacket + 1);
        
        wcsncpy (DriverName, ImageFileName->Buffer, 64);
        DriverName [ 63 ] = UNICODE_NULL;

        IoWriteErrorLogEntry (LogPacket);
    }

    return;
}

#endif

NTSTATUS
IopLoadDriver(
    IN  HANDLE      KeyHandle,
    IN  BOOLEAN     CheckForSafeBoot,
    IN  BOOLEAN     IsFilter,
    OUT NTSTATUS   *DriverEntryStatus
    )
/*++

Routine Description:

    This routine is invoked to load a device or file system driver, either
    during system initialization, or dynamically while the system is running.

Arguments:

    KeyHandle - Supplies a handle to the driver service node in the registry
        that describes the driver to be loaded.

    IsFilter - TRUE if the driver is a WDM filter, FALSE otherwise.

    CheckForSafeBoot - If TRUE, the driver will be loaded only if it belongs
                       to the list of safe mode OK binaries.

    DriverEntryStatus - Receives status returned by DriverEntry(...)

Return Value:

    The function value is the final status of the load operation. If
    STATUS_FAILED_DRIVER_ENTRY is returned, the driver's return value
    is stored in DriverEntryStatus.

Notes:

    Note that this routine closes the KeyHandle before returning.


--*/
{
    NTSTATUS status;
    PLIST_ENTRY nextEntry;
    PKLDR_DATA_TABLE_ENTRY driverEntry;    
    PKEY_BASIC_INFORMATION keyBasicInformation = NULL;
    ULONG keyBasicLength;
    UNICODE_STRING baseName;
    UNICODE_STRING serviceName = {0, 0, NULL};
    OBJECT_ATTRIBUTES objectAttributes;
    PVOID sectionPointer;
    UNICODE_STRING driverName;
    PDRIVER_OBJECT driverObject;
    PIMAGE_NT_HEADERS ntHeaders;
    PVOID imageBaseAddress;
    ULONG_PTR entryPoint;
    HANDLE driverHandle;
    ULONG i;
    POBJECT_NAME_INFORMATION registryPath;
#if DBG
    LARGE_INTEGER stime, etime;
    ULONG dtime;
#endif

    PAGED_CODE();

    driverName.Buffer = (PWSTR) NULL;
    *DriverEntryStatus = STATUS_SUCCESS;
    baseName.Buffer = NULL;

    //
    // Begin by formulating the name of the driver image file to be loaded.
    // Note that this is used to determine whether or not the driver has
    // already been loaded by the OS loader, not necessarily in actually
    // loading the driver image, since the node can override that name.
    //

    status = NtQueryKey( KeyHandle,
                         KeyBasicInformation,
                         (PVOID) NULL,
                         0,
                         &keyBasicLength );
    if (status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL) {

        status = STATUS_ILL_FORMED_SERVICE_ENTRY;
        goto IopLoadExit;
    }

    keyBasicInformation = ExAllocatePool( NonPagedPool,
                                          keyBasicLength + (4 * 2) );
    if (!keyBasicInformation) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto IopLoadExit;
    }

    status = NtQueryKey( KeyHandle,
                         KeyBasicInformation,
                         keyBasicInformation,
                         keyBasicLength,
                         &keyBasicLength );
    if (!NT_SUCCESS( status )) {
        goto IopLoadExit;
    }

    //
    // Create a Unicode string descriptor which forms the name of the
    // driver.
    //

    baseName.Length = (USHORT) keyBasicInformation->NameLength;
    baseName.MaximumLength = (USHORT) (baseName.Length + (4 * 2));
    baseName.Buffer = &keyBasicInformation->Name[0];

    serviceName.Buffer = ExAllocatePool(PagedPool, baseName.Length + sizeof(UNICODE_NULL));
    if (serviceName.Buffer) {
        serviceName.Length = baseName.Length;
        serviceName.MaximumLength = serviceName.Length + sizeof(UNICODE_NULL);
        RtlCopyMemory(serviceName.Buffer, baseName.Buffer, baseName.Length);
        serviceName.Buffer[serviceName.Length / sizeof(WCHAR)] = UNICODE_NULL;
    } else {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto IopLoadExit;
    }

    RtlAppendUnicodeToString( &baseName, L".SYS" );

    //
    // Log the file name
    //
    HeadlessKernelAddLogEntry(HEADLESS_LOG_LOADING_FILENAME, &baseName);

    if (CheckForSafeBoot && InitSafeBootMode) {

        BOOLEAN GroupIsGood = FALSE;
        UNICODE_STRING string;
        PKEY_VALUE_PARTIAL_INFORMATION keyValue;
        UCHAR nameBuffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + 64];
        ULONG length;

        RtlInitUnicodeString( &string, L"Group" );
        keyValue = (PKEY_VALUE_PARTIAL_INFORMATION)nameBuffer;
        RtlZeroMemory(nameBuffer, sizeof(nameBuffer));

        status = NtQueryValueKey(
            KeyHandle,
            &string,
            KeyValuePartialInformation,
            keyValue,
            sizeof(nameBuffer),
            &length
            );
        if (NT_SUCCESS(status)) {

            string.Length = (USHORT)(keyValue->DataLength - sizeof(WCHAR));
            string.MaximumLength = string.Length;
            string.Buffer = (PWSTR)keyValue->Data;

            if (IopSafebootDriverLoad(&string)) {
                GroupIsGood = TRUE;
            }
        }

        if (!GroupIsGood && !IopSafebootDriverLoad(&baseName)) {
            //
            // don't load the driver
            //

            IopBootLog(&baseName, FALSE);

            DbgPrint("SAFEBOOT: skipping device = %wZ(%wZ)\n",&baseName,&string);
            HeadlessKernelAddLogEntry(HEADLESS_LOG_LOAD_SUCCESSFUL, NULL);
            return STATUS_SUCCESS;
        }

    }

    //
    // See if this driver has already been loaded by the boot loader.
    //

    //
    // No need to do KeEnterCriticalRegion as this is called
    // from system process only.
    //
    ExAcquireResourceSharedLite( &PsLoadedModuleResource, TRUE );
    nextEntry = PsLoadedModuleList.Flink;
    while (nextEntry != &PsLoadedModuleList) {

        //
        // Look at the next boot driver in the list.
        //

        driverEntry = CONTAINING_RECORD( nextEntry,
                                         KLDR_DATA_TABLE_ENTRY,
                                         InLoadOrderLinks );

        //
        // If this is not the kernel image (ntoskrnl) and not the HAL (hal),
        // then this is a driver, so initialize it.
        //

        if (RtlEqualUnicodeString(  &baseName,
                             &driverEntry->FullDllName,
                            TRUE )) {

            status = STATUS_IMAGE_ALREADY_LOADED;
            ExReleaseResourceLite( &PsLoadedModuleResource );

            IopBootLog(&baseName, TRUE);
            baseName.Buffer = NULL;
            goto IopLoadExit;
        }

        nextEntry = nextEntry->Flink;
    }
    ExReleaseResourceLite( &PsLoadedModuleResource );

    //
    // This driver has not already been loaded by the OS loader.  Form the
    // full path name for this driver.
    //

    status = IopBuildFullDriverPath(&serviceName, KeyHandle, &baseName);
    if (!NT_SUCCESS(status)) {

        baseName.Buffer = NULL;
        goto IopLoadExit;
    }

    //
    // Now get the name of the driver object.
    //

    status = IopGetDriverNameFromKeyNode( KeyHandle,
                                          &driverName );
    if (!NT_SUCCESS( status )) {
        goto IopLoadExit;
    }

    InitializeObjectAttributes( &objectAttributes,
                                &driverName,
                                OBJ_PERMANENT,
                                (HANDLE) NULL,
                                (PSECURITY_DESCRIPTOR) NULL );

    //
    // Load the driver image into memory.  If this fails partway through
    // the operation, then it will automatically be unloaded.
    //

    //
    // No need to do KeEnterCriticalRegion here as this is only
    // called from system process
    //
    ExAcquireResourceExclusiveLite( &IopDriverLoadResource, TRUE );

    status = MmLoadSystemImage( &baseName,
                                NULL,
                                NULL,
                                0,
                                &sectionPointer,
                                (PVOID *) &imageBaseAddress );

    if (!NT_SUCCESS( status )) {


        //
        // If the image was not already loaded then exit.
        //

        if (status != STATUS_IMAGE_ALREADY_LOADED) {

#if defined(_WIN64)
            //
            // If this is a driver meant for another architecture, then block this driver
            // and continue with loading the rest of the drivers stack.
            //

            if (IopCheckIfNotNativeDriver (status, &baseName) == TRUE) {
            
                if (IsFilter != FALSE) {
                    status = STATUS_DRIVER_BLOCKED;
                } else {
                    status = STATUS_DRIVER_BLOCKED_CRITICAL;
                }
#if DBG
                DbgPrint ("IopLoadDriver - Blocking driver %ws (32-bit) - Status = %lx\n", 
                          baseName.Buffer, status);
#endif

                //
                // Log an event to the eventlog
                //

                IopLogBlockedDriverEvent (&baseName, STATUS_INCOMPATIBLE_DRIVER_BLOCKED, status);
            }
#endif


            ExReleaseResourceLite( &IopDriverLoadResource );
            IopBootLog(&baseName, FALSE);

            goto IopLoadExit;
        }

        //
        // Open the driver object.
        //

        status = ObOpenObjectByName( &objectAttributes,
                                     IoDriverObjectType,
                                     KernelMode,
                                     NULL,
                                     0,
                                     NULL,
                                     &driverHandle );


        if (!NT_SUCCESS( status )) {

            ExReleaseResourceLite( &IopDriverLoadResource );
            IopBootLog(&baseName, FALSE);

            if (status == STATUS_OBJECT_NAME_NOT_FOUND) {

                //
                // Adjust the exit code so that we can distinguish drivers that
                // aren't present from drivers that are present but have had
                // their driver objects made temporary.
                //
                status = STATUS_DRIVER_FAILED_PRIOR_UNLOAD;
            }

            goto IopLoadExit;
        }

        //
        // Reference the handle and obtain a pointer to the driver object so that
        // the handle can be deleted without the object going away.
        //

        status = ObReferenceObjectByHandle( driverHandle,
                                            0,
                                            IoDriverObjectType,
                                            KeGetPreviousMode(),
                                            (PVOID *) &driverObject,
                                            (POBJECT_HANDLE_INFORMATION) NULL );
        NtClose( driverHandle );

        if (!NT_SUCCESS( status )) {
            ExReleaseResourceLite( &IopDriverLoadResource );
            IopBootLog(&baseName, FALSE);
            goto IopLoadExit;
        }


        status = IopResurrectDriver( driverObject );

        //
        // Regardless of the status the driver object should be dereferenced.
        // if the unload has already run then driver is almost gone. If
        // the driver has been resurrected then the I/O system still has its
        // original reference.
        //

        ObDereferenceObject( driverObject );
        ExReleaseResourceLite( &IopDriverLoadResource );
        IopBootLog(&baseName, FALSE);
        goto IopLoadExit;
    } else {

        ntHeaders = RtlImageNtHeader( imageBaseAddress );

        //
        // Check should this driver be loaded.  If yes, the enum subkey
        // of the service will be prepared.
        //

        status = IopPrepareDriverLoading (&serviceName, KeyHandle, imageBaseAddress, IsFilter);
        if (!NT_SUCCESS(status)) {
            MmUnloadSystemImage(sectionPointer);
            ExReleaseResourceLite( &IopDriverLoadResource );
            IopBootLog(&baseName, FALSE);
            goto IopLoadExit;
        }

    }

    //
    // The driver image has now been loaded into memory.  Create the driver
    // object that represents this image.
    //

    status = ObCreateObject( KeGetPreviousMode(),
                             IoDriverObjectType,
                             &objectAttributes,
                             KernelMode,
                             (PVOID) NULL,
                             (ULONG) (sizeof( DRIVER_OBJECT ) + sizeof ( DRIVER_EXTENSION )),
                             0,
                             0,
                             (PVOID *) &driverObject );

    if (!NT_SUCCESS( status )) {
        MmUnloadSystemImage(sectionPointer);
        ExReleaseResourceLite( &IopDriverLoadResource );
        IopBootLog(&baseName, FALSE);
        goto IopLoadExit;
    }

    //
    // Initialize this driver object and insert it into the object table.
    //

    RtlZeroMemory( driverObject, sizeof( DRIVER_OBJECT ) + sizeof ( DRIVER_EXTENSION) );
    driverObject->DriverExtension = (PDRIVER_EXTENSION) (driverObject + 1);
    driverObject->DriverExtension->DriverObject = driverObject;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        driverObject->MajorFunction[i] = IopInvalidDeviceRequest;
    }

    driverObject->Type = IO_TYPE_DRIVER;
    driverObject->Size = sizeof( DRIVER_OBJECT );
    ntHeaders = RtlImageNtHeader( imageBaseAddress );
    entryPoint = ntHeaders->OptionalHeader.AddressOfEntryPoint;
    entryPoint += (ULONG_PTR) imageBaseAddress;
    if (!(ntHeaders->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_WDM_DRIVER)) {
        driverObject->Flags |= DRVO_LEGACY_DRIVER;
    }
    driverObject->DriverInit = (PDRIVER_INITIALIZE) entryPoint;
    driverObject->DriverSection = sectionPointer;
    driverObject->DriverStart = imageBaseAddress;
    driverObject->DriverSize = ntHeaders->OptionalHeader.SizeOfImage;

    status = ObInsertObject( driverObject,
                             (PACCESS_STATE) NULL,
                             FILE_READ_DATA,
                             0,
                             (PVOID *) NULL,
                             &driverHandle );

    ExReleaseResourceLite( &IopDriverLoadResource );

    if (!NT_SUCCESS( status )) {
        IopBootLog(&baseName, FALSE);
        goto IopLoadExit;
    }

    //
    // Reference the handle and obtain a pointer to the driver object so that
    // the handle can be deleted without the object going away.
    //

    status = ObReferenceObjectByHandle( driverHandle,
                                        0,
                                        IoDriverObjectType,
                                        KeGetPreviousMode(),
                                        (PVOID *) &driverObject,
                                        (POBJECT_HANDLE_INFORMATION) NULL );

    ASSERT(status == STATUS_SUCCESS);

    NtClose( driverHandle );

    //
    // Load the Registry information in the appropriate fields of the device
    // object.
    //

    driverObject->HardwareDatabase =
        &CmRegistryMachineHardwareDescriptionSystemName;

    //
    // Store the name of the device driver in the driver object so that it
    // can be easily found by the error log thread.
    //

    driverObject->DriverName.Buffer = ExAllocatePool( PagedPool,
                                                      driverName.MaximumLength );
    if (driverObject->DriverName.Buffer) {
        driverObject->DriverName.MaximumLength = driverName.MaximumLength;
        driverObject->DriverName.Length = driverName.Length;

        RtlCopyMemory( driverObject->DriverName.Buffer,
                       driverName.Buffer,
                       driverName.MaximumLength );
    }

    //
    // Query the name of the registry path for this driver so that it can
    // be passed to the driver.
    //

    registryPath = ExAllocatePool( NonPagedPool, PAGE_SIZE );
    if (!registryPath) {
        ObMakeTemporaryObject( driverObject );
        ObDereferenceObject( driverObject );
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto IopLoadExit;
    }

    status = NtQueryObject( KeyHandle,
                            ObjectNameInformation,
                            registryPath,
                            PAGE_SIZE,
                            &i );
    if (!NT_SUCCESS( status )) {
        ObMakeTemporaryObject( driverObject );
        ObDereferenceObject( driverObject );
        ExFreePool( registryPath );
        goto IopLoadExit;
    }

#if DBG
    KeQuerySystemTime (&stime);
#endif

    //
    // Store the service key name of the device driver in the driver object
    //

    if (serviceName.Buffer) {
        driverObject->DriverExtension->ServiceKeyName.Buffer =
            ExAllocatePool( NonPagedPool, serviceName.MaximumLength );
        if (driverObject->DriverExtension->ServiceKeyName.Buffer) {
            driverObject->DriverExtension->ServiceKeyName.MaximumLength = serviceName.MaximumLength;
            driverObject->DriverExtension->ServiceKeyName.Length = serviceName.Length;

            RtlCopyMemory( driverObject->DriverExtension->ServiceKeyName.Buffer,
                           serviceName.Buffer,
                           serviceName.MaximumLength );
        }
    }

    //
    // Now invoke the driver's initialization routine to initialize itself.
    //

    status = driverObject->DriverInit( driverObject, &registryPath->Name );

    *DriverEntryStatus = status;
    if (!NT_SUCCESS(status)) {

        status = STATUS_FAILED_DRIVER_ENTRY;
    }

#if DBG

    //
    // If DriverInit took longer than 5 seconds, print a message.
    //

    KeQuerySystemTime (&etime);
    dtime  = (ULONG) ((etime.QuadPart - stime.QuadPart) / 1000000);

    if (dtime > 50) {
        DbgPrint( "IOLOAD: Driver %wZ took %d.%ds to %s\n",
            &driverName,
            dtime/10,
            dtime%10,
            NT_SUCCESS(status) ? "initialize" : "fail initialization"
            );

    }
#endif

    //
    // Workaround for broken NT 4.0 3D labs driver
    // They zero out some function table entries by mistake.

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        if (driverObject->MajorFunction[i] == NULL) {
            ASSERT(driverObject->MajorFunction[i] != NULL);
            driverObject->MajorFunction[i] = IopInvalidDeviceRequest;
        }
    }

    //
    // If DriverInit doesn't work, then simply unload the image and mark the driver
    // object as temporary.  This will cause everything to be deleted.
    //

    ExFreePool( registryPath );

    //
    // If we load the driver because we think it is a legacy driver and
    // it does not create any device object in its DriverEntry.  We will
    // unload this driver.
    //

    if (NT_SUCCESS(status) && !IopIsLegacyDriver(driverObject)) {

        status = IopPnpDriverStarted(driverObject, KeyHandle, &serviceName);

        if (!NT_SUCCESS(status)) {
            if (driverObject->DriverUnload) {
                driverObject->Flags |= DRVO_UNLOAD_INVOKED;
                driverObject->DriverUnload(driverObject);
                IopBootLog(&baseName, FALSE);
            } else {
#if DBG
                DbgPrint("IopLoadDriver: A PnP driver %wZ does not support DriverUnload routine.\n", &driverName);
#endif
            }
        }
    }

    if (!NT_SUCCESS( status )) {
        ObMakeTemporaryObject( driverObject );
        ObDereferenceObject( driverObject );
    } else {

        //
        // Free the memory occupied by the driver's initialization routines.
        //

        IopBootLog(&baseName, TRUE);
        MmFreeDriverInitialization( driverObject->DriverSection );
        IopReadyDeviceObjects( driverObject );
    }

IopLoadExit:

    if (NT_SUCCESS(status) || (status == STATUS_IMAGE_ALREADY_LOADED)) {
        HeadlessKernelAddLogEntry(HEADLESS_LOG_LOAD_SUCCESSFUL, NULL);
    } else {
        HeadlessKernelAddLogEntry(HEADLESS_LOG_LOAD_FAILED, NULL);
    }

    //
    // Free any pool that was allocated by this routine that has not yet
    // been freed.
    //

    if (driverName.Buffer != NULL) {
        ExFreePool( driverName.Buffer );
    }

    if (keyBasicInformation != NULL) {
        ExFreePool( keyBasicInformation );
    }

    if (serviceName.Buffer != NULL) {
        ExFreePool(serviceName.Buffer);
    }

    if (baseName.Buffer != NULL) {
        ExFreePool(baseName.Buffer);
    }

    //
    // If this routine is about to return a failure, then let the Configuration
    // Manager know about it.  But, if STATUS_PLUGPLAY_NO_DEVICE, the device was
    // disabled by hardware profile.  In this case we don't need to report it.
    //

    if (!NT_SUCCESS( status ) && (status != STATUS_PLUGPLAY_NO_DEVICE)) {

        NTSTATUS lStatus;
        PULONG errorControl;
        PKEY_VALUE_FULL_INFORMATION keyValueInformation;

        if (status != STATUS_IMAGE_ALREADY_LOADED) {

            //
            // If driver was loaded, do not call IopDriverLoadingFailed to change
            // the driver loading status.  Because, obviously, the driver is
            // running.
            //

            IopDriverLoadingFailed(KeyHandle, NULL);
            lStatus = IopGetRegistryValue( KeyHandle,
                                           L"ErrorControl",
                                           &keyValueInformation );
            if (!NT_SUCCESS( lStatus ) || !keyValueInformation->DataLength) {
                if (NT_SUCCESS( lStatus )) {
                    ExFreePool( keyValueInformation );
                }
            } else {
                errorControl = (PULONG) ((PUCHAR) keyValueInformation + keyValueInformation->DataOffset);
                CmBootLastKnownGood( *errorControl );
                ExFreePool( keyValueInformation );
            }
        }
    }

    //
    // Close the caller's handle and return the final status from the load
    // operation.
    //

    ObCloseHandle( KeyHandle , KernelMode);
    return status;
}


PDEVICE_OBJECT
IopGetDeviceAttachmentBase(
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    This routine returns the lowest level device object associated with
    the specified device.

Arguments:

    DeviceObject - Supplies a pointer to the device for which the bottom of
        attachment chain is to be found.

Return Value:

    The function value is a reference to the lowest level device attached
    to the specified device.  If the supplied device object is that device
    object, then a pointer to it is returned.

    N.B. Caller must own the IopDatabaseLock.

--*/

{
    PDEVICE_OBJECT baseDeviceObject;
    PDEVOBJ_EXTENSION deviceExtension;

    //
    // Descend down the attachment chain until we find a device object
    // that isn't attached to anything else.
    //

    baseDeviceObject = DeviceObject;
    deviceExtension = baseDeviceObject->DeviceObjectExtension;
    while (deviceExtension->AttachedTo != NULL) {

        baseDeviceObject = deviceExtension->AttachedTo;
        deviceExtension = baseDeviceObject->DeviceObjectExtension;
    }

    return baseDeviceObject;
}



VOID
IopDecrementDeviceObjectRef(
    IN PDEVICE_OBJECT DeviceObject,
    IN BOOLEAN AlwaysUnload,
    IN BOOLEAN OnCleanStack
    )

/*++

Routine Description:

    The routine decrements the reference count on a device object.  If the
    reference count goes to zero and the device object is a candidate for deletion
    then IopCompleteUnloadOrDelete is called.  A device object is subject for
    deletion if the AlwaysUnload flag is true, or the device object is pending
    deletion or the driver is pending unload.

Arguments:

    DeviceObject - Supplies the device object whose reference count is to be
                   decremented.

    AlwaysUnload - Indicates if the driver should be unloaded regardless of the
                   state of the unload flag.

    OnCleanStack - Indicates whether the current thread is in the middle a
                   driver operation.

Return Value:

    None.

--*/
{
    KIRQL irql;

    //
    // Decrement the reference count on the device object.  If this is the last
    // last reason that this mini-file system recognizer needs to stay around,
    // then unload it.
    //

    irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );

    ASSERT( DeviceObject->ReferenceCount > 0 );

    DeviceObject->ReferenceCount--;

    if (!DeviceObject->ReferenceCount && (AlwaysUnload ||
         DeviceObject->DeviceObjectExtension->ExtensionFlags &
         (DOE_DELETE_PENDING | DOE_UNLOAD_PENDING | DOE_REMOVE_PENDING))) {

        IopCompleteUnloadOrDelete( DeviceObject, OnCleanStack, irql );
    } else {
        KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
    }

}

VOID
IopLoadFileSystemDriver(
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    This routine is invoked when a mini-file system recognizer driver recognizes
    a volume as being a particular file system, but the driver for that file
    system has not yet been loaded.  This function allows the mini-driver to
    load the real file system, and remove itself from the system, so that the
    real file system can mount the device in question.

Arguments:

    DeviceObject - Registered file system device object for the mini-driver.

Return Value:

    None.

--*/

{
    KEVENT event;
    NTSTATUS status;
    IO_STATUS_BLOCK ioStatus;
    PIRP irp;
    PIO_STACK_LOCATION irpSp;
    PDEVICE_OBJECT attachedDevice;

    PAGED_CODE();

    attachedDevice = DeviceObject;
    while (attachedDevice->AttachedDevice) {
        attachedDevice = attachedDevice->AttachedDevice;
    }

    //
    // Begin by building an I/O Request Packet to have the mini-file system
    // driver load the real file system.
    //

    KeInitializeEvent( &event, NotificationEvent, FALSE );

    irp = IoBuildDeviceIoControlRequest( IRP_MJ_DEVICE_CONTROL,
                                         attachedDevice,
                                         (PVOID) NULL,
                                         0,
                                         (PVOID) NULL,
                                         0,
                                         FALSE,
                                         &event,
                                         &ioStatus );
    if (irp) {

        //
        // Change the actual major and minor function codes to be a file system
        // control with a minor function code of load FS driver.
        //

        irpSp = IoGetNextIrpStackLocation( irp );
        irpSp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
        irpSp->MinorFunction = IRP_MN_LOAD_FILE_SYSTEM;

        //
        // Now issue the request.
        //

        status = IoCallDriver( attachedDevice, irp );
        if (status == STATUS_PENDING) {
            (VOID) KeWaitForSingleObject( &event,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          (PLARGE_INTEGER) NULL );
        }
    }

    //
    // Decrement the reference count on the device object.  If this is the last
    // last reason that this mini-file system recognizer needs to stay around,
    // then unload it.
    //

    IopDecrementDeviceObjectRef(DeviceObject, TRUE, TRUE);

    return;
}

VOID
IopLoadUnloadDriver(
    IN PVOID Parameter
    )

/*++

Routine Description:

    This routine is executed as an EX worker thread routine when a driver is
    to be loaded or unloaded dynamically.  It is used because some drivers
    need to create system threads in the context of the system process, which
    cannot be done in the context of the caller of the system service that
    was invoked to load or unload the specified driver.

Arguments:

    Parameter - Pointer to the load packet describing what work is to be
        done.

Return Value:

    None.

--*/

{
    PLOAD_PACKET loadPacket;
    NTSTATUS status, driverEntryStatus;
    HANDLE keyHandle;

    PAGED_CODE();

    //
    // Begin by getting a pointer to the load packet.
    //

    loadPacket = (PLOAD_PACKET) Parameter;

    //
    // If the driver object field of the packet is non-NULL, then this is
    // a request to complete the unload of a driver.  Simply invoke the
    // driver's unload routine.  Note that the final status of the unload
    // is ignored, so it is not set here.
    //

    if (loadPacket->DriverObject) {

        loadPacket->DriverObject->DriverUnload( loadPacket->DriverObject );
        status = STATUS_SUCCESS;

    } else {

        //
        // The driver specified by the DriverServiceName is to be loaded.
        // Begin by opening the registry node for this driver.  Note
        // that if this is successful, then the load driver routine is
        // responsible for closing the handle.
        //

        status = IopOpenRegistryKey( &keyHandle,
                                     (HANDLE) NULL,
                                     loadPacket->DriverServiceName,
                                     KEY_READ,
                                     FALSE );
        if (NT_SUCCESS( status )) {

            //
            // Invoke the internal common routine to perform the work.
            // This is the same routine that is used by the I/O system
            // initialization code to load drivers.
            //

            status = IopLoadDriver( keyHandle, TRUE, FALSE, &driverEntryStatus );

            if (status == STATUS_FAILED_DRIVER_ENTRY) {

                status = driverEntryStatus;

            } else if (status == STATUS_DRIVER_FAILED_PRIOR_UNLOAD) {

                //
                // Keep legacy behavior (don't change status code)
                //
                status = STATUS_OBJECT_NAME_NOT_FOUND;
            }

            IopCallDriverReinitializationRoutines();
        }
    }

    //
    // Set the final status of the load or unload operation, and indicate to
    // the caller that the operation is now complete.
    //

    loadPacket->FinalStatus = status;
    (VOID) KeSetEvent( &loadPacket->Event, 0, FALSE );
}

NTSTATUS
IopMountVolume(
    IN PDEVICE_OBJECT DeviceObject,
    IN BOOLEAN AllowRawMount,
    IN BOOLEAN DeviceLockAlreadyHeld,
    IN BOOLEAN Alertable,
    OUT PVPB    *Vpb
    )

/*++

Routine Description:

    This routine is used to mount a volume on the specified device.  The Volume
    Parameter Block (VPB) for the specified device is a "clean" VPB.  That is,
    it indicates that the volume has never been mounted.  It is up to the file
    system that eventually mounts the volume to determine whether the volume is,
    or has been, mounted elsewhere.

Arguments:

    DeviceObject - Pointer to device object on which the volume is to be
        mounted.

    AllowRawMount - This parameter tells us if we should continue our
        filesystem search to include the Raw file system.  This flag will
        only be passed in as TRUE as a result of a DASD open.

    DeviceLockAlreadyHeld - If TRUE, then the caller has already acquired
        the device lock and we should not attempt to acquire it.  This is
        currently passed in as TRUE when called from IoVerifyVolume.

Return Value:

    The function value is a successful status code if a volume was successfully
    mounted on the device.  Otherwise, an error code is returned.


--*/

{
    NTSTATUS status;
    KEVENT event;
    PIRP irp;
    PDEVICE_OBJECT fsDeviceObject;
    PDEVICE_OBJECT attachedDevice;
    PLIST_ENTRY entry;
    PLIST_ENTRY queueHeader;
    IO_STATUS_BLOCK ioStatus;
    PIO_STACK_LOCATION irpSp;
    ULONG extraStack;
    LIST_ENTRY dummy;
    ULONG rawMountOnly;
    ULONG numRegOps;
    PETHREAD CurrentThread;

    PAGED_CODE();

    CurrentThread = PsGetCurrentThread ();

    //
    // Obtain the lock for the device to be mounted.  This guarantees that
    // only one thread is attempting to mount (or verify) this particular
    // device at a time.
    //

    if (!DeviceLockAlreadyHeld) {

        status = KeWaitForSingleObject( &DeviceObject->DeviceLock,
                                        Executive,
                                        KeGetPreviousModeByThread(&CurrentThread->Tcb),
                                        Alertable,
                                        (PLARGE_INTEGER) NULL );

        //
        // If the wait ended because of an alert or an APC, return now
        // without mounting the device.  Note that as the wait for the
        // event was unsuccessful, we do not set it on exit.
        //

        if (status == STATUS_ALERTED || status == STATUS_USER_APC) {

            return status;
        }
    }

    //
    // Now acquire the resource database lock for the I/O system to perform this
    // operation.  This resource protects access to the file system queue.
    //

    KeEnterCriticalRegionThread(&CurrentThread->Tcb);
    (VOID) ExAcquireResourceSharedLite( &IopDatabaseResource, TRUE );

    //
    // Check the 'mounted' flag of the VPB to ensure that it is still clear.
    // If it is, then no one has gotten in before this to mount the volume.
    // Attempt to mount the volume in this case.
    //

    if ((DeviceObject->Vpb->Flags & (VPB_MOUNTED | VPB_REMOVE_PENDING)) == 0) {

        //
        // This volume has never been mounted.  Initialize the event and set the
        // status to unsuccessful to set up for the loop.  Also if the device
        // has the verify bit set, clear it.
        //

        KeInitializeEvent( &event, NotificationEvent, FALSE );
        status = STATUS_UNSUCCESSFUL;
        DeviceObject->Flags &= ~DO_VERIFY_VOLUME;

        //
        // Get the actual device that this volume is to be mounted on.  This
        // device is the final device in the list of devices which are attached
        // to the specified real device.
        //

        attachedDevice = DeviceObject;
        while (attachedDevice->AttachedDevice) {
            attachedDevice = attachedDevice->AttachedDevice;
        }

        //
        // Reference the device object so it cannot go away.
        //

        ObReferenceObject( attachedDevice );

        //
        // Determine which type of file system should be invoked based on
        // the device type of the device being mounted.
        //

        if (DeviceObject->DeviceType == FILE_DEVICE_DISK ||
            DeviceObject->DeviceType == FILE_DEVICE_VIRTUAL_DISK) {
            queueHeader = &IopDiskFileSystemQueueHead;
        } else if (DeviceObject->DeviceType == FILE_DEVICE_CD_ROM) {
            queueHeader = &IopCdRomFileSystemQueueHead;
        } else {
            queueHeader = &IopTapeFileSystemQueueHead;
        }

        rawMountOnly = (DeviceObject->Vpb->Flags & VPB_RAW_MOUNT);

        //
        // Now loop through each of the file systems which have been loaded in
        // the system to see whether anyone understands the media in the device.
        //

        for (entry = queueHeader->Flink;
             entry != queueHeader && !NT_SUCCESS( status );
             entry = entry->Flink) {

            PDEVICE_OBJECT savedFsDeviceObject;

            //
            // If this is the final entry (Raw file system), and it is also
            // not the first entry, and a raw mount is not permitted, then
            // break out of the loop at this point, as this volume cannot
            // be mounted for the caller's purposes.
            //

            if (!AllowRawMount && entry->Flink == queueHeader && entry != queueHeader->Flink) {
                break;
            }

            //
            // If raw mount is the only one requested and this is not the last entry on the list
            // then skip.
            //
            if (rawMountOnly && (entry->Flink != queueHeader)) {
                continue;
            }

            fsDeviceObject = CONTAINING_RECORD( entry, DEVICE_OBJECT, Queue.ListEntry );
            savedFsDeviceObject = fsDeviceObject;

            //
            // It is possible that the file system has been attached to, so
            // walk the attached list for the file system.  The number of stack
            // locations that must be allocated in the IRP must include one for
            // the file system itself, and then one for each driver that is
            // attached to it.  Account for all of the stack locations required
            // to get through the mount process.
            //

            extraStack = 1;

            while (fsDeviceObject->AttachedDevice) {
                fsDeviceObject = fsDeviceObject->AttachedDevice;
                extraStack++;
            }

            //
            // Another file system has been found and the volume has still not
            // been mounted.  Attempt to mount the volume using this file
            // system.
            //
            // Begin by resetting the event being used for synchronization with
            // the I/O operation.
            //

            KeClearEvent( &event );

            //
            // Allocate and initialize an IRP for this mount operation.  Notice
            // that the flags for this operation appear the same as a page read
            // operation.  This is because the completion code for both of the
            // operations is exactly the same logic.
            //

            irp = IoAllocateIrp ((CCHAR) (attachedDevice->StackSize + extraStack), FALSE);

            if ( !irp ) {

                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }

            irp->Flags = IRP_MOUNT_COMPLETION | IRP_SYNCHRONOUS_PAGING_IO;
            irp->RequestorMode = KernelMode;
            irp->UserEvent = &event;
            irp->UserIosb = &ioStatus;
            irp->Tail.Overlay.Thread = CurrentThread;
            irpSp = IoGetNextIrpStackLocation( irp );
            irpSp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
            irpSp->MinorFunction = IRP_MN_MOUNT_VOLUME;
            irpSp->Flags = AllowRawMount;
            irpSp->Parameters.MountVolume.Vpb = DeviceObject->Vpb;
            irpSp->Parameters.MountVolume.DeviceObject = attachedDevice;

            numRegOps = IopFsRegistrationOps;

            //
            // Increment the number of reasons that this driver cannot
            // be unloaded.  Note that this must be done while still
            // holding the database resource.
            //

            IopInterlockedIncrementUlong( LockQueueIoDatabaseLock,
                                          &savedFsDeviceObject->ReferenceCount );

            ExReleaseResourceLite( &IopDatabaseResource );

            status = IoCallDriver( fsDeviceObject, irp );

            //
            // Wait for the I/O operation to complete.
            //

            if (status == STATUS_PENDING) {
                (VOID) KeWaitForSingleObject( &event,
                                              Executive,
                                              KernelMode,
                                              FALSE,
                                              (PLARGE_INTEGER) NULL );
            } else {

                //
                // Ensure that the proper status value gets picked up.
                //

                ioStatus.Status = status;
                ioStatus.Information = 0;
            }

            (VOID) ExAcquireResourceSharedLite( &IopDatabaseResource, TRUE );


            //
            // Decrement the number of reasons that this driver cannot be unloaded.
            // If the device object is for FSREC it could not have gotten de-registered
            // here. It should get de-registered only at the time of loading the driver
            // which should happen later.
            //

            IopInterlockedDecrementUlong( LockQueueIoDatabaseLock,
                                          &savedFsDeviceObject->ReferenceCount );

            //
            // If the operation was successful then set the VPB as mounted.
            //

            if (NT_SUCCESS( ioStatus.Status )) {

                status = ioStatus.Status;

                *Vpb = IopMountInitializeVpb(DeviceObject, attachedDevice, rawMountOnly);

            } else {

                //
                // The mount operation failed.  Make a special check here to
                // determine whether or not a popup was enabled, and if so,
                // check to see whether or not the operation was to be aborted.
                // If so, bail out now and return the error to the caller.
                //

                status = ioStatus.Status;
                if (IoIsErrorUserInduced(status) &&
                    ioStatus.Information == IOP_ABORT) {
                    break;
                }

                //
                // If there were any registrations or unregistrations during the period
                // we unlocked the database resource bail out and start all over again.
                //

                if (numRegOps != IopFsRegistrationOps) {

                    //
                    // Reset the list back to the beginning and start over
                    // again.
                    //

                    dummy.Flink = queueHeader->Flink;
                    entry = &dummy;
                    status = STATUS_UNRECOGNIZED_VOLUME;
                }

                //
                // Also check to see whether or not this is a volume that has
                // been recognized, but the file system for it needs to be
                // loaded.  If so, drop the locks held at this point, tell the
                // mini-file system recognizer to load the driver, and then
                // reacquire the locks.
                //

                if (status == STATUS_FS_DRIVER_REQUIRED) {

                    //
                    // Increment the number of reasons that this driver cannot
                    // be unloaded.  Note that this must be done while still
                    // holding the database resource.
                    //

                    IopInterlockedIncrementUlong( LockQueueIoDatabaseLock,
                                                  &savedFsDeviceObject->ReferenceCount );

                    ExReleaseResourceLite( &IopDatabaseResource );

                    if (!DeviceLockAlreadyHeld) {
                        KeSetEvent( &DeviceObject->DeviceLock, 0, FALSE );
                    }

                    KeLeaveCriticalRegionThread(&CurrentThread->Tcb);
                    IopLoadFileSystemDriver( savedFsDeviceObject );

                    //
                    // Now reacquire the locks, in the correct order, and check
                    // to see if the volume has been mounted before we could
                    // get back.  If so, exit; otherwise, restart the file
                    // file system queue scan from the beginning.
                    //

                    if (!DeviceLockAlreadyHeld) {
                        status = KeWaitForSingleObject( &DeviceObject->DeviceLock,
                                                        Executive,
                                                        KeGetPreviousModeByThread(&CurrentThread->Tcb),
                                                        Alertable,
                                                        (PLARGE_INTEGER) NULL );
                        if (status == STATUS_ALERTED || status == STATUS_USER_APC) {

                            //
                            // The device was not mounted by us so
                            // drop the reference before returning.
                            //

                            ObDereferenceObject( attachedDevice );

                            return status;
                        }
                    }

                    KeEnterCriticalRegionThread(&CurrentThread->Tcb);
                    (VOID) ExAcquireResourceSharedLite( &IopDatabaseResource, TRUE );

                    if (DeviceObject->Vpb->Flags & VPB_MOUNTED) {

                        //
                        //  This volume was mounted before we got back.
                        //  Hence deref the attachedDevice as the other thread
                        //  that got the reference is the one that's going to be
                        //  used by the filesystem.
                        //

                        ObDereferenceObject( attachedDevice );
                        status = STATUS_SUCCESS;
                        break;
                    }

                    //
                    // Reset the list back to the beginning and start over
                    // again.
                    //

                    dummy.Flink = queueHeader->Flink;
                    entry = &dummy;
                    status = STATUS_UNRECOGNIZED_VOLUME;
                }

                //
                // If the error wasn't STATUS_UNRECOGNIZED_VOLUME, and this
                // request is not going to the Raw file system, then there
                // is no reason to continue looping.
                //

                if (!AllowRawMount && (status != STATUS_UNRECOGNIZED_VOLUME) &&
                    FsRtlIsTotalDeviceFailure(status)) {
                    break;
                }

            }
        }

        if (!NT_SUCCESS(status)) {

            //
            // The device was not mounted by us so
            // drop the reference.
            // On success this reference is used by the filesystem
            // Its usually Vcb->TargetDeviceObject.
            // On a dismount the filesystem deref (Vcb->TargetDeviceObject)
            //

            ObDereferenceObject( attachedDevice );

        }

    } else if((DeviceObject->Vpb->Flags & VPB_REMOVE_PENDING) != 0) {

        //
        // Pnp is attempting to remove this volume.  Don't allow the mount.
        //

        status = STATUS_DEVICE_DOES_NOT_EXIST;

    } else {

        //
        // The volume for this device has already been mounted.  Return a
        // success code.
        //

        status = STATUS_SUCCESS;
    }

    ExReleaseResourceLite( &IopDatabaseResource );

    //
    // Release the I/O database resource lock and the synchronization event for
    // the device.
    //

    if (!DeviceLockAlreadyHeld) {
        KeSetEvent( &DeviceObject->DeviceLock, 0, FALSE );
    }

    KeLeaveCriticalRegionThread(&CurrentThread->Tcb);

    //
    // Finally, if the mount operation failed, and the target device is the
    // boot partition, then bugcheck the system.  It is not possible for the
    // system to run properly if the system's boot partition cannot be mounted.
    //
    // Note: Don't bugcheck if the system is already booted.
    //

    if (!NT_SUCCESS( status ) &&
        DeviceObject->Flags & DO_SYSTEM_BOOT_PARTITION &&
        InitializationPhase < 2) {
        KeBugCheckEx( UNMOUNTABLE_BOOT_VOLUME, (ULONG_PTR) DeviceObject, status, 0, 0 );
    }

    return status;
}


NTSTATUS
IopInvalidateVolumesForDevice(
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    This routine is used to force filesystems to, as completely as possible, throw
    out volumes which remain referenced for a given device.

Arguments:

    DeviceObject - Pointer to device object for which volumes are to be
        invalidated.

Return Value:

    The function value is a successful status code if all filesystems accepted the
    operation.  Otherwise, an error code is returned.

--*/

{
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS finalStatus;
    KEVENT event;
    PIRP irp;
    PDEVICE_OBJECT fsDeviceObject;
    PDEVICE_OBJECT attachedDevice;
    PFILE_OBJECT storageFileObject;
    HANDLE storageHandle;
    PLIST_ENTRY entry;
    PLIST_ENTRY queueHeader;
    IO_STATUS_BLOCK ioStatus;
    PIO_STACK_LOCATION irpSp;
    PKTHREAD CurrentThread;

    PAGED_CODE();


    //
    // Get the actual device that could be mounted on.
    // Note that there could be multiple device objects on the stack that has a
    // VPB and could be potentially mounted on by FS. So we call the FS with every
    // device object that has a VPB. This is really a simple brute force approach but
    // this is not a high performance path and is backwards compatible.
    //

    for (attachedDevice = DeviceObject ;attachedDevice; attachedDevice = attachedDevice->AttachedDevice) {

        //
        // If the device object has no VPB skip.
        //

        if (!attachedDevice->Vpb) {
            continue;
        }

        //
        // Synchronize against mounts.
        //

        KeWaitForSingleObject(&(attachedDevice->DeviceLock),
                      Executive,
                      KernelMode,
                      FALSE,
                      NULL);
        //
        // Get a handle to this device for use in the fsctl.  The way we have to do
        // this is kind of loopy: note we wind up with two references to clean up.
        //
        // The only use of this fileobject/handle is to communicate the device to
        // invalidate volumes on.  It isn't used for anything else, and must not be.
        //

        storageHandle = NULL;
        storageFileObject = NULL;

        try {

            storageFileObject = IoCreateStreamFileObjectLite( NULL, attachedDevice );
            storageFileObject->Vpb = attachedDevice->Vpb;

            status = ObOpenObjectByPointer( storageFileObject,
                                            OBJ_KERNEL_HANDLE,
                                            NULL,
                                            0,
                                            IoFileObjectType,
                                            KernelMode,
                                            &storageHandle );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            status = GetExceptionCode();
        }

        if (NT_SUCCESS( status )) {

            //
            // Now acquire the resource database lock for the I/O system to perform this
            // operation.  This resource protects access to the file system queue.
            //

            CurrentThread = KeGetCurrentThread ();
            KeEnterCriticalRegionThread(CurrentThread);
            (VOID) ExAcquireResourceSharedLite( &IopDatabaseResource, TRUE );

            //
            // Determine which type of file system should be invoked based on
            // the device type of the device being invalidated.
            //

            if (DeviceObject->DeviceType == FILE_DEVICE_DISK ||
                DeviceObject->DeviceType == FILE_DEVICE_VIRTUAL_DISK) {
                queueHeader = &IopDiskFileSystemQueueHead;
            } else if (DeviceObject->DeviceType == FILE_DEVICE_CD_ROM) {
                queueHeader = &IopCdRomFileSystemQueueHead;
            } else {
                queueHeader = &IopTapeFileSystemQueueHead;
            }

            //
            // Initialize the event and set the status to set up
            // for the loop.
            //

            KeInitializeEvent( &event, NotificationEvent, FALSE );
            finalStatus = STATUS_SUCCESS;

            //
            // Now loop through each of the file systems which have been loaded in
            // the system and ask them to invalidate volumes they have had mounted
            // on it.
            //

            for (entry = queueHeader->Flink;
                 entry != queueHeader;
                 entry = entry->Flink) {

                //
                // If this is the final entry (Raw file system), then break out of the
                // loop at this point, as volumes cannot be invalidated for the caller's
                // purposes in Raw.
                //

                if (entry->Flink == queueHeader) {
                    break;
                }

                fsDeviceObject = CONTAINING_RECORD( entry, DEVICE_OBJECT, Queue.ListEntry );

                //
                // It is possible that the file system has been attached to, so
                // walk the attached list for the file system.
                //

                while (fsDeviceObject->AttachedDevice) {
                    fsDeviceObject = fsDeviceObject->AttachedDevice;
                }

                //
                // Another file system has been found.  Attempt to invalidate volumes
                // using this file system.
                //
                // Begin by resetting the event being used for synchronization with
                // the I/O operation.
                //

                KeClearEvent( &event );

                //
                // Build an IRP for this operation.
                //

                irp = IoBuildDeviceIoControlRequest( FSCTL_INVALIDATE_VOLUMES,
                                                     fsDeviceObject,
                                                     &storageHandle,
                                                     sizeof(HANDLE),
                                                     NULL,
                                                     0,
                                                     FALSE,
                                                     &event,
                                                     &ioStatus );

                if (irp == NULL) {

                    finalStatus = STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }

                irpSp = IoGetNextIrpStackLocation( irp );
                irpSp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;

                status = IoCallDriver( fsDeviceObject, irp );

                //
                // Wait for the I/O operation to complete.
                //

                if (status == STATUS_PENDING) {
                    (VOID) KeWaitForSingleObject( &event,
                                                  Executive,
                                                  KernelMode,
                                                  FALSE,
                                                  (PLARGE_INTEGER) NULL );

                    status = ioStatus.Status;

                } else {

                    //
                    // Ensure that the proper status value gets picked up.
                    //

                    ioStatus.Status = status;
                    ioStatus.Information = 0;
                }

                //
                // Commute status' indicating the operation is not implemented
                // to success.  If a filesystem does not implement, it must not
                // hold volumes that are not mounted.
                //

                if (status == STATUS_INVALID_DEVICE_REQUEST ||
                    status == STATUS_NOT_IMPLEMENTED) {

                    status = STATUS_SUCCESS;
                }

                //
                //  Hand back the first failure we get, but plow on anyway.
                //

                if (NT_SUCCESS( finalStatus ) && !NT_SUCCESS( status )) {
                    finalStatus = status;
                }
            }

            ExReleaseResourceLite( &IopDatabaseResource );
            KeLeaveCriticalRegionThread(CurrentThread);

            if (storageFileObject) {
                ObDereferenceObject( storageFileObject );
                if (storageHandle) {
                    ZwClose( storageHandle ); // Note that this is a close for which the FS has not
                                              // gotten the corresponding open.
                }
            }

            status = finalStatus;
        }

        //
        // Unlock the device lock to let mounts go
        //

        KeSetEvent(&(attachedDevice->DeviceLock), IO_NO_INCREMENT, FALSE);
    }


    return status;
}

NTSTATUS
IopOpenLinkOrRenameTarget(
    OUT PHANDLE TargetHandle,
    IN PIRP Irp,
    IN PVOID RenameBuffer,
    IN PFILE_OBJECT FileObject
    )

/*++

Routine Description:

    This routine is invoked by the rename, set link and set copy-on-write code
    in the I/O system's NtSetInformationFile system service when the caller has
    specified a fully qualified file name as the target of a rename, set link,
    or set copy-on-write operation.  This routine attempts to open the parent
    of the specified file and checks the following:

        o   If the file itself exists, then the caller must have specified that
            the target is to be replaced, otherwise an error is returned.

        o   Ensures that the target file specification refers to the same volume
            upon which the source file exists.

Arguments:

    TargetHandle - Supplies the address of a variable to return the handle to
        the opened target file if no errors have occurred.

    Irp - Supplies a pointer to the IRP that represents the current rename
        request.

    RenameBuffer - Supplies a pointer to the system intermediate buffer that
        contains the caller's rename parameters.

    FileObject - Supplies a pointer to the file object representing the file
        being renamed.

Return Value:

    The function value is the final status of the operation.

Note:

    This function assumes that the layout of a rename, set link and set
    copy-on-write information structure are exactly the same.

--*/

{
    NTSTATUS status;
    IO_STATUS_BLOCK ioStatus;
    HANDLE handle;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING newFileName;
    PIO_STACK_LOCATION irpSp;
    PFILE_OBJECT targetFileObject;
    OBJECT_HANDLE_INFORMATION handleInformation;
    PFILE_RENAME_INFORMATION renameBuffer = RenameBuffer;
    FILE_BASIC_INFORMATION  basicInformation;
    ACCESS_MASK accessMask;

    PAGED_CODE();

    ASSERT( sizeof( FILE_RENAME_INFORMATION ) ==
            sizeof( FILE_LINK_INFORMATION ) );
    ASSERT( FIELD_OFFSET( FILE_RENAME_INFORMATION, ReplaceIfExists ) ==
            FIELD_OFFSET( FILE_LINK_INFORMATION, ReplaceIfExists ) );
    ASSERT( FIELD_OFFSET( FILE_RENAME_INFORMATION, RootDirectory ) ==
            FIELD_OFFSET( FILE_LINK_INFORMATION, RootDirectory ) );
    ASSERT( FIELD_OFFSET( FILE_RENAME_INFORMATION, FileNameLength ) ==
            FIELD_OFFSET( FILE_LINK_INFORMATION, FileNameLength ) );
    ASSERT( FIELD_OFFSET( FILE_RENAME_INFORMATION, FileName ) ==
            FIELD_OFFSET( FILE_LINK_INFORMATION, FileName ) );

    ASSERT( sizeof( FILE_RENAME_INFORMATION ) ==
            sizeof( FILE_MOVE_CLUSTER_INFORMATION ) );
    ASSERT( FIELD_OFFSET( FILE_RENAME_INFORMATION, ReplaceIfExists ) ==
            FIELD_OFFSET( FILE_MOVE_CLUSTER_INFORMATION, ClusterCount ) );
    ASSERT( FIELD_OFFSET( FILE_RENAME_INFORMATION, RootDirectory ) ==
            FIELD_OFFSET( FILE_MOVE_CLUSTER_INFORMATION, RootDirectory ) );
    ASSERT( FIELD_OFFSET( FILE_RENAME_INFORMATION, FileNameLength ) ==
            FIELD_OFFSET( FILE_MOVE_CLUSTER_INFORMATION, FileNameLength ) );
    ASSERT( FIELD_OFFSET( FILE_RENAME_INFORMATION, FileName ) ==
            FIELD_OFFSET( FILE_MOVE_CLUSTER_INFORMATION, FileName ) );

    //
    // Check if the fileobject is a directory or a regular file.
    // The access mask is different based on that behavior.
    //

    accessMask = FILE_WRITE_DATA;

    if (!(FileObject->Flags & FO_DIRECT_DEVICE_OPEN)) {
        status = IopGetBasicInformationFile(FileObject, &basicInformation);

        if (!NT_SUCCESS(status)) {
            return status;
        }

        if (basicInformation.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            accessMask = FILE_ADD_SUBDIRECTORY;
        }
    }

    //
    // A fully qualified file name was specified.  Begin by attempting to open
    // the parent directory of the specified target file.
    //

    newFileName.Length = (USHORT) renameBuffer->FileNameLength;
    newFileName.MaximumLength = (USHORT) renameBuffer->FileNameLength;
    newFileName.Buffer = renameBuffer->FileName;

    InitializeObjectAttributes( &objectAttributes,
                                &newFileName,
                                (FileObject->Flags & FO_OPENED_CASE_SENSITIVE ? 0 : OBJ_CASE_INSENSITIVE)|OBJ_KERNEL_HANDLE,
                                renameBuffer->RootDirectory,
                                (PSECURITY_DESCRIPTOR) NULL );

    //
    // Check if the fileobject is not to the top of the stack.
    //

    if (FileObject->Flags & FO_FILE_OBJECT_HAS_EXTENSION) {

        PIOP_FILE_OBJECT_EXTENSION  fileObjectExtension =
            (PIOP_FILE_OBJECT_EXTENSION)(FileObject + 1);

        ASSERT(!(FileObject->Flags & FO_DIRECT_DEVICE_OPEN));

        status = IoCreateFileSpecifyDeviceObjectHint( &handle,
                                                      accessMask | SYNCHRONIZE,
                                                      &objectAttributes,
                                                      &ioStatus,
                                                      (PLARGE_INTEGER) NULL,
                                                      0,
                                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                      FILE_OPEN,
                                                      FILE_OPEN_FOR_BACKUP_INTENT,
                                                      (PVOID) NULL,
                                                      0L,
                                                      CreateFileTypeNone,
                                                      (PVOID) NULL,
                                                      IO_NO_PARAMETER_CHECKING |
                                                      IO_OPEN_TARGET_DIRECTORY |
                                                      IO_FORCE_ACCESS_CHECK |
                                                      IOP_CREATE_USE_TOP_DEVICE_OBJECT_HINT,
                                                      fileObjectExtension->TopDeviceObjectHint );

    } else {

        status = IoCreateFile( &handle,
                               accessMask | SYNCHRONIZE,
                               &objectAttributes,
                               &ioStatus,
                               (PLARGE_INTEGER) NULL,
                               0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               FILE_OPEN,
                               FILE_OPEN_FOR_BACKUP_INTENT,
                               (PVOID) NULL,
                               0L,
                               CreateFileTypeNone,
                               (PVOID) NULL,
                               IO_NO_PARAMETER_CHECKING |
                               IO_OPEN_TARGET_DIRECTORY |
                               IO_FORCE_ACCESS_CHECK );
    }

    if (NT_SUCCESS( status )) {
        //
        // The open operation for the target file's parent directory was
        // successful.  Check to see whether or not the file exists.
        //

        irpSp = IoGetNextIrpStackLocation( Irp );
        if (irpSp->Parameters.SetFile.FileInformationClass == FileLinkInformation &&
            !renameBuffer->ReplaceIfExists &&
            ioStatus.Information == FILE_EXISTS) {

            //
            // The target file exists, and the caller does not want to replace
            // it.  This is a name collision error so cleanup and return.
            //

            ObCloseHandle( handle , KernelMode);
            status = STATUS_OBJECT_NAME_COLLISION;

        } else {

            //
            // Everything up to this point is fine, so dereference the handle
            // to a pointer to the file object and ensure that the two file
            // specifications refer to the same device.
            //

            status = ObReferenceObjectByHandle( handle,
                                              accessMask,
                                              IoFileObjectType,
                                              KernelMode,
                                              (PVOID *) &targetFileObject,
                                              &handleInformation );
            if (NT_SUCCESS( status )) {

                ObDereferenceObject( targetFileObject );

                if (IoGetRelatedDeviceObject( targetFileObject) !=
                    IoGetRelatedDeviceObject( FileObject )) {

                    //
                    // The two files refer to different devices.  Clean everything
                    // up and return an appropriate error.
                    //

                    ObCloseHandle( handle, KernelMode );
                    status = STATUS_NOT_SAME_DEVICE;

                } else {

                    //
                    // Otherwise, everything worked, so allow the rename operation
                    // to continue.
                    //

                    irpSp->Parameters.SetFile.FileObject = targetFileObject;
                    *TargetHandle = handle;
                    status = STATUS_SUCCESS;

                }

            } else {

                //
                // There was an error referencing the handle to what should
                // have been the target directory.  This generally means that
                // there was a resource problem or the handle was invalid, etc.
                // Simply attempt to close the handle and return the error.
                //

                ObCloseHandle( handle , KernelMode);

            }

        }
    }

    //
    // Return the final status of the operation.
    //

    return status;
}

NTSTATUS
IopOpenRegistryKey(
    OUT PHANDLE Handle,
    IN HANDLE BaseHandle OPTIONAL,
    IN PUNICODE_STRING KeyName,
    IN ACCESS_MASK DesiredAccess,
    IN BOOLEAN Create
    )

/*++

Routine Description:

    Opens or creates a VOLATILE registry key using the name passed in based
    at the BaseHandle node.

Arguments:

    Handle - Pointer to the handle which will contain the registry key that
        was opened.

    BaseHandle - Handle to the base path from which the key must be opened.

    KeyName - Name of the Key that must be opened/created.

    DesiredAccess - Specifies the desired access that the caller needs to
        the key.

    Create - Determines if the key is to be created if it does not exist.

Return Value:

   The function value is the final status of the operation.

--*/

{
    OBJECT_ATTRIBUTES objectAttributes;
    ULONG disposition;

    PAGED_CODE();

    //
    // Initialize the object for the key.
    //

    InitializeObjectAttributes( &objectAttributes,
                                KeyName,
                                OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,
                                BaseHandle,
                                (PSECURITY_DESCRIPTOR) NULL );

    //
    // Create the key or open it, as appropriate based on the caller's
    // wishes.
    //

    if (Create) {
        return ZwCreateKey( Handle,
                            DesiredAccess,
                            &objectAttributes,
                            0,
                            (PUNICODE_STRING) NULL,
                            REG_OPTION_VOLATILE,
                            &disposition );
    } else {
        return ZwOpenKey( Handle,
                          DesiredAccess,
                          &objectAttributes );
    }
}

NTSTATUS
IopQueryXxxInformation(
    IN PFILE_OBJECT FileObject,
    IN ULONG InformationClass,
    IN ULONG Length,
    IN KPROCESSOR_MODE Mode,
    OUT PVOID Information,
    OUT PULONG ReturnedLength,
    IN BOOLEAN FileInformation
    )

/*++

Routine Description:

    This routine returns the requested information about a specified file
    or volume.  The information returned is determined by the class that
    is specified, and it is placed into the caller's output buffer.

Arguments:

    FileObject - Supplies a pointer to the file object about which the requested
        information is returned.

    FsInformationClass - Specifies the type of information which should be
        returned about the file/volume.

    Length - Supplies the length of the buffer in bytes.

    FsInformation - Supplies a buffer to receive the requested information
        returned about the file.  This buffer must not be pageable and must
        reside in system space.

    ReturnedLength - Supplies a variable that is to receive the length of the
        information written to the buffer.

    FileInformation - Boolean that indicates whether the information requested
        is for a file or a volume.

    Mode - Previous mode of the caller to this API.

Return Value:

    The status returned is the final completion status of the operation.

--*/

{
    PIRP irp;
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject;
    KEVENT event;
    PIO_STACK_LOCATION irpSp;
    IO_STATUS_BLOCK localIoStatus;
    BOOLEAN synchronousIo;

    PAGED_CODE();

    //
    // Reference the file object here so that no special checks need be made
    // in I/O completion to determine whether or not to dereference the file
    // object.
    //

    ObReferenceObject( FileObject );

    //
    // Make a special check here to determine whether this is a synchronous
    // I/O operation.  If it is, then wait here until the file is owned by
    // the current thread.  If this is not a (serialized) synchronous I/O
    // operation, then initialize the local event.
    //

    if (FileObject->Flags & FO_SYNCHRONOUS_IO) {

        BOOLEAN interrupted;

        if (!IopAcquireFastLock( FileObject )) {
            status = IopAcquireFileObjectLock( FileObject,
                                               Mode,
                                               (BOOLEAN) ((FileObject->Flags & FO_ALERTABLE_IO) != 0),
                                               &interrupted );
            if (interrupted) {
                ObDereferenceObject( FileObject );
                return status;
            }
        }
        KeClearEvent( &FileObject->Event );
        synchronousIo = TRUE;
    } else {
        KeInitializeEvent( &event, SynchronizationEvent, FALSE );
        synchronousIo = FALSE;
    }

    //
    // Get the address of the target device object.
    //

    deviceObject = IoGetRelatedDeviceObject( FileObject );

    //
    // Allocate and initialize the I/O Request Packet (IRP) for this operation.
    // The allocation is performed with an exception handler in case the
    // caller does not have enough quota to allocate the packet.
    //

    irp = IoAllocateIrp( deviceObject->StackSize, !synchronousIo );
    if (!irp) {

        //
        // An IRP could not be allocated.  Cleanup and return an appropriate
        // error status code.
        //

        IopAllocateIrpCleanup( FileObject, (PKEVENT) NULL );

        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->Tail.Overlay.OriginalFileObject = FileObject;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    irp->RequestorMode = Mode;

    //
    // Fill in the service independent parameters in the IRP.
    //

    if (synchronousIo) {
        irp->UserEvent = (PKEVENT) NULL;
    } else {
        irp->UserEvent = &event;
        irp->Flags = IRP_SYNCHRONOUS_API;
    }
    irp->UserIosb = &localIoStatus;
    irp->Overlay.AsynchronousParameters.UserApcRoutine = (PIO_APC_ROUTINE) NULL;

    //
    // Get a pointer to the stack location for the first driver.  This will be
    // used to pass the original function codes and parameters.
    //

    irpSp = IoGetNextIrpStackLocation( irp );
    irpSp->MajorFunction = FileInformation ?
                           IRP_MJ_QUERY_INFORMATION :
                           IRP_MJ_QUERY_VOLUME_INFORMATION;
    irpSp->FileObject = FileObject;

    //
    // Set the system buffer address to the address of the caller's buffer and
    // set the flags so that the buffer is not deallocated.
    //

    irp->AssociatedIrp.SystemBuffer = Information;
    irp->Flags |= IRP_BUFFERED_IO;

    //
    // Copy the caller's parameters to the service-specific portion of the
    // IRP.
    //

    if (FileInformation) {
        irpSp->Parameters.QueryFile.Length = Length;
        irpSp->Parameters.QueryFile.FileInformationClass = InformationClass;
    } else {
        irpSp->Parameters.QueryVolume.Length = Length;
        irpSp->Parameters.QueryVolume.FsInformationClass = InformationClass;
    }

    //
    // Insert the packet at the head of the IRP list for the thread.
    //

    IopQueueThreadIrp( irp );

    //
    // Now simply invoke the driver at its dispatch entry with the IRP.
    //

    status = IoCallDriver( deviceObject, irp );

    //
    // If this operation was a synchronous I/O operation, check the return
    // status to determine whether or not to wait on the file object.  If
    // the file object is to be waited on, wait for the operation to complete
    // and obtain the final status from the file object itself.
    //

    if (synchronousIo) {
        if (status == STATUS_PENDING) {
            status = KeWaitForSingleObject( &FileObject->Event,
                                            Executive,
                                            Mode,
                                            (BOOLEAN) ((FileObject->Flags & FO_ALERTABLE_IO) != 0),
                                            (PLARGE_INTEGER) NULL );
            if (status == STATUS_ALERTED) {
                IopCancelAlertedRequest( &FileObject->Event, irp );
            }
            status = FileObject->FinalStatus;
        }
        IopReleaseFileObjectLock( FileObject );

    } else {

        //
        // This is a normal synchronous I/O operation, as opposed to a
        // serialized synchronous I/O operation.  For this case, wait
        // for the local event and copy the final status information
        // back to the caller.
        //

        if (status == STATUS_PENDING) {
            (VOID) KeWaitForSingleObject( &event,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          (PLARGE_INTEGER) NULL );
            status = localIoStatus.Status;
        }
    }

    *ReturnedLength = (ULONG) localIoStatus.Information;
    return status;
}

VOID
IopRaiseHardError(
    IN PVOID NormalContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )

/*++

Routine Description:

    This routine raises a hard error popup in the context of the current
    thread.  The APC was used to get into the context of this thread so that
    the popup would be sent to the appropriate port.

Arguments:

    NormalContext - Supplies a pointer to the I/O Request Packet (IRP) that
        was initially used to request the operation that has failed.

    SystemArgument1 - Supplies a pointer to the media's volume parameter block.
        See IoRaiseHardError documentation for more information.

    SystemArgument2 - Supplies a pointer to the real device object.  See
        IoRaiseHardError documentation for more information.

Return Value:

    None.

--*/

{
    ULONG_PTR parameters[3];
    ULONG numberOfParameters;
    ULONG parameterMask;
    ULONG response;
    NTSTATUS status;
    PIRP irp = (PIRP) NormalContext;
    PVPB vpb = (PVPB) SystemArgument1;
    PDEVICE_OBJECT realDeviceObject = (PDEVICE_OBJECT) SystemArgument2;

    ULONG length = 0;
    POBJECT_NAME_INFORMATION objectName;

    UNICODE_STRING labelName;

    //
    // Determine the name of the device and the volume label of the offending
    // media.  Start by determining the size of the DeviceName, and allocate
    // enough storage for both the ObjectName structure and the string
    //

    ObQueryNameString( realDeviceObject, NULL, 0, &length );

    if ((objectName = ExAllocatePool(PagedPool, length)) == NULL) {

        status = STATUS_INSUFFICIENT_RESOURCES;

    } else {

        status = STATUS_SUCCESS;
    }

    if (!NT_SUCCESS( status ) ||
        !NT_SUCCESS( status = ObQueryNameString( realDeviceObject,
                                                 objectName,
                                                 length,
                                                 &response ) )) {

        //
        // Allocation of the pool to put up this popup did not work or
        // something else failed, so there isn't really much that can be
        // done here.  Simply return an error back to the user.
        //

        if (objectName) {
            ExFreePool( objectName );
        }

        irp->IoStatus.Status = status;
        irp->IoStatus.Information = 0;

        IoCompleteRequest( irp, IO_DISK_INCREMENT );

        return;
    }

    //
    // The volume label has a max size of 32 characters (Unicode).  Convert
    // it to a Unicode string for output in the popup message.
    //

    if (vpb != NULL && vpb->Flags & VPB_MOUNTED) {

        labelName.Buffer = &vpb->VolumeLabel[0];
        labelName.Length = vpb->VolumeLabelLength;
        labelName.MaximumLength = MAXIMUM_VOLUME_LABEL_LENGTH;

    } else {

        RtlInitUnicodeString( &labelName, NULL );
    }

    //
    // Different pop-ups have different printf formats.  Depending on the
    // specific error value, adjust the parameters.
    //

    switch( irp->IoStatus.Status ) {

    case STATUS_MEDIA_WRITE_PROTECTED:
    case STATUS_WRONG_VOLUME:

        numberOfParameters = 3;
        parameterMask = 3;

        parameters[0] = (ULONG_PTR) &labelName;
        parameters[1] = (ULONG_PTR) &objectName->Name;
        parameters[2] = (ULONG_PTR) &PsGetCurrentProcess()->UniqueProcessId;

        break;

    case STATUS_DEVICE_NOT_READY:
    case STATUS_IO_TIMEOUT:
    case STATUS_NO_MEDIA_IN_DEVICE:
    case STATUS_UNRECOGNIZED_MEDIA:

        numberOfParameters = 2;
        parameterMask = 1;

        parameters[0] = (ULONG_PTR) &objectName->Name;
        parameters[1] = (ULONG_PTR) &PsGetCurrentProcess()->UniqueProcessId;
        parameters[2] = 0;

        break;

    default:

        numberOfParameters = 0;
        parameterMask = 0;

    }

    //
    // Simply raise the hard error.
    //

    if (ExReadyForErrors) {
        PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation( irp );
        BOOLEAN attached = FALSE;

        if ((irpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL) &&
            (irpSp->MinorFunction == IRP_MN_MOUNT_VOLUME)) {
            
            KeAttachProcess( &(THREAD_TO_PROCESS( irp->Tail.Overlay.Thread ))->Pcb );
            
            attached = TRUE;        
        }
        
        status = ExRaiseHardError( irp->IoStatus.Status,
                                   numberOfParameters,
                                   parameterMask,
                                   parameters,
                                   OptionCancelTryContinue,
                                   &response );
        
        if (attached) {           
            
            KeDetachProcess();        
        }

    } else {

        status = STATUS_UNSUCCESSFUL;
        response = ResponseReturnToCaller;
    }

    //
    // Free any pool or other resources that were allocated to output the
    // popup.
    //

    ExFreePool( objectName );

    //
    // If there was a problem, or the user didn't want to retry, just
    // complete the request.  Otherwise simply call the driver entry
    // point and retry the IRP as if it had never been tried before.
    //

    if (!NT_SUCCESS( status ) || response != ResponseTryAgain) {

        //
        // Before completing the request, make one last check.  If this was
        // a mount request, and the reason for the failure was t/o, no media,
        // or unrecognized media, then set the Information field of the status
        // block to indicate whether or not an abort was performed.
        //

        if (response == ResponseCancel) {
            PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation( irp );
            if (irpSp->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
                irpSp->MinorFunction == IRP_MN_MOUNT_VOLUME) {
                irp->IoStatus.Information = IOP_ABORT;
            } else {
                irp->IoStatus.Status = STATUS_REQUEST_ABORTED;
            }
        }

        //
        // An error was incurred, so zero out the information field before
        // completing the request if this was an input operation.  Otherwise,
        // IopCompleteRequest will try to copy to the user's buffer.
        //

        if (irp->Flags & IRP_INPUT_OPERATION) {
            irp->IoStatus.Information = 0;
        }

        IoCompleteRequest( irp, IO_DISK_INCREMENT );

    } else {

        PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation( irp );
        PDEVICE_OBJECT fsDeviceObject = irpSp->DeviceObject;
        PDRIVER_OBJECT driverObject = fsDeviceObject->DriverObject;

        //
        // Retry the request from the top.
        //

        driverObject->MajorFunction[irpSp->MajorFunction]( fsDeviceObject,
                                                           irp );

    }
}

VOID
IopRaiseInformationalHardError(
    IN PVOID NormalContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )

/*++

Routine Description:

    This routine performs the actual pop-up.  It will called from either the
    hard-error thread, or a APC routine in a user thread after exiting the
    file system.

Arguments:

    NormalContext - Contains the information for the pop-up

    SystemArgument1 - not used.

    SystemArgument1 - not used.

Return Value:

    None.

--*/

{
    ULONG parameterPresent;
    ULONG_PTR errorParameter;
    ULONG errorResponse;
    PIOP_HARD_ERROR_PACKET hardErrorPacket;

    UNREFERENCED_PARAMETER( SystemArgument1 );
    UNREFERENCED_PARAMETER( SystemArgument2 );

    hardErrorPacket = (PIOP_HARD_ERROR_PACKET) NormalContext;

    //
    // Simply raise the hard error if the system is ready to accept one.
    //

    errorParameter = (ULONG_PTR) &hardErrorPacket->String;

    parameterPresent = (hardErrorPacket->String.Buffer != NULL);

    if (ExReadyForErrors) {
        (VOID) ExRaiseHardError( hardErrorPacket->ErrorStatus,
                                 parameterPresent,
                                 parameterPresent,
                                 parameterPresent ? &errorParameter : NULL,
                                 OptionOkNoWait,
                                 &errorResponse );
    }

    //
    // Now free the packet and the buffer, if one was specified.
    //

    if (hardErrorPacket->String.Buffer) {
        ExFreePool( hardErrorPacket->String.Buffer );
    }

    ExFreePool( hardErrorPacket );
    InterlockedDecrement(&IopHardError.NumPendingApcPopups);
}

VOID
IopReadyDeviceObjects(
    IN PDRIVER_OBJECT DriverObject
    )

/*++

Routine Description:

    This routine is invoked to mark all of the device objects owned by the
    specified driver as having been fully initialized and therefore ready
    for access by other drivers/clients.

Arguments:

    DriverObject - Supplies a pointer to the driver object for the driver
        whose devices are to be marked as being "ready".

Return Value:

    None.

--*/

{
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;

    PAGED_CODE();

    //
    // Loop through all of the driver's device objects, clearing the
    // DO_DEVICE_INITIALIZING flag.
    //

    DriverObject->Flags |= DRVO_INITIALIZED;
    while (deviceObject) {
        deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
        deviceObject = deviceObject->NextDevice;
    }
}

NTSTATUS
IopResurrectDriver(
    PDRIVER_OBJECT DriverObject
    )

/*++

Routine Description:

    This routine is invoked to clear unload pending flag on all of the device
    objects owned by the specified driver, if the unload routine has not run.
    This allows the driver to come back to life after a pending unload.


Arguments:

    DriverObject - Supplies a pointer to the driver object for the driver
        whose devices are to be cleared.

Return Value:

    Status - Returns success if the driver's unload routine has not run;
        otherwise STATUS_IMAGE_ALREADY_LOADED is returned.

--*/

{
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
    KIRQL irql;

    //
    // Acquire the I/O spinlock that protects the device list and
    // driver flags.
    //

    irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );

    if (DriverObject->Flags & DRVO_UNLOAD_INVOKED || !deviceObject ||
        !(deviceObject->DeviceObjectExtension->ExtensionFlags & DOE_UNLOAD_PENDING)) {

        KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
        return STATUS_IMAGE_ALREADY_LOADED;
    }

    //
    // Loop through all of the driver's device objects, clearing the
    // DOE_UNLOAD_PENDING flag.
    //

    while (deviceObject) {
        deviceObject->DeviceObjectExtension->ExtensionFlags &= ~DOE_UNLOAD_PENDING;
        deviceObject = deviceObject->NextDevice;
    }

    KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
    return STATUS_SUCCESS;

}

VOID
IopMarshalIds(
    OUT PTRACKING_BUFFER TrackingBuffer,
    IN  PFILE_VOLUMEID_WITH_TYPE  TargetVolumeId,
    IN  PFILE_OBJECTID_BUFFER  TargetObjectId,
    IN  PFILE_TRACKING_INFORMATION TrackingInfo
    )

/*++

Routine Description:

    This routine marshals the TargetVolumeId and TargetObjectId
    into the supplied TrackingBuffer in a standard remotable format.

    It also clears the DestinationFile handle to NULL, and sets the
    ObjectInformationLength to the size of the marshalled data.

Arguments:

    TrackingBuffer - The buffer to receive the marshalled parameters.

    TargetVolumeId - The volume id to marshal.

    TargetObjectId - The object id to marshal.

    TrackingInfo   - The additional tracking information to marshal.

--*/

{
    ULONG ObjectInformationLength = 0;

    TrackingBuffer->TrackingInformation.DestinationFile = (HANDLE) NULL;

    RtlZeroMemory( &TrackingBuffer->TrackingInformation.ObjectInformation[ ObjectInformationLength ],
                   sizeof(TargetVolumeId->Type) );

    RtlCopyMemory( &TrackingBuffer->TrackingInformation.ObjectInformation[ ObjectInformationLength ],
                   &TargetVolumeId->Type,
                   sizeof(TargetVolumeId->Type) );
    ObjectInformationLength += sizeof(TargetVolumeId->Type);

    RtlCopyMemory( &TrackingBuffer->TrackingInformation.ObjectInformation[ ObjectInformationLength ],
                   &TargetVolumeId->VolumeId[0],
                   sizeof(TargetVolumeId->VolumeId) );
    ObjectInformationLength += sizeof(TargetVolumeId->VolumeId);

    RtlCopyMemory( &TrackingBuffer->TrackingInformation.ObjectInformation[ ObjectInformationLength ],
                   &TargetObjectId->ObjectId[0],
                   sizeof(TargetObjectId->ObjectId) );
    ObjectInformationLength += sizeof(TargetObjectId->ObjectId);

    RtlCopyMemory( &TrackingBuffer->TrackingInformation.ObjectInformation[ ObjectInformationLength ],
                   &TrackingInfo->ObjectInformation[0],
                   TrackingInfo->ObjectInformationLength );
    ObjectInformationLength += TrackingInfo->ObjectInformationLength;

    TrackingBuffer->TrackingInformation.ObjectInformationLength = ObjectInformationLength;

}

VOID
IopUnMarshalIds(
    IN  FILE_TRACKING_INFORMATION * TrackingInformation,
    OUT FILE_VOLUMEID_WITH_TYPE * TargetVolumeId,
    OUT GUID * TargetObjectId,
    OUT GUID * TargetMachineId
    )

/*++

Routine Description:

    This routine unmarshals the TargetVolumeId and TargetObjectId
    from the supplied TrackingInformation from a standard remotable format.

Arguments:

    TrackingInformation - The buffer containing the marshalled parameters.

    TargetVolumeId - Buffer to receive the volume id.

    TargetObjectId - Buffer to receive the object id.

    TargetMachineId - Buffer to receive the machine id.

--*/

{
    ULONG ObjectInformationLength = 0;

    RtlCopyMemory( &TargetVolumeId->Type,
                   &TrackingInformation->ObjectInformation[ ObjectInformationLength ],
                   sizeof(TargetVolumeId->Type) );
    ObjectInformationLength += sizeof(TargetVolumeId->Type);


    RtlCopyMemory( &TargetVolumeId->VolumeId[0],
                   &TrackingInformation->ObjectInformation[ ObjectInformationLength ],
                   sizeof(TargetVolumeId->VolumeId) );
    ObjectInformationLength += sizeof(TargetVolumeId->VolumeId);

    RtlCopyMemory( TargetObjectId,
                   &TrackingInformation->ObjectInformation[ ObjectInformationLength ],
                   sizeof(*TargetObjectId) );
    ObjectInformationLength += sizeof(*TargetObjectId);

    if( TrackingInformation->ObjectInformationLength > ObjectInformationLength ) {
        RtlCopyMemory( TargetMachineId,
                       &TrackingInformation->ObjectInformation[ ObjectInformationLength ],
                       min( sizeof(*TargetMachineId), TrackingInformation->ObjectInformationLength - ObjectInformationLength) );
        // ObjectInformationLength += sizeof(GUID);
    }
}


NTSTATUS
IopSendMessageToTrackService(
    IN PFILE_VOLUMEID_WITH_TYPE SourceVolumeId,
    IN PFILE_OBJECTID_BUFFER SourceObjectId,
    IN PFILE_TRACKING_INFORMATION TargetObjectInformation
    )

/*++

Routine Description:

    This routine is invoked to send a message to the user-mode link tracking
    service to inform it that a file has been moved so that it can track it
    by its object ID.

Arguments:

    SourceVolumeId - Volume ID of the source file.

    SourceObjectId - Object ID of the source file.

    TargetObjectInformation - Volume ID, object ID of the target file.

Return Value:

    The final function value is the final completion status of the operation.


--*/

{
    typedef struct _LINK_TRACKING_MESSAGE {
        NTSTATUS Status;
        ULONG Request;
        FILE_VOLUMEID_WITH_TYPE SourceVolumeId;    // src vol type & id
        FILE_OBJECTID_BUFFER     SourceObjectId;    // src obj id & birth info
        FILE_VOLUMEID_WITH_TYPE TargetVolumeId;    // tgt vol type & id
        GUID TargetObjectId;                        // tgt obj id
        GUID TargetMachineId;
    } LINK_TRACKING_MESSAGE, *PLINK_TRACKING_MESSAGE;

    typedef struct _LINK_TRACKING_RESPONSE {
        NTSTATUS Status;
    } LINK_TRACKING_RESPONSE, *PLINK_TRACKING_RESPONSE;

    PPORT_MESSAGE portMessage;
    PPORT_MESSAGE portReplyMessage;
    CHAR portReply[ 256 ];
    PLINK_TRACKING_MESSAGE requestMessage;
    PLINK_TRACKING_RESPONSE replyMessage;
    NTSTATUS status;
    ULONG loopCount = 0;
    KPROCESSOR_MODE PreviousMode;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();
    //
    // Begin by determining whether or not the LPC port to the link tracking
    // service has been opened.  If not, then attempt to open it now.
    //

retry:

    if (!IopLinkTrackingServiceObject) {

        //
        // The port has not yet been opened.  Check to see whether or not
        // the service has been started.  If not, then get out now as there
        // will be no port if the service is not running.
        //

        if (!KeReadStateEvent( IopLinkTrackingServiceEvent )) {
            return STATUS_NO_TRACKING_SERVICE;
        }

                for (;; ) {
                        status = KeWaitForSingleObject(&IopLinkTrackingPortObject,
                                                                                  Executive,
                                                                                  PreviousMode,
                                                                                  FALSE,
                                                                                  (PLARGE_INTEGER) NULL );

                        if ((status == STATUS_USER_APC) || (status == STATUS_ALERTED)) {
                                return status;
                        }

                        //
                        // There is no referenced object pointer to the
                        // link tracking port so open it.
                        //
                        if (!IopLinkTrackingServiceObject)  {
                                ExInitializeWorkItem(
                                        &IopLinkTrackingPacket.WorkQueueItem,
                                        IopConnectLinkTrackingPort,
                                        &IopLinkTrackingPacket);
                                (VOID)KeResetEvent(&IopLinkTrackingPacket.Event);
                                ExQueueWorkItem( &IopLinkTrackingPacket.WorkQueueItem,
                                                                        DelayedWorkQueue );
                                status = KeWaitForSingleObject(
                                                        &IopLinkTrackingPacket.Event,
                                                        Executive,
                                                        PreviousMode,
                                                        FALSE,
                                                        (PLARGE_INTEGER) NULL );

                                if ((status == STATUS_USER_APC) || (status == STATUS_ALERTED)) {
                                        NOTHING;
                                } else if (!NT_SUCCESS( IopLinkTrackingPacket.FinalStatus )) {
                                        status = IopLinkTrackingPacket.FinalStatus;
                                }

                                KeSetEvent(&IopLinkTrackingPortObject,
                                                0,
                                                FALSE);
                                if (status == STATUS_SUCCESS) {
                                                break;
                                } else {
                                        return status;
                                }

                        } else {
                                //
                                // The connection is established.
                                //

                                KeSetEvent(&IopLinkTrackingPortObject,
                                                0,
                                                FALSE);
                                break;
                        }
        }
    }

    //
    // Form a message from the input parameters and send it to the caller.
    //

    portMessage = ExAllocatePool( PagedPool,
                                  sizeof( LINK_TRACKING_MESSAGE ) +
                                  sizeof( PORT_MESSAGE ) );
    if (!portMessage) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    requestMessage = (PLINK_TRACKING_MESSAGE) (portMessage + 1);
    RtlZeroMemory( requestMessage, sizeof(*requestMessage) );

    requestMessage->Status = STATUS_SUCCESS;
    requestMessage->Request = 0;

    RtlCopyMemory( &requestMessage->SourceVolumeId,
                   SourceVolumeId,
                   sizeof( FILE_VOLUMEID_WITH_TYPE ) );

    RtlCopyMemory( &requestMessage->SourceObjectId,
                   SourceObjectId,
                   sizeof( FILE_OBJECTID_BUFFER ) );

    IopUnMarshalIds(  TargetObjectInformation,
                   &requestMessage->TargetVolumeId,
                   &requestMessage->TargetObjectId,
                   &requestMessage->TargetMachineId);

    portMessage->u1.s1.TotalLength = (USHORT) (sizeof( PORT_MESSAGE ) +
                                              sizeof( LINK_TRACKING_MESSAGE ));
    portMessage->u1.s1.DataLength = (USHORT) sizeof( LINK_TRACKING_MESSAGE );
    portMessage->u2.ZeroInit = 0;

    status = LpcRequestWaitReplyPort( IopLinkTrackingServiceObject,
                                      portMessage,
                                      (PPORT_MESSAGE) &portReply[0] );
    if (!NT_SUCCESS( status )) {
        if (status == STATUS_PORT_DISCONNECTED) {
                        status = KeWaitForSingleObject(&IopLinkTrackingPortObject,
                                                                                                Executive,
                                                                                                PreviousMode,
                                                                                                FALSE,
                                                                                                (PLARGE_INTEGER) NULL );
            ObDereferenceObject( IopLinkTrackingServiceObject );
                        IopLinkTrackingServiceObject = NULL;
                        KeSetEvent(&IopLinkTrackingPortObject,
                                0,
                                FALSE);
            if (!loopCount) {
                loopCount += 1;
                goto retry;
            }
        }
    }

    if (NT_SUCCESS( status )) {
        portReplyMessage = (PPORT_MESSAGE) &portReply[0];
        replyMessage = (PLINK_TRACKING_RESPONSE) (portReplyMessage + 1);
        status = replyMessage->Status;
    }

    return status;
}

NTSTATUS
IopSetEaOrQuotaInformationFile(
    IN HANDLE FileHandle,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN PVOID Buffer,
    IN ULONG Length,
    IN BOOLEAN SetEa
    )

/*++

Routine Description:

    This routine is invoked by the NtSetEa[Quota]InformationFile system services
    to either modify the EAs on a file or the quota entries on a volume.  All of
    the specified entries in the buffer are made to the file or volume.

Arguments:

    FileHandle - Supplies a handle to the file/volume for which the entries are
        to be applied.

    IoStatusBlock - Address of the caller's I/O status block.

    Buffer - Supplies a buffer containing the entries to be added/modified.

    Length - Supplies the length, in bytes, of the buffer.

    SetEa - A BOOLEAN that indicates whether to change the EAs on a file or
        the quota entries on the volume.

Return Value:

    The status returned is the final completion status of the operation.

--*/

{
    PIRP irp;
    NTSTATUS status;
    PFILE_OBJECT fileObject;
    PDEVICE_OBJECT deviceObject;
    PKEVENT event = (PKEVENT) NULL;
    KPROCESSOR_MODE requestorMode;
    PIO_STACK_LOCATION irpSp;
    IO_STATUS_BLOCK localIoStatus;
    BOOLEAN synchronousIo;
    PETHREAD CurrentThread;

    PAGED_CODE();

    //
    // Get the previous mode;  i.e., the mode of the caller.
    //

    CurrentThread = PsGetCurrentThread ();
    requestorMode = KeGetPreviousModeByThread(&CurrentThread->Tcb);

    if (requestorMode != KernelMode) {

        //
        // The caller's access mode is user, so probe each of the arguments
        // and capture them as necessary.  If any failures occur, the condition
        // handler will be invoked to handle them.  It will simply cleanup and
        // return an access violation status code back to the system service
        // dispatcher.
        //

        try {

            //
            // The IoStatusBlock parameter must be writeable by the caller.
            //

            ProbeForWriteIoStatus( IoStatusBlock);

            //
            // The Buffer parameter must be readable by the caller.
            //

            ProbeForRead( Buffer, Length, sizeof( ULONG ) );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            //
            // An exception was incurred while probing the caller's parameters.
            // Cleanup and return an appropriate error status code.
            //

            return GetExceptionCode();
        }
    }

    //
    // There were no blatant errors so far, so reference the file object so
    // the target device object can be found.  Note that if the handle does
    // not refer to a file object, or if the caller does not have the required
    // access to the file, then it will fail.
    //

    status = ObReferenceObjectByHandle( FileHandle,
                                        SetEa ? FILE_WRITE_EA : FILE_WRITE_DATA,
                                        IoFileObjectType,
                                        requestorMode,
                                        (PVOID *) &fileObject,
                                        NULL );
    if (!NT_SUCCESS( status )) {
        return status;
    }

    //
    // Make a special check here to determine whether this is a synchronous
    // I/O operation.  If it is, then wait here until the file is owned by
    // the current thread.  If this is not a (serialized) synchronous I/O
    // operation, then allocate and initialize the local event.
    //

    if (fileObject->Flags & FO_SYNCHRONOUS_IO) {

        BOOLEAN interrupted;

        if (!IopAcquireFastLock( fileObject )) {
            status = IopAcquireFileObjectLock( fileObject,
                                               requestorMode,
                                               (BOOLEAN) ((fileObject->Flags & FO_ALERTABLE_IO) != 0),
                                               &interrupted );
            if (interrupted) {
                ObDereferenceObject( fileObject );
                return status;
            }
        }
        synchronousIo = TRUE;
    } else {

        //
        // This is a synchronous API being invoked for a file that is opened
        // for asynchronous I/O.  This means that this system service is
        // to synchronize the completion of the operation before returning
        // to the caller.  A local event is used to do this.
        //

        event = ExAllocatePool( NonPagedPool, sizeof( KEVENT ) );
        if (!event) {
            ObDereferenceObject( fileObject );
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        KeInitializeEvent( event, SynchronizationEvent, FALSE );
        synchronousIo = FALSE;
    }

    //
    // Set the file object to the Not-Signaled state.
    //

    KeClearEvent( &fileObject->Event );

    //
    // Get the address of the target device object.
    //

    deviceObject = IoGetRelatedDeviceObject( fileObject );

    //
    // Allocate and initialize the I/O Request Packet (IRP) for this operation.
    // The allocation is performed with an exception handler in case the
    // caller does not have enough quota to allocate the packet.

    irp = IoAllocateIrp( deviceObject->StackSize, FALSE );
    if (!irp) {

        //
        // An IRP could not be allocated.  Cleanup and return an appropriate
        // error status code.
        //

        if (!(fileObject->Flags & FO_SYNCHRONOUS_IO)) {
            ExFreePool( event );
        }

        IopAllocateIrpCleanup( fileObject, (PKEVENT) NULL );

        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->Tail.Overlay.OriginalFileObject = fileObject;
    irp->Tail.Overlay.Thread = CurrentThread;
    irp->RequestorMode = requestorMode;

    //
    // Fill in the service independent parameters in the IRP.
    //

    if (synchronousIo) {
        irp->UserEvent = (PKEVENT) NULL;
        irp->UserIosb = IoStatusBlock;
    } else {
        irp->UserEvent = event;
        irp->UserIosb = &localIoStatus;
        irp->Flags = IRP_SYNCHRONOUS_API;
    }
    irp->Overlay.AsynchronousParameters.UserApcRoutine = (PIO_APC_ROUTINE) NULL;

    //
    // Get a pointer to the stack location for the first driver.  This will be
    // used to pass the original function codes and parameters.
    //

    irpSp = IoGetNextIrpStackLocation( irp );
    irpSp->MajorFunction = SetEa ? IRP_MJ_SET_EA : IRP_MJ_SET_QUOTA;
    irpSp->FileObject = fileObject;

    //
    // Now determine whether this driver expects to have data buffered to it
    // or whether it performs direct I/O.  This is based on the DO_BUFFERED_IO
    // flag in the device object.  if the flag is set, then a system buffer is
    // allocated and driver's data is copied to it.  If the DO_DIRECT_IO flag
    // is set in the device object, then a Memory Descriptor List (MDL) is
    // allocated and the caller's buffer is locked down using it.  Finally, if
    // the driver specifies neither of the flags, then simply pass the address
    // and length of the buffer and allow the driver to perform all of the
    // checking and buffering if any is required.
    //

    if (deviceObject->Flags & DO_BUFFERED_IO) {

        PVOID systemBuffer;
        ULONG errorOffset;

        if (Length) {
            //
            // The driver wishes the caller's buffer to be copied into an
            // intermediary buffer.  Allocate the system buffer and specify
            // that it should be deallocated on completion.  Also check to
            // ensure that the caller's EA list or quota list is valid.  All
            // of this is performed within an exception handler that will perform
            // cleanup if the operation fails.
            //

            try {

                //
                // Allocate the intermediary system buffer and charge the caller
                // quota for its allocation.  Copy the caller's buffer into the
                // system buffer and check to ensure that it is valid.
                //

                systemBuffer = ExAllocatePoolWithQuota( NonPagedPool, Length );

                irp->AssociatedIrp.SystemBuffer = systemBuffer;

                RtlCopyMemory( systemBuffer, Buffer, Length );

                if (SetEa) {
                    status = IoCheckEaBufferValidity( systemBuffer,
                                                      Length,
                                                      &errorOffset );
                } else {
                    status = IoCheckQuotaBufferValidity( systemBuffer,
                                                         Length,
                                                         &errorOffset );
                }

                if (!NT_SUCCESS( status )) {
                    IoStatusBlock->Status = status;
                    IoStatusBlock->Information = errorOffset;
                    ExRaiseStatus( status );
                }

            } except(EXCEPTION_EXECUTE_HANDLER) {

                //
                // An exception was incurred while allocating the buffer, copying
                // the caller's data into it, or walking the buffer.  Determine
                // what happened, cleanup, and return an appropriate error status
                // code.
                //

                IopExceptionCleanup( fileObject,
                                     irp,
                                     (PKEVENT) NULL,
                                     event );

                return GetExceptionCode();

            }

            //
            // Set the flags so that the completion code knows to deallocate the
            // buffer.
            //

            irp->Flags |= IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;

        } else {
            irp->AssociatedIrp.SystemBuffer = NULL;
            irp->UserBuffer = Buffer;
        }


    } else if (deviceObject->Flags & DO_DIRECT_IO) {

        PMDL mdl;

        if (Length) {
            //
            // This is a direct I/O operation.  Allocate an MDL and invoke the
            // memory management routine to lock the buffer into memory.  This is
            // done using an exception handler that will perform cleanup if the
            // operation fails.
            //

            mdl = (PMDL) NULL;

            try {

                //
                // Allocate an MDL, charging quota for it, and hang it off of the
                // IRP.  Probe and lock the pages associated with the caller's
                // buffer for read access and fill in the MDL with the PFNs of those
                // pages.
                //

                mdl = IoAllocateMdl( Buffer, Length, FALSE, TRUE, irp );
                if (!mdl) {
                    ExRaiseStatus( STATUS_INSUFFICIENT_RESOURCES );
                }
                IopProbeAndLockPages( mdl, requestorMode, IoReadAccess, deviceObject, irpSp->MajorFunction);

            } except(EXCEPTION_EXECUTE_HANDLER) {

                //
                // An exception was incurred while either probing the caller's
                // buffer or allocating the MDL.  Determine what actually happened,
                // clean everything up, and return an appropriate error status code.
                //

                IopExceptionCleanup( fileObject,
                                     irp,
                                     (PKEVENT) NULL,
                                     event );

                return GetExceptionCode();

            }
        }

    } else {

        //
        // Pass the address of the user's buffer so the driver has access to
        // it.  It is now the driver's responsibility to do everything.
        //

        irp->UserBuffer = Buffer;

    }

    //
    // Copy the caller's parameters to the service-specific portion of the
    // IRP.
    //

    if (SetEa) {
        irpSp->Parameters.SetEa.Length = Length;
    } else {
        irpSp->Parameters.SetQuota.Length = Length;
    }

    //
    // Queue the packet, call the driver, and synchronize appropriately with
    // I/O completion.
    //

    status = IopSynchronousServiceTail( deviceObject,
                                        irp,
                                        fileObject,
                                        FALSE,
                                        requestorMode,
                                        synchronousIo,
                                        OtherTransfer );

    //
    // If the file for this operation was not opened for synchronous I/O, then
    // synchronization of completion of the I/O operation has not yet occurred
    // since the allocated event must be used for synchronous APIs on files
    // opened for asynchronous I/O.  Synchronize the completion of the I/O
    // operation now.
    //

    if (!synchronousIo) {

        status = IopSynchronousApiServiceTail( status,
                                               event,
                                               irp,
                                               requestorMode,
                                               &localIoStatus,
                                               IoStatusBlock );
    }

    return status;
}

NTSTATUS
IopSetRemoteLink(
    IN PFILE_OBJECT FileObject,
    IN PFILE_OBJECT DestinationFileObject OPTIONAL,
    IN PFILE_TRACKING_INFORMATION FileInformation OPTIONAL
    )

/*++

Routine Description:

    This routine is invoked to remote an NtSetInformationFile API call via an
    FSCTL to the Redirector.  The call will cause the remote system to perform
    the service call to track the link for a file which was just moved.

Arguments:

    FileObject - Supplies the file object for the file that was moved.

    DestinationFileObject - Optionally supplies the file object for the new
        destination location for the file.

    FileInformation - Optionally supplies the volume and file object IDs of
        the target file.

Return Value:

    The final function value is the final completion status of the operation.

--*/

{
    REMOTE_LINK_BUFFER remoteBuffer;
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;
    PIRP irp;
    KEVENT event;
    PIO_STACK_LOCATION irpSp;
    PDEVICE_OBJECT deviceObject;
    ULONG length = 0;

    PAGED_CODE();

    //
    // Initialize the event structure to synchronize completion of the I/O
    // request.
    //

    KeInitializeEvent( &event,
                       NotificationEvent,
                       FALSE );

    //
    // Build an I/O Request Packet to be sent to the file system driver to get
    // the volume ID.
    //

    deviceObject = IoGetRelatedDeviceObject( FileObject );

    irp = IoBuildDeviceIoControlRequest( FSCTL_LMR_SET_LINK_TRACKING_INFORMATION,
                                         deviceObject,
                                         NULL,
                                         0,
                                         NULL,
                                         0,
                                         FALSE,
                                         &event,
                                         &ioStatus );
    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Initialize the remote link buffer according to the input information.
    //

    if (DestinationFileObject) {

        // The FileObject and DestinationFileObject are on the same machine
        remoteBuffer.TrackingInformation.TargetFileObject = DestinationFileObject;

        if (FileInformation) {
            // Copy the ObjectInformation from the FileInformation buffer into
            // the TargetLinkTrackingInformationBuffer.  Set 'length' to include
            // this buffer.

            remoteBuffer.TrackingInformation.TargetLinkTrackingInformationLength
                = length = FileInformation->ObjectInformationLength;
            RtlCopyMemory( &remoteBuffer.TrackingInformation.TargetLinkTrackingInformationBuffer,
                           FileInformation->ObjectInformation,
                           length );
        } else {
            // We don't have any extra FileInformation.
            remoteBuffer.TrackingInformation.TargetLinkTrackingInformationLength = 0;
        }

        // Increment the length to include the size of the non-optional fields in
        // REMOTE_LINK_TRACKING_INFORMATION.
        length += sizeof( PFILE_OBJECT ) + sizeof( ULONG );

    } else {
        // There's no DestinationFileObject, so all the necessary information is in the
        // FileInformation structure.
        length = FileInformation->ObjectInformationLength + sizeof( HANDLE ) + sizeof( ULONG );
        RtlCopyMemory( &remoteBuffer.TrackingInformation,
                       FileInformation,
                       length );
        remoteBuffer.TrackingInformation.TargetFileObject = NULL;
    }

    //
    // Fill in the remainder of the IRP to retrieve the object ID for the
    // file.
    //

    irp->Flags |= IRP_SYNCHRONOUS_API;
    irp->AssociatedIrp.SystemBuffer = &remoteBuffer;
    irp->Tail.Overlay.OriginalFileObject = FileObject;

    irpSp = IoGetNextIrpStackLocation( irp );
    irpSp->FileObject = FileObject;
    irpSp->MajorFunction = IRP_MJ_FILE_SYSTEM_CONTROL;
    irpSp->MinorFunction = IRP_MN_KERNEL_CALL;
    irpSp->Parameters.FileSystemControl.InputBufferLength = length;

    //
    // Take out another reference to the file object to guarantee that it does
    // not get deleted.
    //

    ObReferenceObject( FileObject );

    //
    // Call the driver to get the request.
    //

    status = IoCallDriver( deviceObject, irp );

    //
    // Synchronize completion of the request.
    //

    if (status == STATUS_PENDING) {
        status = KeWaitForSingleObject( &event,
                                        Executive,
                                        KernelMode,
                                        FALSE,
                                        (PLARGE_INTEGER) NULL );
        status = ioStatus.Status;
    }

    return status;
}

VOID
IopStartApcHardError(
    IN PVOID StartContext
    )

/*++

Routine Description:

    This function is invoked in an ExWorker thread when we need to do a
    hard error pop-up, but the Irp's originating thread is at APC level,
    ie. IoPageRead.  It starts a thread to hold the pop-up.

Arguments:

    StartContext - Startup context, contains a IOP_APC_HARD_ERROR_PACKET.

Return Value:

    None.

--*/

{
    HANDLE thread;
    NTSTATUS status;

    //
    //  Create the hard error pop-up thread.  If for whatever reason we
    //  can't do this then just complete the Irp with the error.
    //

    status = PsCreateSystemThread( &thread,
                                   0,
                                   (POBJECT_ATTRIBUTES)NULL,
                                   (HANDLE)0,
                                   (PCLIENT_ID)NULL,
                                   IopApcHardError,
                                   StartContext );

    if ( !NT_SUCCESS( status ) ) {


        IoCompleteRequest( ((PIOP_APC_HARD_ERROR_PACKET)StartContext)->Irp,
                           IO_DISK_INCREMENT );
        ExFreePool( StartContext );
        return;
    }

    //
    //  Close thread handle
    //

    ZwClose(thread);
}

NTSTATUS
IopSynchronousApiServiceTail(
    IN NTSTATUS ReturnedStatus,
    IN PKEVENT Event,
    IN PIRP Irp,
    IN KPROCESSOR_MODE RequestorMode,
    IN PIO_STATUS_BLOCK LocalIoStatus,
    OUT PIO_STATUS_BLOCK IoStatusBlock
    )

/*++

Routine Description:

    This routine is invoked when a synchronous API is invoked for a file
    that has been opened for asynchronous I/O.  This function synchronizes
    the completion of the I/O operation on the file.

Arguments:

    ReturnedStatus - Supplies the status that was returned from the call to
        IoCallDriver.

    Event - Address of the allocated kernel event to be used for synchronization
        of the I/O operation.

    Irp - Address of the I/O Request Packet submitted to the driver.

    RequestorMode - Processor mode of the caller when the operation was
        requested.

    LocalIoStatus - Address of the I/O status block used to capture the final
        status by the service itself.

    IoStatusBlock - Address of the I/O status block supplied by the caller of
        the system service.

Return Value:

    The function value is the final status of the operation.


--*/

{
    NTSTATUS status;

    PAGED_CODE();

    //
    // This is a normal synchronous I/O operation, as opposed to a
    // serialized synchronous I/O operation.  For this case, wait for
    // the local event and copy the final status information back to
    // the caller.
    //

    status = ReturnedStatus;

    if (status == STATUS_PENDING) {

        status = KeWaitForSingleObject( Event,
                                        Executive,
                                        RequestorMode,
                                        FALSE,
                                        (PLARGE_INTEGER) NULL );

        if (status == STATUS_USER_APC) {

            //
            // The wait request has ended either because the thread was
            // alerted or an APC was queued to this thread, because of
            // thread rundown or CTRL/C processing.  In either case, try
            // to bail out of this I/O request carefully so that the IRP
            // completes before this routine exists or the event will not
            // be around to set to the Signaled state.
            //

            IopCancelAlertedRequest( Event, Irp );

        }

        status = LocalIoStatus->Status;
    }

    try {

        *IoStatusBlock = *LocalIoStatus;

    } except(EXCEPTION_EXECUTE_HANDLER) {

        //
        // An exception occurred attempting to write the caller's I/O
        // status block.  Simply change the final status of the operation
        // to the exception code.
        //

        status = GetExceptionCode();
    }

    ExFreePool( Event );

    return status;
}

NTSTATUS
IopSynchronousServiceTail(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PFILE_OBJECT FileObject,
    IN BOOLEAN DeferredIoCompletion,
    IN KPROCESSOR_MODE RequestorMode,
    IN BOOLEAN SynchronousIo,
    IN TRANSFER_TYPE TransferType
    )

/*++

Routine Description:

    This routine is invoked to complete the operation of a system service.
    It queues the IRP to the thread's queue, updates the transfer count,
    calls the driver, and finally synchronizes completion of the I/O.

Arguments:

    DeviceObject - Device on which the I/O is to occur.

    Irp - I/O Request Packet representing the I/O operation.

    FileObject - File object for this open instantiation.

    DeferredIoCompletion - Indicates whether deferred completion is possible.

    RequestorMode - Mode in which request was made.

    SynchronousIo - Indicates whether the operation is to be synchronous.

    TransferType - Type of transfer being performed: read, write, or other.

Return Value:

    The function value is the final status of the operation.

--*/

{
    NTSTATUS status;

    PAGED_CODE();

    //
    // Insert the packet at the head of the IRP list for the thread.
    //

    if (!SynchronousIo) {
        IopQueueThreadIrp( Irp );
    }

    //
    // Update the operation count statistic for the current process.
    //

    switch( TransferType ) {

    case ReadTransfer:
        IopUpdateReadOperationCount();
        break;

    case WriteTransfer:
        IopUpdateWriteOperationCount();
        break;

    case OtherTransfer:
        IopUpdateOtherOperationCount();
        break;
    }

    //
    // Now simply invoke the driver at its dispatch entry with the IRP.
    //

    status = IoCallDriver( DeviceObject, Irp );

    //
    // If deferred I/O completion is possible, check for pending returned
    // from the driver.  If the driver did not return pending, then the
    // packet has not actually been completed yet, so complete it here.
    //

    if (DeferredIoCompletion) {

        if (status != STATUS_PENDING) {

            //
            // The I/O operation was completed without returning a status of
            // pending.  This means that at this point, the IRP has not been
            // fully completed.  Complete it now.
            //

            PKNORMAL_ROUTINE normalRoutine;
            PVOID normalContext;
            KIRQL irql = PASSIVE_LEVEL; // Just to shut up the compiler

            ASSERT( !Irp->PendingReturned );

            if (!SynchronousIo) {
                KeRaiseIrql( APC_LEVEL, &irql );
            }
            IopCompleteRequest( &Irp->Tail.Apc,
                                &normalRoutine,
                                &normalContext,
                                (PVOID *) &FileObject,
                                &normalContext );

            if (!SynchronousIo) {
                KeLowerIrql( irql );
            }
        }
    }

    //
    // If this operation was a synchronous I/O operation, check the return
    // status to determine whether or not to wait on the file object.  If
    // the file object is to be waited on, wait for the operation to complete
    // and obtain the final status from the file object itself.
    //

    if (SynchronousIo) {

        if (status == STATUS_PENDING) {

            status = KeWaitForSingleObject( &FileObject->Event,
                                            Executive,
                                            RequestorMode,
                                            (BOOLEAN) ((FileObject->Flags & FO_ALERTABLE_IO) != 0),
                                            (PLARGE_INTEGER) NULL );

            if (status == STATUS_ALERTED || status == STATUS_USER_APC) {

                //
                // The wait request has ended either because the thread was alerted
                // or an APC was queued to this thread, because of thread rundown or
                // CTRL/C processing.  In either case, try to bail out of this I/O
                // request carefully so that the IRP completes before this routine
                // exists so that synchronization with the file object will remain
                // intact.
                //

                IopCancelAlertedRequest( &FileObject->Event, Irp );

            }

            status = FileObject->FinalStatus;

        }

        IopReleaseFileObjectLock( FileObject );

    }

    return status;
}

VOID
IopTimerDispatch(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )

/*++

Routine Description:

    This routine scans the I/O system timer database and invokes each driver
    that has enabled a timer in the list, once every second.

Arguments:

    Dpc - Supplies a pointer to a control object of type DPC.

    DeferredContext - Optional deferred context;  not used.

    SystemArgument1 - Optional argument 1;  not used.

    SystemArgument2 - Optional argument 2;  not used.

Return Value:

    None.

--*/

{
    PLIST_ENTRY timerEntry;
    PIO_TIMER timer;
    KIRQL irql;
    ULONG i;

    UNREFERENCED_PARAMETER( Dpc );
    UNREFERENCED_PARAMETER( DeferredContext );
    UNREFERENCED_PARAMETER( SystemArgument1 );
    UNREFERENCED_PARAMETER( SystemArgument2 );

    //
    // Check to see whether or not there are any timers in the queue that
    // have been enabled.  If so, then walk the list and invoke all of the
    // drivers' routines.  Note that if the counter changes, which it can
    // because the spin lock is not owned, then a timer routine may be
    // missed.  However, this is acceptable, since the driver inserting the
    // entry could be context switched away from, etc.  Therefore, this is
    // not a critical resource for the most part.
    //

    if (IopTimerCount) {

        //
        // There is at least one timer entry in the queue that is enabled.
        // Walk the queue and invoke each specified timer routine.
        //

        ExAcquireSpinLock( &IopTimerLock, &irql );
        i = IopTimerCount;
        timerEntry = IopTimerQueueHead.Flink;

        //
        // For each entry found that is enabled, invoke the driver's routine
        // with its specified context parameter.  The local count is used
        // to abort the queue traversal when there are more entries in the
        // queue, but they are not enabled.
        //

        for (timerEntry = IopTimerQueueHead.Flink;
             (timerEntry != &IopTimerQueueHead) && i;
             timerEntry = timerEntry->Flink ) {

            timer = CONTAINING_RECORD( timerEntry, IO_TIMER, TimerList );

            if (timer->TimerFlag) {
                timer->TimerRoutine( timer->DeviceObject, timer->Context );
                i--;
            }
        }
        ExReleaseSpinLock( &IopTimerLock, irql );
    }
}





NTSTATUS
IopTrackLink(
    IN PFILE_OBJECT FileObject,
    IN OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN PFILE_TRACKING_INFORMATION FileInformation,
    IN ULONG Length,
    IN PKEVENT Event,
    IN KPROCESSOR_MODE RequestorMode
    )

/*++

Routine Description:

    This routine is invoked to track a link.  It tracks the source file's Object
    ID to the target file so that links to the source will follow to the new
    location of the target.

Arguments:

    FileObject - Supplies a pointer to the referenced source file object.

    IoStatusBlock - Pointer to the caller's I/O status block.

    FileInformation - A buffer containing the parameters for the move that was
        performed.

    Length - Specifies the length of the FileInformation buffer.

    Event - An event to be set to the Signaled state once the operation has been
        performed, provided it was successful.

    RequestorMode - Requestor mode of the caller.

N.B. - Note that the presence of an event indicates that the source file was
    opened for asynchronous I/O, otherwise it was opened for synchronous I/O.

Return Value:

    The status returned is the final completion status of the operation.


--*/

{
    PFILE_TRACKING_INFORMATION trackingInfo = NULL;
    PFILE_OBJECT dstFileObject = NULL;
    FILE_VOLUMEID_WITH_TYPE SourceVolumeId;
    FILE_OBJECTID_BUFFER SourceObjectId;
    FILE_OBJECTID_BUFFER NormalizedObjectId;
    FILE_OBJECTID_BUFFER CrossVolumeObjectId;
    FILE_VOLUMEID_WITH_TYPE TargetVolumeId;
    FILE_OBJECTID_BUFFER TargetObjectId;
    TRACKING_BUFFER trackingBuffer;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    //
    // Begin by capturing the caller's buffer, if required.
    //

    if (RequestorMode != KernelMode) {

        try {
            trackingInfo = ExAllocatePoolWithQuota( PagedPool,
                                                    Length );
            RtlCopyMemory( trackingInfo, FileInformation, Length );

            if (!trackingInfo->DestinationFile ||
               ((Length - FIELD_OFFSET( FILE_TRACKING_INFORMATION, ObjectInformation ))
                < trackingInfo->ObjectInformationLength)) {
                ExFreePool( trackingInfo );
                return STATUS_INVALID_PARAMETER;
            }

        } except(EXCEPTION_EXECUTE_HANDLER) {

            //
            // An exception was incurred while allocating the intermediary
            // system buffer or while copying the caller's data into the
            // buffer.  Cleanup and return an appropriate error status code.
            //

            if (trackingInfo) {
                ExFreePool( trackingInfo );
            }

            return GetExceptionCode();
        }
    } else {
        trackingInfo = FileInformation;
    }

    //
    // If a destination file handle was specified, convert it to a pointer to
    // a file object.
    //

    if (trackingInfo->DestinationFile) {
        status = ObReferenceObjectByHandle( trackingInfo->DestinationFile,
                                            FILE_WRITE_DATA,
                                            IoFileObjectType,
                                            RequestorMode,
                                            (PVOID *) &dstFileObject,
                                            NULL );
        if (!NT_SUCCESS( status )) {
            if (RequestorMode != KernelMode) {
                ExFreePool( trackingInfo );
            }
            return status;
        }
    }

    try {

        //
        // Determine whether this is a local or a remote link tracking
        // operation.
        //

        if (IsFileLocal( FileObject )) {

            //
            // The source file, i.e., the one being moved, is a file local to
            // this system.  Determine the form of the target file and track
            // it accordingly.
            //

            if (trackingInfo->DestinationFile) {

                if (IsFileLocal( dstFileObject )) {

                    BOOLEAN IdSetOnTarget = FALSE;

                    //
                    // The target file is specified as a handle and it is local.
                    // Simply perform the query and set locally.  Note that if
                    // the source file does not have an object ID, then no
                    // tracking will be performed, but it will appear as if the
                    // operation worked.
                    //

                    status = IopGetSetObjectId( FileObject,
                                                &SourceObjectId,
                                                sizeof( SourceObjectId ),
                                                FSCTL_GET_OBJECT_ID );

                    if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
                        status = STATUS_SUCCESS;
                        leave;
                    }

                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // If the extended info field is zero then this file
                    // has no interesting tracking information.
                    //
                    if (RtlCompareMemoryUlong(SourceObjectId.BirthObjectId,
                                       sizeof(SourceObjectId.BirthObjectId),
                                       0) == sizeof(SourceObjectId.BirthObjectId)) {
                        status = STATUS_SUCCESS;
                        leave;
                    }

                    //
                    // Get the volume ID of the source and destination
                    //

                    status = IopGetVolumeId( dstFileObject,
                                             &TargetVolumeId,
                                             sizeof( TargetVolumeId ) );
                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    status = IopGetVolumeId( FileObject,
                                             &SourceVolumeId,
                                             sizeof( SourceVolumeId ) );
                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Delete the ID from the source now, since the
                    // target may be on the same volume.  If there's a
                    // subsequent error, we'll try to restore it.
                    //

                    status = IopGetSetObjectId( FileObject,
                                                NULL,
                                                0,
                                                FSCTL_DELETE_OBJECT_ID );
                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Set the ID on the target.  If it's a cross-volume
                    // move, set the bit that indicates same.
                    //

                    CrossVolumeObjectId = TargetObjectId = SourceObjectId;
                    if( !RtlEqualMemory( &TargetVolumeId.VolumeId[0],
                                         &SourceVolumeId.VolumeId[0],
                                         sizeof(SourceVolumeId.VolumeId) )) {
                        CrossVolumeObjectId.BirthVolumeId[0] |= 1;
                    }

                    status = IopGetSetObjectId( dstFileObject,
                                                &CrossVolumeObjectId,
                                                sizeof( CrossVolumeObjectId ),
                                                FSCTL_SET_OBJECT_ID );

                    if( status == STATUS_DUPLICATE_NAME ||
                        status == STATUS_OBJECT_NAME_COLLISION ) {

                        // This object ID is already in use on the target volume,
                        // or the dest file already has an object ID.
                        // Get the file's ID (or have NTFS generate a new one).

                        status = IopGetSetObjectId( dstFileObject,
                                                    &TargetObjectId,
                                                    sizeof(TargetObjectId),
                                                    FSCTL_CREATE_OR_GET_OBJECT_ID );
                        if( NT_SUCCESS(status) ) {

                            // Write the birth ID

                            status = IopGetSetObjectId( dstFileObject,
                                                        &CrossVolumeObjectId.ExtendedInfo[0],
                                                        sizeof( CrossVolumeObjectId.ExtendedInfo ),
                                                        FSCTL_SET_OBJECT_ID_EXTENDED );
                        }
                    }

                    if( NT_SUCCESS(status) ) {

                        IdSetOnTarget = TRUE;

                        // If this was a cross-volume move, notify the tracking service.

                        if( !RtlEqualMemory( &TargetVolumeId.VolumeId[0],
                                             &SourceVolumeId.VolumeId[0],
                                             sizeof(SourceVolumeId.VolumeId) )) {

                            IopMarshalIds( &trackingBuffer, &TargetVolumeId, &TargetObjectId, trackingInfo );

                            // Bit 0 must be reset before notifying tracking service
                            NormalizedObjectId = SourceObjectId;
                            NormalizedObjectId.BirthVolumeId[0] &= 0xfe;

                            status = IopSendMessageToTrackService( &SourceVolumeId,
                                                                   &NormalizedObjectId,
                                                                   &trackingBuffer.TrackingInformation );
                        }
                    }

                    //
                    // If there was an error after the ObjectID was deleted
                    // from the source.  Try to restore it before returning.
                    //

                    if( !NT_SUCCESS(status) ) {
                        NTSTATUS statusT = STATUS_SUCCESS;

                        if( IdSetOnTarget ) {

                            if( RtlEqualMemory( &TargetObjectId.ObjectId,
                                                &SourceObjectId.ObjectId,
                                                sizeof(TargetObjectId.ObjectId) )) {

                                // This ID was set with FSCTL_SET_OBJECT_ID
                                statusT = IopGetSetObjectId( dstFileObject,
                                                             NULL,
                                                             0,
                                                             FSCTL_DELETE_OBJECT_ID );

                            } else {

                                // Restore the target's extended data.

                                statusT = IopGetSetObjectId( dstFileObject,
                                                             &TargetObjectId.ExtendedInfo[0],
                                                             sizeof(TargetObjectId.ExtendedInfo),
                                                             FSCTL_SET_OBJECT_ID_EXTENDED );
                            }
                        }

                        if( NT_SUCCESS( statusT )) {

                            IopGetSetObjectId( FileObject,
                                               &SourceObjectId,
                                               sizeof(SourceObjectId),
                                               FSCTL_SET_OBJECT_ID );
                        }

                        leave;
                    }


                } else {    // if (IsFileLocal( dstFileObject ))

                    //
                    // The source file is local, but the destination file object
                    // is remote.  For this case query the target file's object
                    // ID and notify the link tracking system that the file has
                    // been moved across systems.
                    //

                    //
                    // Begin by ensuring that the source file has an object ID
                    // already.  If not, then just make it appear as if the
                    // operation worked.
                    //

                    status = IopGetSetObjectId( FileObject,
                                                &SourceObjectId,
                                                sizeof( SourceObjectId ),
                                                FSCTL_GET_OBJECT_ID );
                    if (!NT_SUCCESS( status )) {
                        status = STATUS_SUCCESS;
                        leave;
                    }


                    //
                    // If the extended info field is zero then this file
                    // has no interesting tracking information.
                    //
                    if (RtlCompareMemoryUlong(&SourceObjectId.BirthObjectId,
                                       sizeof(SourceObjectId.BirthObjectId),
                                       0) == sizeof(SourceObjectId.BirthObjectId)) {
                        status = STATUS_SUCCESS;
                        leave;
                    }

                    //
                    // Query the volume ID of the target.
                    //

                    status = IopGetSetObjectId( dstFileObject,
                                                &TargetVolumeId,
                                                sizeof( FILE_VOLUMEID_WITH_TYPE ),
                                                FSCTL_LMR_GET_LINK_TRACKING_INFORMATION );
                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Query the object ID of the target.
                    //

                    status = IopGetSetObjectId( dstFileObject,
                                                &TargetObjectId,
                                                sizeof( TargetObjectId ),
                                                FSCTL_CREATE_OR_GET_OBJECT_ID );
                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Notify the tracking system of the move.
                    //

                    IopMarshalIds( &trackingBuffer, &TargetVolumeId, &TargetObjectId, trackingInfo );
                    status = IopTrackLink( FileObject,
                                           IoStatusBlock,
                                           &trackingBuffer.TrackingInformation,
                                           FIELD_OFFSET( FILE_TRACKING_INFORMATION,
                                                ObjectInformation ) +
                                                    trackingBuffer.TrackingInformation.ObjectInformationLength,
                                           Event,
                                           KernelMode );
                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Delete the ID from the source
                    //

                    status = IopGetSetObjectId( FileObject,
                                                NULL,
                                                0,
                                                FSCTL_DELETE_OBJECT_ID );
                    if( !NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Set the Birth ID on the target, turning on the bit
                    // that indicates that this file has been involved in a cross-
                    // volume move.
                    //

                    CrossVolumeObjectId = SourceObjectId;
                    CrossVolumeObjectId.BirthVolumeId[0] |= 1;

                    status = IopGetSetObjectId( dstFileObject,
                                                &CrossVolumeObjectId.ExtendedInfo[0],
                                                sizeof( CrossVolumeObjectId.ExtendedInfo ),
                                                FSCTL_SET_OBJECT_ID_EXTENDED );
                    if (!NT_SUCCESS( status )) {

                        // Try to restore the source
                        IopGetSetObjectId( FileObject,
                                           &SourceObjectId,
                                           sizeof(SourceObjectId),
                                           FSCTL_SET_OBJECT_ID );
                        leave;
                    }


                }   // if (IsFileLocal( dstFileObject ))

            } else {    // if (trackingInfo->DestinationFile)

                //
                // A destination file handle was not specified.  Simply query
                // the source file's object ID and call the link tracking code.
                // Note that the function input buffer contains the volume ID
                // and file object ID of the target.  Note also that it is
                // assumed that the source file has an object ID.
                //

                status = IopGetVolumeId( FileObject,
                                         &SourceVolumeId,
                                         sizeof( SourceVolumeId ) );
                if (!NT_SUCCESS( status )) {
                    leave;
                }

                status = IopGetSetObjectId( FileObject,
                                            &SourceObjectId,
                                            sizeof( SourceObjectId ),
                                            FSCTL_GET_OBJECT_ID );
                if (!NT_SUCCESS( status )) {
                    leave;
                }

                //
                // If the extended info field is zero then this file
                // has no interesting tracking information.
                //
                if (RtlCompareMemoryUlong(SourceObjectId.BirthObjectId,
                                       sizeof(SourceObjectId.BirthObjectId),
                                       0) == sizeof(SourceObjectId.BirthObjectId)) {
                    status = STATUS_SUCCESS;
                    leave;
                }
                //
                // Inform the user-mode link tracking service that the file
                // has been moved.
                //

                NormalizedObjectId = SourceObjectId;
                NormalizedObjectId.BirthVolumeId[0] &= 0xfe;

                status = IopSendMessageToTrackService( &SourceVolumeId,
                                                       &NormalizedObjectId,
                                                       FileInformation );
                if (!NT_SUCCESS( status )) {
                    leave;
                }

            }   // if (trackingInfo->DestinationFile) ... else

        } else {    // if (IsFileLocal( FileObject ))

            //
            // The source file is remote.  For this case, remote the operation
            // to the system on which the source file is located.  Begin by
            // ensuring that the source file actually has an object ID.  If
            // not, then get out now since there is nothing to be done.
            //

            status = IopGetSetObjectId( FileObject,
                                        &SourceObjectId,
                                        sizeof( SourceObjectId ),
                                        FSCTL_GET_OBJECT_ID );

            if (status == STATUS_OBJECT_NAME_NOT_FOUND)
            {
                status = STATUS_SUCCESS;
                leave;
            }

            if (!NT_SUCCESS( status )) {
                leave;
            }

            //
            // If the extended info field is zero then this file
            // has no interesting tracking information.
            //
            if (RtlCompareMemoryUlong(SourceObjectId.BirthObjectId,
                                      sizeof(SourceObjectId.BirthObjectId),
                                      0) == sizeof(SourceObjectId.BirthObjectId)) {
                status = STATUS_SUCCESS;
                leave;
            }
            if (trackingInfo->DestinationFile) {

                //
                // A handle was specified for the destination file.  Determine
                // whether it is local or remote.  If remote and both handles
                // refer to the same machine, then ship the entire API to that
                // machine and have it perform the operation.
                //
                // Otherwise, query the target file's object ID, and then redo
                // the operation.  This will cause the API to be remoted to the
                // machine where the source file resides.
                //

                if (IsFileLocal( dstFileObject )) {

                    //
                    // The source is remote and the destination is local, so
                    // query the object ID of the target and recursively track
                    // the link from the source file's remote node.
                    //

                    status = IopGetVolumeId( dstFileObject,
                                             &TargetVolumeId,
                                             sizeof( TargetVolumeId ) );
                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    status = IopGetSetObjectId( dstFileObject,
                                                &TargetObjectId,
                                                sizeof( TargetObjectId ),
                                                FSCTL_CREATE_OR_GET_OBJECT_ID );
                    if (!NT_SUCCESS( status )) {
                        leave;
                    }


                    //
                    // Notify the tracking system of the move.
                    //

                    IopMarshalIds( &trackingBuffer, &TargetVolumeId, &TargetObjectId, trackingInfo );

                    status = IopTrackLink( FileObject,
                                           IoStatusBlock,
                                           &trackingBuffer.TrackingInformation,
                                           FIELD_OFFSET( FILE_TRACKING_INFORMATION,
                                                ObjectInformation ) +
                                                    trackingBuffer.TrackingInformation.ObjectInformationLength,
                                           Event,
                                           KernelMode );
                    if( !NT_SUCCESS(status) ) {
                        leave;
                    }

                    //
                    //  Delete the ID from the source
                    //

                    status = IopGetSetObjectId( FileObject,
                                                NULL,
                                                0,
                                                FSCTL_DELETE_OBJECT_ID );
                    if( !NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Set the birth ID on the target, also turning on the bit
                    // that indicates that this file has moved across volumes.
                    //

                    CrossVolumeObjectId = SourceObjectId;
                    CrossVolumeObjectId.BirthVolumeId[0] |= 1;

                    status = IopGetSetObjectId( dstFileObject,
                                                &CrossVolumeObjectId.ExtendedInfo[0],
                                                sizeof( CrossVolumeObjectId.ExtendedInfo ),
                                                FSCTL_SET_OBJECT_ID_EXTENDED );

                    if( !NT_SUCCESS( status )) {

                        IopGetSetObjectId( FileObject,
                                           &SourceObjectId,
                                           sizeof(SourceObjectId),
                                           FSCTL_SET_OBJECT_ID );
                        leave;
                    }

                }   // if (IsFileLocal( dstFileObject ))

                else if (!IopIsSameMachine( FileObject, trackingInfo->DestinationFile)) {

                    //
                    // The source and the target are remote from each other and from
                    // this machine.  Query the object ID of the target and recursively
                    // track the link from the source file's remote node.
                    //

                    //
                    // Query the volume ID of the target.
                    //

                    status = IopGetSetObjectId( dstFileObject,
                                                &TargetVolumeId,
                                                sizeof( FILE_VOLUMEID_WITH_TYPE ),
                                                FSCTL_LMR_GET_LINK_TRACKING_INFORMATION );

                    if (!NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Query the object ID of the target.
                    //

                    status = IopGetSetObjectId( dstFileObject,
                                                &TargetObjectId,
                                                sizeof( TargetObjectId ),
                                                FSCTL_CREATE_OR_GET_OBJECT_ID );
                    if( !NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Notify the tracking system of the move.
                    //

                    IopMarshalIds( &trackingBuffer, &TargetVolumeId, &TargetObjectId, trackingInfo );

                    status = IopTrackLink( FileObject,
                                           IoStatusBlock,
                                           &trackingBuffer.TrackingInformation,
                                           FIELD_OFFSET( FILE_TRACKING_INFORMATION,
                                                ObjectInformation ) +
                                                    trackingBuffer.TrackingInformation.ObjectInformationLength,
                                           Event,
                                           KernelMode );
                    if( !NT_SUCCESS( status )) {
                        leave;
                    }

                    //
                    // Set the birth ID on the target, turning on the bit that indicates
                    // that this file has moved across volumes.
                    //

                    CrossVolumeObjectId = SourceObjectId;
                    CrossVolumeObjectId.BirthVolumeId[0] |= 1;

                    status = IopGetSetObjectId( dstFileObject,
                                                &CrossVolumeObjectId.ExtendedInfo[0],
                                                sizeof( CrossVolumeObjectId.ExtendedInfo ),
                                                FSCTL_SET_OBJECT_ID_EXTENDED );

                    if( !NT_SUCCESS( status )) {
                        IopGetSetObjectId( FileObject,
                                           &SourceObjectId,
                                           sizeof(SourceObjectId),
                                           FSCTL_SET_OBJECT_ID );
                        leave;
                    }

                } else {    // else if (!IopIsSameMachine( FileObject, trackingInfo->DestinationFile))

                    //
                    // Both the source and the target are remote and they're
                    // both on the same remote machine.  For this case, remote
                    // the entire API using the file object pointers.
                    //

                    status = IopSetRemoteLink( FileObject, dstFileObject, trackingInfo );

                }   // else if (!IopIsSameMachine( FileObject, trackingInfo->DestinationFile)) ... else

            } else {    // if (trackingInfo->DestinationFile)

                //
                // The source file is remote and the object ID of the target is
                // contained w/in the tracking buffer.  Simply remote the API
                // to the remote machine using the source file object pointer
                // and the object ID of the target in the buffer.
                //

                status = IopSetRemoteLink( FileObject, NULL, FileInformation );

            }   // if (trackingInfo->DestinationFile) ... else
        }   // if (IsFileLocal( FileObject )) ... else

    } finally {

        //
        // Ensure that everything has been cleaned up.
        //

        if (RequestorMode != KernelMode && trackingInfo) {
            ExFreePool( trackingInfo );
        }

        if (dstFileObject ) {
            ObDereferenceObject( dstFileObject );
        }

        KeSetEvent( Event, 0, FALSE );
    }

    return status;
}

VOID
IopUserCompletion(
    IN PKAPC Apc,
    IN PKNORMAL_ROUTINE *NormalRoutine,
    IN PVOID *NormalContext,
    IN PVOID *SystemArgument1,
    IN PVOID *SystemArgument2
    )

/*++

Routine Description:

    This routine is invoked in the final processing of an IRP.  Everything has
    been completed except that the caller's APC routine must be invoked.  The
    system will do this as soon as this routine exits.  The only processing
    remaining to be completed by the I/O system is to free the I/O Request
    Packet itself.

Arguments:

    Apc - Supplies a pointer to kernel APC structure.

    NormalRoutine - Supplies a pointer to a pointer to the normal function
        that was specified when the APC was initialized.

    NormalContext - Supplies a pointer to a pointer to an arbitrary data
        structure that was specified when the APC was initialized.

    SystemArgument1, SystemArgument2 - Supplies a set of two pointers to
        two arguments that contain untyped data.

Return Value:

    None.

Note:

    If no other processing is ever needed, and the APC can be placed at the
    beginning of the IRP, then this routine could be replaced by simply
    specifying the address of the pool deallocation routine in the APC instead
    of the address of this routine.

Caution:

    This routine is also invoked as a general purpose rundown routine for APCs.
    Should this code ever need to directly access any of the other parameters
    other than Apc, this routine will need to be split into two separate
    routines.  The rundown routine should perform exactly the following code's
    functionality.

--*/

{
    UNREFERENCED_PARAMETER( NormalRoutine );
    UNREFERENCED_PARAMETER( NormalContext );
    UNREFERENCED_PARAMETER( SystemArgument1 );
    UNREFERENCED_PARAMETER( SystemArgument2 );

    PAGED_CODE();

    //
    // Free the packet.
    //

    IoFreeIrp( CONTAINING_RECORD( Apc, IRP, Tail.Apc ) );
}



VOID
IopUserRundown(
    IN PKAPC Apc
    )

/*++

Routine Description:

    This routine is invoked during thread termination as the rundown routine
    for it simply calls IopUserCompletion.

Arguments:

    Apc - Supplies a pointer to kernel APC structure.

Return Value:

    None.


--*/

{
    PAGED_CODE();

    //
    // Free the packet.
    //

    IoFreeIrp( CONTAINING_RECORD( Apc, IRP, Tail.Apc ) );
}

NTSTATUS
IopXxxControlFile(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    IN ULONG IoControlCode,
    IN PVOID InputBuffer OPTIONAL,
    IN ULONG InputBufferLength,
    OUT PVOID OutputBuffer OPTIONAL,
    IN ULONG OutputBufferLength,
    IN BOOLEAN DeviceIoControl
    )

/*++

Routine Description:

    This service builds descriptors or MDLs for the supplied buffer(s) and
    passes the untyped data to the driver associated with the file handle.
    handle.  It is up to the driver to check the input data and function
    IoControlCode for validity, as well as to make the appropriate access
    checks.

Arguments:

    FileHandle - Supplies a handle to the file on which the service is being
        performed.

    Event - Supplies an optional event to be set to the Signaled state when
        the service is complete.

    ApcRoutine - Supplies an optional APC routine to be executed when the
        service is complete.

    ApcContext - Supplies a context parameter to be passed to the ApcRoutine,
        if an ApcRoutine was specified.

    IoStatusBlock - Address of the caller's I/O status block.

    IoControlCode - Subfunction code to determine exactly what operation is
        being performed.

    InputBuffer - Optionally supplies an input buffer to be passed to the
        driver.  Whether or not the buffer is actually optional is dependent
        on the IoControlCode.

    InputBufferLength - Length of the InputBuffer in bytes.

    OutputBuffer - Optionally supplies an output buffer to receive information
        from the driver.  Whether or not the buffer is actually optional is
        dependent on the IoControlCode.

    OutputBufferLength - Length of the OutputBuffer in bytes.

    DeviceIoControl - Determines whether this is a Device or File System
        Control function.

Return Value:

    The status returned is success if the control operation was properly
    queued to the I/O system.   Once the operation completes, the status
    can be determined by examining the Status field of the I/O status block.

--*/

{
    PIRP irp;
    NTSTATUS status;
    PFILE_OBJECT fileObject;
    PDEVICE_OBJECT deviceObject;
    PKEVENT eventObject = (PKEVENT) NULL;
    PIO_STACK_LOCATION irpSp;
    ULONG method;
    OBJECT_HANDLE_INFORMATION handleInformation;
    BOOLEAN synchronousIo;
    IO_STATUS_BLOCK localIoStatus;
    PFAST_IO_DISPATCH fastIoDispatch;
    POOL_TYPE poolType;
    PULONG majorFunction;
    KPROCESSOR_MODE requestorMode;
    PETHREAD CurrentThread;

    PAGED_CODE();

    //
    // Get the method that the buffers are being passed by.
    //

    method = IoControlCode & 3;

    //
    // Check the caller's parameters based on the mode of the caller.
    //

    CurrentThread = PsGetCurrentThread ();
    requestorMode = KeGetPreviousModeByThread(&CurrentThread->Tcb);

    if (requestorMode != KernelMode) {

        //
        // The caller's access mode is not kernel so probe each of the arguments
        // and capture them as necessary.  If any failures occur, the condition
        // handler will be invoked to handle them.  It will simply cleanup and
        // return an access violation status code back to the system service
        // dispatcher.
        //

        try {

            //
            // The IoStatusBlock parameter must be writeable by the caller.
            //

            ProbeForWriteIoStatus (IoStatusBlock);

            //
            // The output buffer can be used in any one of the following three ways,
            // if it is specified:
            //
            //     0) It can be a normal, buffered output buffer.
            //
            //     1) It can be a DMA input buffer.
            //
            //     2) It can be a DMA output buffer.
            //
            // Which way the buffer is to be used it based on the low-order two bits
            // of the IoControlCode.
            //
            // If the method is 0 we probe the output buffer for write access.
            // If the method is not 3 we probe the input buffer for read access.
            //

            if (method == METHOD_BUFFERED) {
                if (ARGUMENT_PRESENT( OutputBuffer )) {
                    ProbeForWrite( OutputBuffer,
                                   OutputBufferLength,
                                   sizeof( UCHAR ) );
                } else {
                    OutputBufferLength = 0;
                }
            }

            if (method != METHOD_NEITHER) {
                if (ARGUMENT_PRESENT( InputBuffer )) {
                    ProbeForRead( InputBuffer,
                                  InputBufferLength,
                                  sizeof( UCHAR ) );
                } else {
                    InputBufferLength = 0;
                }
            }

        } except(EXCEPTION_EXECUTE_HANDLER) {

            //
            // An exception was incurred while attempting to probe or write
            // one of the caller's parameters.  Simply return an appropriate
            // error status code.
            //

            return GetExceptionCode();

        }
    }

    //
    // There were no blatant errors so far, so reference the file object so
    // the target device object can be found.  Note that if the handle does
    // not refer to a file object, or if the caller does not have the required
    // access to the file, then it will fail.
    //

    status = ObReferenceObjectByHandle( FileHandle,
                                        0L,
                                        IoFileObjectType,
                                        requestorMode,
                                        (PVOID *) &fileObject,
                                        &handleInformation );
    if (!NT_SUCCESS( status )) {
        return status;
    }

    //
    // If this file has an I/O completion port associated w/it, then ensure
    // that the caller did not supply an APC routine, as the two are mutually
    // exclusive methods for I/O completion notification.
    //

    if (fileObject->CompletionContext && IopApcRoutinePresent( ApcRoutine )) {
        ObDereferenceObject( fileObject );
        return STATUS_INVALID_PARAMETER;
    }

    //
    // Now check the access type for this control code to ensure that the
    // caller has the appropriate access to this file object to perform the
    // operation.
    //

    if (requestorMode != KernelMode) {

        ULONG accessMode = (IoControlCode >> 14) & 3;

        if (accessMode != FILE_ANY_ACCESS) {

            //
            // This I/O control requires that the caller have read, write,
            // or read/write access to the object.  If this is not the case,
            // then cleanup and return an appropriate error status code.
            //

            if (SeComputeGrantedAccesses( handleInformation.GrantedAccess, accessMode ) != accessMode ) {
                ObDereferenceObject( fileObject );
                return STATUS_ACCESS_DENIED;
            }
        }
    }

    //
    // Get the address of the event object and set the event to the Not-
    // Signaled state, if an event was specified.  Note here, too, that if
    // the handle does not refer to an event, or if the event cannot be
    // written, then the reference will fail.
    //

    if (ARGUMENT_PRESENT( Event )) {
        status = ObReferenceObjectByHandle( Event,
                                            EVENT_MODIFY_STATE,
                                            ExEventObjectType,
                                            requestorMode,
                                            (PVOID *) &eventObject,
                                            NULL );
        if (!NT_SUCCESS( status )) {
            ObDereferenceObject( fileObject );
            return status;
        } else {
            KeClearEvent( eventObject );
        }
    }

    //
    // Make a special check here to determine whether this is a synchronous
    // I/O operation.  If it is, then wait here until the file is owned by
    // the current thread.
    //

    if (fileObject->Flags & FO_SYNCHRONOUS_IO) {
        BOOLEAN interrupted;

        if (!IopAcquireFastLock( fileObject )) {
            status = IopAcquireFileObjectLock( fileObject,
                                               requestorMode,
                                               (BOOLEAN) ((fileObject->Flags & FO_ALERTABLE_IO) != 0),
                                               &interrupted );
            if (interrupted) {
                if (eventObject) {
                    ObDereferenceObject( eventObject );
                }
                ObDereferenceObject( fileObject );
                return status;
            }
        }
        synchronousIo = TRUE;
    } else {
        synchronousIo = FALSE;

#if defined(_WIN64)
        if (requestorMode != KernelMode) {
            try {

                //
                // If this is a 32-bit asynchronous IO, then mark the Iosb being sent as so.
                // Note: IopMarkApcRoutineIfAsyncronousIo32 must be called after probing
                //       the IoStatusBlock structure for write.
                //

                IopMarkApcRoutineIfAsyncronousIo32(IoStatusBlock,ApcRoutine,FALSE);

            } except (EXCEPTION_EXECUTE_HANDLER) {

                //
                // Cleanup and return an appropriate status code.
                //

                if (eventObject) {
                    ObDereferenceObject( eventObject );
                }
                ObDereferenceObject( fileObject );
                return GetExceptionCode ();
            }
        }
#endif
    }

    //
    // Get the address of the target device object.  If this file represents
    // a device that was opened directly, then simply use the device or its
    // attached device(s) directly.
    //

    if (!(fileObject->Flags & FO_DIRECT_DEVICE_OPEN)) {
        deviceObject = IoGetRelatedDeviceObject( fileObject );
    } else {
        deviceObject = IoGetAttachedDevice( fileObject->DeviceObject );
    }

    if (DeviceIoControl) {

        //
        // Also get the address of the Fast I/O dispatch structure.
        //

        fastIoDispatch = deviceObject->DriverObject->FastIoDispatch;

        //
        // Turbo device control support.  If the device has a fast I/O entry
        // point for DeviceIoControlFile, call the entry point and give it a
        // chance to try to complete the request.  Note if FastIoDeviceControl
        // returns FALSE or we get an I/O error, we simply fall through and
        // go the "long way" and create an Irp.
        //

        if (fastIoDispatch && fastIoDispatch->FastIoDeviceControl) {

            //
            // Before we actually call the fast I/O routine in the driver,
            // we must probe OutputBuffer if the method is METHOD_IN_DIRECT or METHOD_OUT_DIRECT.
            //

            if (requestorMode != KernelMode && ARGUMENT_PRESENT(OutputBuffer)) {

                try {

                    if (method == METHOD_IN_DIRECT) {
                        ProbeForRead( OutputBuffer,
                                      OutputBufferLength,
                                      sizeof( UCHAR ) );
                    } else if (method == METHOD_OUT_DIRECT) {
                        ProbeForWrite( OutputBuffer,
                                       OutputBufferLength,
                                       sizeof( UCHAR ) );
                    }

                } except(EXCEPTION_EXECUTE_HANDLER) {

                    //
                    // An exception was incurred while attempting to probe
                    // the output buffer.  Clean up and return an
                    // appropriate error status code.
                    //

                    if (synchronousIo) {
                        IopReleaseFileObjectLock( fileObject );
                    }

                    if (eventObject) {
                        ObDereferenceObject( eventObject );
                    }

                    ObDereferenceObject( fileObject );

                    return GetExceptionCode();
                }
            }

            //
            // If we are dismounting a volume, increment the shared count.  This
            // allows user-space applications to efficiently test for validity
            // of current directory handles.
            //

            if (IoControlCode == FSCTL_DISMOUNT_VOLUME) {
                InterlockedIncrement( (PLONG) &SharedUserData->DismountCount );
            }


            //
            // Call the driver's fast I/O routine.
            //

            if (fastIoDispatch->FastIoDeviceControl( fileObject,
                                                     TRUE,
                                                     InputBuffer,
                                                     InputBufferLength,
                                                     OutputBuffer,
                                                     OutputBufferLength,
                                                     IoControlCode,
                                                     &localIoStatus,
                                                     deviceObject )) {

                PVOID port;
                PVOID key;

                //
                // The driver successfully performed the I/O in it's
                // fast device control routine.  Carefully return the
                // I/O status.
                //

                try {
#if defined(_WIN64)
                    //
                    // If this is a32-bit thread, and the IO request is
                    // asynchronous, then the IOSB is 32-bit. Wow64 always sends
                    // the 32-bit IOSB when the I/O is asynchronous.
                    //
                    if (IopIsIosb32(ApcRoutine)) {
                        PIO_STATUS_BLOCK32 UserIosb32 = (PIO_STATUS_BLOCK32)IoStatusBlock;

                        UserIosb32->Information = (ULONG)localIoStatus.Information;
                        UserIosb32->Status = (NTSTATUS)localIoStatus.Status;
                    } else {
                        *IoStatusBlock = localIoStatus;
                    }
#else
                    *IoStatusBlock = localIoStatus;
#endif
                } except( EXCEPTION_EXECUTE_HANDLER ) {
                    localIoStatus.Status = GetExceptionCode();
                    localIoStatus.Information = 0;
                }


                //
                // If there is an I/O completion port object associated w/this request,
                // save it here. We cannot look at the fileobject after signaling the
                // event as that might result in the attachment of a completion port.
                // This makes the behavior consistent with the one in IopCompleteRequest.
                //

                if (fileObject->CompletionContext) {
                    port = fileObject->CompletionContext->Port;
                    key = fileObject->CompletionContext->Key;
                } else {
                    port = NULL;
                    key = NULL;
                }

                //
                // If an event was specified, set it.
                //

                if (ARGUMENT_PRESENT( Event )) {
                    KeSetEvent( eventObject, 0, FALSE );
                    ObDereferenceObject( eventObject );
                }

                //
                // Note that the file object event need not be set to the
                // Signaled state, as it is already set.  Release the
                // file object lock, if necessary.
                //

                if (synchronousIo) {
                    IopReleaseFileObjectLock( fileObject );
                }

                //
                // If this file object has a completion port associated with it
                // and this request has a non-NULL APC context then a completion
                // message needs to be queued.
                //

                if (port && ARGUMENT_PRESENT( ApcContext )) {
                    if (!NT_SUCCESS(IoSetIoCompletion( port,
                                                       key,
                                                       ApcContext,
                                                       localIoStatus.Status,
                                                       localIoStatus.Information,
                                                       TRUE ))) {
                        localIoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                    }
                }

                //
                // Cleanup and return.
                //

                ObDereferenceObject( fileObject );
                return localIoStatus.Status;
            }
        }

    }

    //
    // Set the file object to the Not-Signaled state.
    //

    KeClearEvent( &fileObject->Event );

    //
    // Allocate and initialize the I/O Request Packet (IRP) for this operation.

    irp = IopAllocateIrp( deviceObject->StackSize, !synchronousIo );

    if (!irp) {

        //
        // An IRP could not be allocated.  Cleanup and return an appropriate
        // error status code.
        //

        IopAllocateIrpCleanup( fileObject, eventObject );

        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->Tail.Overlay.OriginalFileObject = fileObject;
    irp->Tail.Overlay.Thread = CurrentThread;
    irp->Tail.Overlay.AuxiliaryBuffer = (PVOID) NULL;
    irp->RequestorMode = requestorMode;
    irp->PendingReturned = FALSE;
    irp->Cancel = FALSE;
    irp->CancelRoutine = (PDRIVER_CANCEL) NULL;

    //
    // Fill in the service independent parameters in the IRP.
    //

    irp->UserEvent = eventObject;
    irp->UserIosb = IoStatusBlock;
    irp->Overlay.AsynchronousParameters.UserApcRoutine = ApcRoutine;
    irp->Overlay.AsynchronousParameters.UserApcContext = ApcContext;

    //
    // Get a pointer to the stack location for the first driver.  This will be
    // used to pass the original function codes and parameters.  Note that
    // setting the major function here also sets:
    //
    //      MinorFunction = 0;
    //      Flags = 0;
    //      Control = 0;
    //

    irpSp = IoGetNextIrpStackLocation( irp );
    majorFunction = (PULONG) (&irpSp->MajorFunction);
    *majorFunction = DeviceIoControl ? IRP_MJ_DEVICE_CONTROL : IRP_MJ_FILE_SYSTEM_CONTROL;
    irpSp->FileObject = fileObject;

    //
    // Copy the caller's parameters to the service-specific portion of the
    // IRP for those parameters that are the same for all three methods.
    //

    irpSp->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;
    irpSp->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
    irpSp->Parameters.DeviceIoControl.IoControlCode = IoControlCode;

    //
    // Set the pool type based on the type of function being performed.
    //

    poolType = DeviceIoControl ? NonPagedPoolCacheAligned : NonPagedPool;

    //
    // Based on the method that the buffer are being passed, either allocate
    // buffers or build MDLs.  Note that in some cases no probing has taken
    // place so the exception handler must catch access violations.
    //

    irp->MdlAddress = (PMDL) NULL;
    irp->AssociatedIrp.SystemBuffer = (PVOID) NULL;

    switch ( method ) {

    case METHOD_BUFFERED:

        //
        // For this case, allocate a buffer that is large enough to contain
        // both the input and the output buffers.  Copy the input buffer to
        // the allocated buffer and set the appropriate IRP fields.
        //

        irpSp->Parameters.DeviceIoControl.Type3InputBuffer = (PVOID) NULL;

        try {

            if (InputBufferLength || OutputBufferLength) {
                irp->AssociatedIrp.SystemBuffer =
                    ExAllocatePoolWithQuota( poolType,
                                             (InputBufferLength > OutputBufferLength) ? InputBufferLength : OutputBufferLength );

                if (ARGUMENT_PRESENT( InputBuffer )) {
                    RtlCopyMemory( irp->AssociatedIrp.SystemBuffer,
                                   InputBuffer,
                                   InputBufferLength );
                }
                irp->Flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
                irp->UserBuffer = OutputBuffer;
                if (ARGUMENT_PRESENT( OutputBuffer )) {
                    irp->Flags |= IRP_INPUT_OPERATION;
                }
            } else {
                irp->Flags = 0;
                irp->UserBuffer = (PVOID) NULL;
            }

        } except(EXCEPTION_EXECUTE_HANDLER) {

            //
            // An exception was incurred while either allocating the
            // the system buffer or moving the caller's data.  Determine
            // what actually happened, cleanup accordingly, and return
            // an appropriate error status code.
            //

            IopExceptionCleanup( fileObject,
                                 irp,
                                 eventObject,
                                 (PKEVENT) NULL );

            return GetExceptionCode();
        }

        break;

    case METHOD_IN_DIRECT:
    case METHOD_OUT_DIRECT:

        //
        // For these two cases, allocate a buffer that is large enough to
        // contain the input buffer, if any, and copy the information to
        // the allocated buffer.  Then build an MDL for either read or write
        // access, depending on the method, for the output buffer.  Note
        // that the buffer length parameters have been jammed to zero for
        // users if the buffer parameter was not passed.  (Kernel callers
        // should be calling the service correctly in the first place.)
        //
        // Note also that it doesn't make a whole lot of sense to specify
        // either method #1 or #2 if the IOCTL does not require the caller
        // to specify an output buffer.
        //

        irp->Flags = 0;
        irpSp->Parameters.DeviceIoControl.Type3InputBuffer = (PVOID) NULL;

        try {

            if (InputBufferLength && ARGUMENT_PRESENT( InputBuffer )) {
                irp->AssociatedIrp.SystemBuffer =
                    ExAllocatePoolWithQuota( poolType,
                                             InputBufferLength );
                RtlCopyMemory( irp->AssociatedIrp.SystemBuffer,
                               InputBuffer,
                               InputBufferLength );
                irp->Flags = IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER;
            }

            if (OutputBufferLength != 0) {
                irp->MdlAddress = IoAllocateMdl( OutputBuffer,
                                                 OutputBufferLength,
                                                 FALSE,
                                                 TRUE,
                                                 irp  );
                if (irp->MdlAddress == NULL) {
                    ExRaiseStatus( STATUS_INSUFFICIENT_RESOURCES );
                }
                IopProbeAndLockPages( irp->MdlAddress,
                                     requestorMode,
                                     (LOCK_OPERATION) ((method == 1) ? IoReadAccess : IoWriteAccess),
                                       deviceObject,
                                      *majorFunction);
            }

        } except(EXCEPTION_EXECUTE_HANDLER) {

            //
            // An exception was incurred while either allocating the
            // system buffer, copying the caller's data, allocating the
            // MDL, or probing and locking the caller's buffer. Determine
            // what actually happened, cleanup accordingly, and return
            // an appropriate error status code.
            //

            IopExceptionCleanup( fileObject,
                                 irp,
                                 eventObject,
                                 (PKEVENT) NULL );

            return GetExceptionCode();
        }

        break;

    case METHOD_NEITHER:

        //
        // For this case, do nothing.  Everything is up to the driver.
        // Simply give the driver a copy of the caller's parameters and
        // let the driver do everything itself.
        //

        irp->Flags = 0;
        irp->UserBuffer = OutputBuffer;
        irpSp->Parameters.DeviceIoControl.Type3InputBuffer = InputBuffer;
    }

    //
    // Pass the read/write access granted bits down to the driver. This allows drivers to check
    // access for ioctls that were mistakenly defined as FILE_ANY_ACCESS and cannot be redefined for
    // compatibility reasons.
    //

    irpSp->Flags |= SeComputeGrantedAccesses( handleInformation.GrantedAccess, FILE_READ_DATA ) ? SL_READ_ACCESS_GRANTED : 0;
    irpSp->Flags |= SeComputeGrantedAccesses( handleInformation.GrantedAccess, FILE_WRITE_DATA ) ? SL_WRITE_ACCESS_GRANTED : 0;

    //
    // Defer I/O completion for FSCTL requests, but not for IOCTL requests,
    // since file systems set pending properly but device driver do not.
    //

    if (!DeviceIoControl) {
        irp->Flags |= IRP_DEFER_IO_COMPLETION;
    }

    //
    // If we are dismounting a volume, increment the shared count.  This
    // allows user-space applications to efficiently test for validity
    // of current directory handles.
    //

    if (IoControlCode == FSCTL_DISMOUNT_VOLUME) {
        InterlockedIncrement( (PLONG) &SharedUserData->DismountCount );
    }


    //
    // Queue the packet, call the driver, and synchronize appropriately with
    // I/O completion.
    //

    return IopSynchronousServiceTail( deviceObject,
                                      irp,
                                      fileObject,
                                      (BOOLEAN)!DeviceIoControl,
                                      requestorMode,
                                      synchronousIo,
                                      OtherTransfer );
}

NTSTATUS
IopLookupBusStringFromID (
    IN  HANDLE KeyHandle,
    IN  INTERFACE_TYPE InterfaceType,
    OUT PWCHAR Buffer,
    IN  ULONG Length,
    OUT PULONG BusFlags OPTIONAL
    )
/*++

Routine Description:

    Translates INTERFACE_TYPE to its corresponding WCHAR[] string.

Arguments:

    KeyHandle - Supplies a handle to the opened registry key,
        HKLM\System\CurrentControlSet\Control\SystemResources\BusValues.

    InterfaceType - Supplies the interface type for which a descriptive
        name is to be retrieved.

    Buffer - Supplies a pointer to a unicode character buffer that will
        receive the bus name.  Since this buffer is used in an
        intermediate step to retrieve a KEY_VALUE_FULL_INFORMATION structure,
        it must be large enough to contain this structure (including the
        longest value name & data length under KeyHandle).

    Length - Supplies the length, in bytes, of the Buffer.

    BusFlags - Optionally receives the flags specified in the second
        DWORD of the matching REG_BINARY value.

Return Value:

    The function value is the final status of the operation.

--*/
{
    NTSTATUS                        status;
    ULONG                           Index, junk, i, j;
    PULONG                          pl;
    PKEY_VALUE_FULL_INFORMATION     KeyInformation;
    WCHAR                           c;

    PAGED_CODE();

    Index = 0;
    KeyInformation = (PKEY_VALUE_FULL_INFORMATION) Buffer;

    for (; ;) {
        status = ZwEnumerateValueKey (
                        KeyHandle,
                        Index++,
                        KeyValueFullInformation,
                        Buffer,
                        Length,
                        &junk
                        );

        if (!NT_SUCCESS (status)) {
            return status;
        }

        if (KeyInformation->Type != REG_BINARY) {
            continue;
        }

        pl = (PULONG) ((PUCHAR) KeyInformation + KeyInformation->DataOffset);
        if ((ULONG) InterfaceType != pl[0]) {
            continue;
        }

        //
        // Found a match - move the name to the start of the buffer
        //

        if(ARGUMENT_PRESENT(BusFlags)) {
            *BusFlags = pl[1];
        }

        j = KeyInformation->NameLength / sizeof (WCHAR);
        for (i=0; i < j; i++) {
            c = KeyInformation->Name[i];
            Buffer[i] = c;
        }

        Buffer[i] = 0;
        return STATUS_SUCCESS;
    }
}


BOOLEAN
IopSafebootDriverLoad(
    PUNICODE_STRING DriverId
    )
/*++

Routine Description:

    Checks to see if a driver or service is included
    in the current safeboot registry section.

Arguments:

    DriverId - Specifies which driver is to be validated.
        The string should contain a driver executable name
        like foo.sys or a GUID for a pnp driver class.

Return Value:

    TRUE    - driver/service is in the registry
    FALSE   - driver/service is NOT in the registry

--*/
{
    NTSTATUS status;
    HANDLE hSafeBoot,hGuid;
    UNICODE_STRING safeBootKey;
    UNICODE_STRING SafeBootTypeString;



    //
    // set the first part of the registry key name
    //

    switch (InitSafeBootMode) {
        case SAFEBOOT_MINIMAL:
            RtlInitUnicodeString(&SafeBootTypeString,SAFEBOOT_MINIMAL_STR_W);
            break;

        case SAFEBOOT_NETWORK:
            RtlInitUnicodeString(&SafeBootTypeString,SAFEBOOT_NETWORK_STR_W);
            break;

        case SAFEBOOT_DSREPAIR:
            return TRUE;

        default:
            KdPrint(("SAFEBOOT: invalid safeboot option = %d\n",InitSafeBootMode));
            return FALSE;
    }

    safeBootKey.Length = 0;
    safeBootKey.MaximumLength = DriverId->Length + SafeBootTypeString.Length + (4*sizeof(WCHAR));
    safeBootKey.Buffer = (PWCHAR)ExAllocatePool(PagedPool,safeBootKey.MaximumLength);
    if (!safeBootKey.Buffer) {
        KdPrint(("SAFEBOOT: could not allocate pool\n"));
        return FALSE;
    }

    RtlCopyUnicodeString(&safeBootKey,&SafeBootTypeString);
    status = RtlAppendUnicodeToString(&safeBootKey,L"\\");
    if (!NT_SUCCESS(status)) {
        ExFreePool (safeBootKey.Buffer);
        KdPrint(("SAFEBOOT: could not create registry key string = %x\n",status));
        return FALSE;
    }
    status = RtlAppendUnicodeStringToString(&safeBootKey,DriverId);
    if (!NT_SUCCESS(status)) {
        ExFreePool (safeBootKey.Buffer);
        KdPrint(("SAFEBOOT: could not create registry key string = %x\n",status));
        return FALSE;
    }

    status = IopOpenRegistryKey (
        &hSafeBoot,
        NULL,
        &CmRegistryMachineSystemCurrentControlSetControlSafeBoot,
        KEY_ALL_ACCESS,
        FALSE
        );
    if (NT_SUCCESS(status)) {
        status = IopOpenRegistryKey (
            &hGuid,
            hSafeBoot,
            &safeBootKey,
            KEY_ALL_ACCESS,
            FALSE
            );
        ObCloseHandle(hSafeBoot, KernelMode);
        if (NT_SUCCESS(status)) {
            ObCloseHandle(hGuid, KernelMode);
            ExFreePool(safeBootKey.Buffer);
            return TRUE;
        }
    }

    ExFreePool(safeBootKey.Buffer);

    return FALSE;
}



#ifdef ALLOC_DATA_PRAGMA
#pragma  data_seg("PAGEDATA")
#endif
static PBOOT_LOG_RECORD BootLogRecord;
#ifdef ALLOC_DATA_PRAGMA
#pragma  data_seg()
#endif

VOID
IopInitializeBootLogging(
    PLOADER_PARAMETER_BLOCK LoaderBlock,
    PCHAR HeaderString
    )
/*++

Routine Description:

    Initializes strings for boot logging.

Arguments:

    LoaderBlock - the loader parameter block

Return Value:

    VOID

--*/
{
    PKLDR_DATA_TABLE_ENTRY DataTableEntry;
    PMESSAGE_RESOURCE_ENTRY MessageEntry;
    ANSI_STRING AnsiString;
    NTSTATUS Status;
    PLIST_ENTRY nextEntry;
    PKLDR_DATA_TABLE_ENTRY driverEntry;


    PAGED_CODE();

    if (BootLogRecord != NULL) {
        return;
    }

    BootLogRecord = (PBOOT_LOG_RECORD) ExAllocatePool(NonPagedPool, sizeof(BOOT_LOG_RECORD));

    if (BootLogRecord == NULL) {
        return;
    }

    RtlZeroMemory(BootLogRecord, sizeof(BOOT_LOG_RECORD));

    ExInitializeResourceLite(&BootLogRecord->Resource);

    //
    // No need to do KeEnterCriticalRegion as this is called
    // from system process only.
    //
    ExAcquireResourceExclusiveLite(&BootLogRecord->Resource, TRUE);

    DataTableEntry = CONTAINING_RECORD(LoaderBlock->LoadOrderListHead.Flink,
                                        KLDR_DATA_TABLE_ENTRY,
                                        InLoadOrderLinks);

    Status = RtlFindMessage (DataTableEntry->DllBase, 11, 0, BOOTLOG_LOADED, &MessageEntry);

    if (NT_SUCCESS( Status )) {
        AnsiString.Buffer = (PCHAR) MessageEntry->Text;
        AnsiString.Length = (USHORT)strlen((const char *)MessageEntry->Text);
        AnsiString.MaximumLength = AnsiString.Length + 1;

        RtlAnsiStringToUnicodeString(&BootLogRecord->LoadedString, &AnsiString, TRUE);

        // whack the crlf at the end of the string

        if (BootLogRecord->LoadedString.Length > 2 * sizeof(WCHAR)) {
            BootLogRecord->LoadedString.Length -= 2 * sizeof(WCHAR);
            BootLogRecord->LoadedString.Buffer[BootLogRecord->LoadedString.Length / sizeof(WCHAR)] = UNICODE_NULL;
        }
    }

    Status = RtlFindMessage (DataTableEntry->DllBase, 11, 0, BOOTLOG_NOT_LOADED, &MessageEntry);

    if (NT_SUCCESS( Status )) {
        AnsiString.Buffer = (PCHAR) MessageEntry->Text;
        AnsiString.Length = (USHORT)strlen((const char *)MessageEntry->Text);
        AnsiString.MaximumLength = AnsiString.Length + 1;

        RtlAnsiStringToUnicodeString(&BootLogRecord->NotLoadedString, &AnsiString, TRUE);

        // whack the crlf at the end of the string

        if (BootLogRecord->NotLoadedString.Length > 2 * sizeof(WCHAR)) {
            BootLogRecord->NotLoadedString.Length -= 2 * sizeof(WCHAR);
            BootLogRecord->NotLoadedString.Buffer[BootLogRecord->NotLoadedString.Length / sizeof(WCHAR)] = UNICODE_NULL;
        }
    }

    // The header string (copied from DebugString in Phase1Initialization) appears to have a leading null byte

    HeaderString++;

    RtlCreateUnicodeStringFromAsciiz(&BootLogRecord->HeaderString, HeaderString);

    // Log the drivers loaded by the boot loader

    ExAcquireResourceSharedLite( &PsLoadedModuleResource, TRUE );
    nextEntry = PsLoadedModuleList.Flink;
    while (nextEntry != &PsLoadedModuleList) {

        //
        // Look at the next boot driver in the list.
        //

        driverEntry = CONTAINING_RECORD( nextEntry,
                                         KLDR_DATA_TABLE_ENTRY,
                                         InLoadOrderLinks );

        IopBootLog(&driverEntry->FullDllName, TRUE);

        nextEntry = nextEntry->Flink;
    }

    ExReleaseResourceLite( &PsLoadedModuleResource );

    ExReleaseResourceLite(&BootLogRecord->Resource);
}

VOID
IopBootLog(
    PUNICODE_STRING LogEntry,
    BOOLEAN Loaded
    )
/*++

Routine Description:

    Create and write out a log entry.  Before NtInitializeRegistry is called, log entries are spooled
    into the registry.  When NtInitalizeRegistry is called by the session manager, the
    log file is created if necessary and truncated.  Log entries in the registry are
    then copied into the log file and the registry entries are deleted.

Arguments:

    LogEntry - the text to log.
    Loaded - indicates whether to prepend the "Loaded" string or the "Not Loaded" string.

Return Value:

    VOID


--*/
{
    WCHAR NameBuffer[BOOTLOG_STRSIZE];
    UNICODE_STRING KeyName;
    UNICODE_STRING ValueName;
    UNICODE_STRING CrLf;
    UNICODE_STRING Space;
    NTSTATUS Status;

    WCHAR MessageBuffer[BOOTLOG_STRSIZE];
    UNICODE_STRING MessageString = {
        0,
        BOOTLOG_STRSIZE,
        &MessageBuffer[0]
    };

    PAGED_CODE();

    if (BootLogRecord == NULL) {
        return;
    }

    //
    // No need to do KeEnterCriticalRegion as this is called
    // from system process only.
    //
    ExAcquireResourceExclusiveLite(&BootLogRecord->Resource, TRUE);

    if (Loaded) {
        RtlCopyUnicodeString(&MessageString, &BootLogRecord->LoadedString);
    } else {
        RtlCopyUnicodeString(&MessageString, &BootLogRecord->NotLoadedString);
    }

    // add a space after the message prefix

    RtlInitUnicodeString(&Space, L" ");

    RtlAppendUnicodeStringToString(&MessageString, &Space);

    RtlAppendUnicodeStringToString(&MessageString, LogEntry);

    // add a CR LF

    RtlInitUnicodeString(&CrLf, L"\r\n");
    RtlAppendUnicodeStringToString(&MessageString, &CrLf);

    swprintf(NameBuffer, L"%d", BootLogRecord->NextKey++);

    RtlCreateUnicodeString(&KeyName, NameBuffer);
    RtlInitUnicodeString(&ValueName, L"");

    if (!BootLogRecord->FileLogging) {
        HANDLE hLogKey, hBootKey;

        Status = IopOpenRegistryKey (
            &hBootKey,
            NULL,
            &CmRegistryMachineSystemCurrentControlSetControlBootLog,
            KEY_ALL_ACCESS,
            TRUE
            );

        if (NT_SUCCESS(Status)) {
            Status = IopOpenRegistryKey (
                &hLogKey,
                hBootKey,
                &KeyName,
                KEY_ALL_ACCESS,
                TRUE
                );
            if (NT_SUCCESS(Status)) {
                Status = IopSetRegistryStringValue(
                    hLogKey,
                    &ValueName,
                    &MessageString
                    );
                ZwClose(hLogKey);
            }
            ZwClose(hBootKey);
        }

    } else {
        IopBootLogToFile( &MessageString );
    }

    RtlFreeUnicodeString(&KeyName);

    ExReleaseResourceLite(&BootLogRecord->Resource);
}

VOID
IopCopyBootLogRegistryToFile(
    VOID
    )
/*++

Routine Description:

    Copy the text in the registry entries into the log file and delete the registry entries.  Set the
    flag that indicates direct logging to the log file.

Arguments:

    NONE

Return Value:

    VOID


--*/
{
    UNICODE_STRING KeyName;
    WCHAR NameBuffer[BOOTLOG_STRSIZE];
    NTSTATUS Status;
    HANDLE hLogKey, hBootKey;
    ULONG Index;
    PKEY_VALUE_FULL_INFORMATION Information;
    LARGE_INTEGER LocalTime;
    TIME_FIELDS TimeFields;
    CHAR AnsiTimeBuffer[256];
    ANSI_STRING AnsiTimeString;
    UNICODE_STRING UnicodeTimeString;
    UNICODE_STRING LogString;
    PKTHREAD CurrentThread;

    PAGED_CODE();

    if (BootLogRecord == NULL) {
        return;
    }

    CurrentThread = KeGetCurrentThread ();
    KeEnterCriticalRegionThread(CurrentThread);
    ExAcquireResourceExclusiveLite(&BootLogRecord->Resource, TRUE);

    IopBootLogToFile(&BootLogRecord->HeaderString);

    ExSystemTimeToLocalTime(&KeBootTime, &LocalTime);

    RtlTimeToTimeFields(&LocalTime, &TimeFields);

    sprintf(
        AnsiTimeBuffer,
        "%2d %2d %4d %02d:%02d:%02d.%03d\r\n",
        TimeFields.Month,
        TimeFields.Day,
        TimeFields.Year,
        TimeFields.Hour,
        TimeFields.Minute,
        TimeFields.Second,
        TimeFields.Milliseconds
    );

    RtlInitAnsiString(&AnsiTimeString, AnsiTimeBuffer);

    RtlAnsiStringToUnicodeString(&UnicodeTimeString, &AnsiTimeString, TRUE);

    IopBootLogToFile(&UnicodeTimeString);

    RtlFreeUnicodeString(&UnicodeTimeString);

    //
    // Read all of the strings in the registry and write them to the log file.
    // Delete the registry keys when done.
    //

    Status = IopOpenRegistryKey (
        &hBootKey,
        NULL,
        &CmRegistryMachineSystemCurrentControlSetControlBootLog,
        KEY_ALL_ACCESS,
        FALSE
        );

    if (NT_SUCCESS(Status)) {
        for (Index = 0; Index < BootLogRecord->NextKey; Index++) {
            swprintf(NameBuffer, L"%d", Index);

            RtlCreateUnicodeString(&KeyName, NameBuffer);

            Status = IopOpenRegistryKey (
                &hLogKey,
                hBootKey,
                &KeyName,
                KEY_ALL_ACCESS,
                FALSE
                );

            if (NT_SUCCESS(Status)) {
                Status = IopGetRegistryValue(
                    hLogKey,
                    L"",
                    &Information
                    );

                if (NT_SUCCESS(Status)){
                    RtlInitUnicodeString(&LogString, (PWSTR) ((PUCHAR)Information + Information->DataOffset));
                    IopBootLogToFile(&LogString);
                }
                ExFreePool(Information);
                ZwDeleteKey(hLogKey);
                ZwClose(hLogKey);
            }
        }
        ZwDeleteKey(hBootKey);
        ZwClose(hBootKey);

        //
        // Write directly to the file from now on.
        //

        BootLogRecord->FileLogging = TRUE;
    }

    ExReleaseResourceLite(&BootLogRecord->Resource);
    KeLeaveCriticalRegionThread(CurrentThread);
}


NTSTATUS
IopBootLogToFile(
    PUNICODE_STRING String
    )
/*++

Routine Description:

    Write the buffer into the log file.

Arguments:

    Buffer - pointer to the string to write out.
    Length - number of bytes to write

Return Value:

    The function status is the final status of the operation.


--*/
{
    OBJECT_ATTRIBUTES ObjA;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE FileHandle;
    WCHAR UnicodeHeader = 0xfeff;
    PKTHREAD CurrentThread;

    PAGED_CODE();

    if (BootLogRecord == NULL) {
        return STATUS_SUCCESS;
    }

    CurrentThread = KeGetCurrentThread ();
    KeEnterCriticalRegionThread(CurrentThread);
    ExAcquireResourceExclusiveLite(&BootLogRecord->Resource, TRUE);

    if (BootLogRecord->LogFileName.Buffer == NULL) {
        RtlInitUnicodeString(&BootLogRecord->LogFileName, L"\\SystemRoot\\ntbtlog.txt");
    }

    InitializeObjectAttributes(&ObjA, &BootLogRecord->LogFileName, OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE, NULL, NULL);

    Status = ZwCreateFile(&FileHandle,
                            GENERIC_WRITE,
                            &ObjA,
                            &IoStatusBlock,
                            NULL,
                            FILE_ATTRIBUTE_NORMAL,
                            FILE_SHARE_READ,
                            FILE_OPEN_IF,
                            FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY,
                            NULL,
                            0
                            );

    if (NT_SUCCESS(Status)) {

        //
        // If the file is created for the first time, write the header.
        //

        if (IoStatusBlock.Information == FILE_CREATED) {

            Status = ZwWriteFile(
                        FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatusBlock,
                        (PVOID) &UnicodeHeader,
                        sizeof(WCHAR),
                        NULL,
                        NULL
                        );
        }

        if (NT_SUCCESS(Status)) {

            LARGE_INTEGER EndOfFile;

            EndOfFile.HighPart = 0xffffffff;
            EndOfFile.LowPart = FILE_WRITE_TO_END_OF_FILE;

            Status = ZwWriteFile(
                        FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatusBlock,
                        (PVOID) String->Buffer,
                        String->Length,
                        &EndOfFile,
                        NULL
                        );

        }

        ZwClose(FileHandle);
    }

    ExReleaseResourceLite(&BootLogRecord->Resource);
    KeLeaveCriticalRegionThread(CurrentThread);

    return Status;
}

PLIST_ENTRY
FASTCALL
IopInterlockedInsertHeadList (
    IN PLIST_ENTRY ListHead,
    IN PLIST_ENTRY ListEntry
    )

/*++

Routine Description:

    This function inserts an entry at the head of a list using the I/O
    database lock for synchronization.

Arguments:

    Listhead - Supplies a pointer to the list head.

    ListEntry - Supplies a pointer to the list entry.

Return Value:

    If the list was previously empty, then NULL is returned as the function
    value. Otherwise, a pointer to the previous first entry in the list is
    returned as the function value.

--*/

{

    PLIST_ENTRY entry;
    KIRQL irql;

    irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );
    entry = ListHead->Flink;
    if ( entry == ListHead ) {
        entry = NULL;
    }

    InsertHeadList( ListHead, ListEntry );
    KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
    return entry;
}

PLIST_ENTRY
FASTCALL
IopInterlockedInsertTailList (
    IN PLIST_ENTRY ListHead,
    IN PLIST_ENTRY ListEntry
    )

/*++

Routine Description:

    This function inserts an entry at the tail of a list using the I/O
    database lock for synchronization.

Arguments:

    Listhead - Supplies a pointer to the list head.

    ListEntry - Supplies a pointer to the list entry.

Return Value:

    If the list was previously empty, then NULL is returned as the function
    value. Otherwise, a pointer to the previous last entry in the list is
    returned as the function value.

--*/

{

    PLIST_ENTRY entry;
    KIRQL irql;

    irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );
    entry = ListHead->Blink;
    if ( entry == ListHead) {
        entry = NULL;
    }

    InsertTailList( ListHead, ListEntry );
    KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
    return entry;
}

PLIST_ENTRY
FASTCALL
IopInterlockedRemoveHeadList (
    IN PLIST_ENTRY ListHead
    )

/*++

Routine Description:

    This function removes the first entry form a list using the I/O database
    lock for synchronization.

Arguments:

    Listhead - Supplies a pointer to the list head.

Return Value:

    If the list is empty, then NULL is returned as the function value.
    Otherwise, a pointer to the first entry in the list is returned as
    the function value.

--*/

{

    PLIST_ENTRY entry;
    KIRQL irql;

    irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );
    entry = ListHead->Flink;
    if ( entry != ListHead ) {
        RemoveEntryList( entry );

    } else {
        entry = NULL;
    }

    KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
    return entry;
}

ULONG
FASTCALL
IopInterlockedDecrementUlong (
   IN KSPIN_LOCK_QUEUE_NUMBER Number,
   IN OUT PLONG Addend
   )

/*++

Routine Description:

    This function decrements the specified value using a queued spin lock
    for synchronization.

Arguments:

    Number - Supplies the number of the queued spin lock.

    Addend - Supplies a pointer to the variable to be decremented.

Return Value:

    The value of the variable before the decrement is applied.

--*/

{

    KIRQL irql;
    ULONG value;

#if !DBG && defined(NT_UP)
    UNREFERENCED_PARAMETER (Number);
#endif

    irql = KeAcquireQueuedSpinLock( Number );
    *(LONG volatile *)Addend -= 1;
    value = *Addend;
    KeReleaseQueuedSpinLock( Number, irql );
    return value + 1;
}

ULONG
FASTCALL
IopInterlockedIncrementUlong (
   IN KSPIN_LOCK_QUEUE_NUMBER Number,
   IN OUT PLONG Addend
   )

/*++

Routine Description:

    This function increments the specified value using a queued spin lock
    for synchronization.

Arguments:

    Number - Supplies the number of the queued spin lock.

    Addend - Supplies a pointer to the variable to be incremented.

Return Value:

    The value of the variable before the increment is applied.

--*/

{

    KIRQL irql;
    ULONG value;

#if !DBG && defined(NT_UP)
    UNREFERENCED_PARAMETER (Number);
#endif

    irql = KeAcquireQueuedSpinLock( Number );
    *(LONG volatile *)Addend += 1;
    value = *Addend;
    KeReleaseQueuedSpinLock( Number, irql );
    return value - 1;
}

BOOLEAN
IopCallBootDriverReinitializationRoutines(
    VOID
    )

/*++

Routine Description:

    This routine processes the boot driver reinitialization list.  It calls each
    entry and then removes it from the list.

Arguments:

    NONE

Returns:

    TRUE if any entries were processed.

--*/

{
    PLIST_ENTRY entry;
    PREINIT_PACKET reinitEntry;
    BOOLEAN routinesFound = FALSE;

    //
    // Walk the list reinitialization list in case this driver, or
    // some other driver, has requested to be invoked at a re-
    // initialization entry point.
    //

    while (entry = IopInterlockedRemoveHeadList( &IopBootDriverReinitializeQueueHead )) {
        routinesFound = TRUE;
        reinitEntry = CONTAINING_RECORD( entry, REINIT_PACKET, ListEntry );
        reinitEntry->DriverObject->DriverExtension->Count++;
        reinitEntry->DriverObject->Flags &= ~DRVO_BOOTREINIT_REGISTERED;
        reinitEntry->DriverReinitializationRoutine( reinitEntry->DriverObject,
                                                    reinitEntry->Context,
                                                    reinitEntry->DriverObject->DriverExtension->Count );
        ExFreePool( reinitEntry );
    }

    return routinesFound;
}

BOOLEAN
IopCallDriverReinitializationRoutines(
    VOID
    )

/*++

Routine Description:

    This routine processes the driver reinitialization list.  It calls each
    entry and then removes it from the list.

Arguments:

    NONE

Returns:

    TRUE if any entries were processed.

--*/

{
    PLIST_ENTRY entry;
    PREINIT_PACKET reinitEntry;
    BOOLEAN routinesFound = FALSE;

    PAGED_CODE();

    //
    // Walk the list reinitialization list in case this driver, or
    // some other driver, has requested to be invoked at a re-
    // initialization entry point.
    //

    while (entry = IopInterlockedRemoveHeadList( &IopDriverReinitializeQueueHead )) {
        routinesFound = TRUE;
        reinitEntry = CONTAINING_RECORD( entry, REINIT_PACKET, ListEntry );
        reinitEntry->DriverObject->DriverExtension->Count++;
        reinitEntry->DriverObject->Flags &= ~DRVO_REINIT_REGISTERED;
        reinitEntry->DriverReinitializationRoutine( reinitEntry->DriverObject,
                                                    reinitEntry->Context,
                                                    reinitEntry->DriverObject->DriverExtension->Count );
        ExFreePool( reinitEntry );
    }

    return routinesFound;
}

PDRIVER_OBJECT
IopReferenceDriverObjectByName (
    IN PUNICODE_STRING DriverName
    )

/*++

Routine Description:

    This routine references a driver object by a given driver name.

Arguments:

    DriverName - supplies a pointer to the name of the driver whose driver object is
        to be referenced.

Returns:

    A pointer to a DRIVER_OBJECT if succeeds.  Otherwise, a NULL value.

--*/

{
    OBJECT_ATTRIBUTES objectAttributes;
    HANDLE driverHandle;
    NTSTATUS status;
    PDRIVER_OBJECT driverObject;

    //
    // Make sure the driver name is valid.
    //

    if (DriverName->Length == 0) {
        return NULL;
    }

    InitializeObjectAttributes(&objectAttributes,
                               DriverName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL
                               );
    status = ObOpenObjectByName(&objectAttributes,
                                IoDriverObjectType,
                                KernelMode,
                                NULL,
                                FILE_READ_ATTRIBUTES,
                                NULL,
                                &driverHandle
                                );
    if (NT_SUCCESS(status)) {

        //
        // Now reference the driver object.
        //

        status = ObReferenceObjectByHandle(driverHandle,
                                           0,
                                           IoDriverObjectType,
                                           KernelMode,
                                           &driverObject,
                                           NULL
                                           );
        NtClose(driverHandle);
        if (NT_SUCCESS(status)) {
            return driverObject;
        }
    }

    return NULL;
}


PIRP
IopAllocateReserveIrp(
    IN CCHAR StackSize
    )
/*++

Routine Description:

    This routine allocates a reserve IRP for paging reads.

Arguments:

    StackSize - IRP stack size.

Return Value:

    The function value is an IRP.

--*/
{
    PIOP_RESERVE_IRP_ALLOCATOR  allocator = &IopReserveIrpAllocator;

    if (StackSize > allocator->ReserveIrpStackSize) {
        return NULL;
    }


    while (InterlockedExchange(&allocator->IrpAllocated, 1) == 1) {

        (VOID)KeWaitForSingleObject(&allocator->Event,
                                    Executive,
                                    KernelMode,
                                    FALSE,
                                    (PLARGE_INTEGER)0);
    }

    IoInitializeIrp(allocator->ReserveIrp, IoSizeOfIrp(StackSize), StackSize);
    return (allocator->ReserveIrp);
}

VOID
IopFreeReserveIrp(
    IN  CCHAR   PriorityBoost
    )
/*++

Routine Description:

    This routine frees a reserve IRP

Arguments:

    PriorityBoost - Boost to be supplied to waiting threads.

Return Value:

    None

--*/
{
    InterlockedExchange(&IopReserveIrpAllocator.IrpAllocated, 0);
    KeSetEvent(&IopReserveIrpAllocator.Event, PriorityBoost, FALSE);
}


NTSTATUS
IopGetBasicInformationFile(
    IN  PFILE_OBJECT            FileObject,
    IN  PFILE_BASIC_INFORMATION BasicInformationBuffer
    )
/*++

Routine Description:

    This routine gets the basic information of a fileobject.

Arguments:

    FileObject  - Fileobject for which the information is needed.
    BasicInformationBuffer - Buffer of type FILE_BASIC_INFORMATION

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject;
    IO_STATUS_BLOCK localIoStatus;
    PFAST_IO_DISPATCH fastIoDispatch;
    ULONG   lengthNeeded;
    BOOLEAN queryResult;

    PAGED_CODE();

    deviceObject = IoGetRelatedDeviceObject( FileObject );

    fastIoDispatch = deviceObject->DriverObject->FastIoDispatch;

    if (fastIoDispatch && fastIoDispatch->FastIoQueryBasicInfo) {

        queryResult = fastIoDispatch->FastIoQueryBasicInfo( FileObject,
                                                            (FileObject->Flags & FO_SYNCHRONOUS_IO) ? TRUE : FALSE,
                                                            BasicInformationBuffer,
                                                            &localIoStatus,
                                                            deviceObject );
        if (queryResult) {
            return (localIoStatus.Status);
        }
    }

    //
    // Use the special API because the fileobject may be synchronous.
    //

    status = IopGetFileInformation(FileObject,
                                   sizeof(FILE_BASIC_INFORMATION),
                                   FileBasicInformation,
                                   BasicInformationBuffer,
                                   &lengthNeeded);
    return status;
}

PVPB
IopMountInitializeVpb(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PDEVICE_OBJECT  AttachedDevice,
    IN  ULONG           RawMountOnly
    )
/*++

Routine Description:

    This routine initializes the mounted volume VPB holding the VPB lock.

Arguments:

    DeviceObject  - Disk device object
    AttachedDevice - Top of FS stack.
    RawMountOnly   - Only allow raw mounts

Return Value:

    none.

--*/
{
    KIRQL   irql;
    PVPB    vpb;

    IoAcquireVpbSpinLock(&irql);

    vpb = DeviceObject->Vpb;

    vpb->Flags = VPB_MOUNTED;


    //
    // We explicitly propagate VPB_RAW_MOUNT as the previous
    // statement that has been there for a long time in NT
    // could be clearing other flags which should be cleared.
    //

    if (RawMountOnly) {
        vpb->Flags |= VPB_RAW_MOUNT;
    }

    vpb->DeviceObject->StackSize = (UCHAR) (AttachedDevice->StackSize + 1);

    //
    // Set the reverse Vpb pointer in the filesystem device object's VPB.
    //

    vpb->DeviceObject->DeviceObjectExtension->Vpb = vpb;

    vpb->ReferenceCount += 1;

    IoReleaseVpbSpinLock(irql);

    return vpb;
}

BOOLEAN
IopVerifyDeviceObjectOnStack(
    IN  PDEVICE_OBJECT  BaseDeviceObject,
    IN  PDEVICE_OBJECT  TopDeviceObject
    )
/*++

Routine Description:

    This routine checks if a device object is on a device stack.

Arguments:

    BaseDeviceObject  - Lowest device object on the stack.
    TopDeviceObject - Device which is to be tested.

Return Value:

    Returns TRUE if TopDeviceObject is on stack.

--*/
{
    KIRQL           irql;
    PDEVICE_OBJECT  currentDeviceObject;

    //
    // Loop through all of the device object's attached to the specified
    // device.  When the last device object is found that is not attached
    // to, return it.
    //

    ASSERT( BaseDeviceObject != NULL);

    irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );
    currentDeviceObject = BaseDeviceObject;

    do {
        if (currentDeviceObject == TopDeviceObject) {
            KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
            return TRUE;
        }
        currentDeviceObject = currentDeviceObject->AttachedDevice;
    } while (currentDeviceObject);

    KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
    return FALSE;
}

BOOLEAN
IopVerifyDiskSignature(
    IN PDRIVE_LAYOUT_INFORMATION_EX DriveLayout,
    IN PARC_DISK_SIGNATURE          LoaderDiskBlock,
    OUT PULONG                      DiskSignature
    )
/*++

Routine Description:

    This routine verifies disk signature that is present in the loader block
    and that retrieved by the storage drivers.

Arguments:

    DriveLayout - Information obtained from the storage stack.
    LoaderDiskBlock - Signature info from loader
    SectorBuffer - Buffer containing sector 0 on the disk.
    DiskSignature - If successful contains disk signature.

Return Value:

    Returns TRUE if signature matches.

--*/
{
    ULONG   diskSignature;

    if (!LoaderDiskBlock->ValidPartitionTable) {
        return FALSE;
    }

    //
    // Save off the signature in local variable if
    // its a MBR disk
    //

    if (DriveLayout->PartitionStyle == PARTITION_STYLE_MBR) {
        diskSignature = DriveLayout->Mbr.Signature;
        if (LoaderDiskBlock->Signature == diskSignature) {
            if (DiskSignature) {
                *DiskSignature = diskSignature;
            }
            return TRUE;
        }
    }

    //
    // Get hold of the signature from MBR if its GPT disk
    //

    if (DriveLayout->PartitionStyle == PARTITION_STYLE_GPT) {

        if (!LoaderDiskBlock->IsGpt) {
            return FALSE;
        }

        if (!RtlEqualMemory(LoaderDiskBlock->GptSignature, &DriveLayout->Gpt.DiskId, sizeof(GUID))) {
            return FALSE;
        }

        if (DiskSignature) {
            *DiskSignature = 0;
        }
        return TRUE;
    }

    return FALSE;
}

NTSTATUS
IopGetDriverPathInformation(
    IN  PFILE_OBJECT                        FileObject,
    IN  PFILE_FS_DRIVER_PATH_INFORMATION    FsDpInfo,
    IN  ULONG                               Length
    )
/*++

Routine Description:

    This routine returns true if a driver specified by name is present in the IO path
    for the fileobject.

Arguments:

    FileObject - FileObject to which IO is issued.
    FsDpInfo - Info that contains driver name.

Return Value:

    NTSTATUS

--*/
{
    UNICODE_STRING  driverString;
    PDRIVER_OBJECT  driverObject;
    NTSTATUS        status;
    KIRQL           irql;

    if ((ULONG) (Length - FIELD_OFFSET( FILE_FS_DRIVER_PATH_INFORMATION, DriverName[0] )) < FsDpInfo->DriverNameLength) {
        return STATUS_INVALID_PARAMETER;
    }

    driverString.Buffer = FsDpInfo->DriverName;
    driverString.Length = (USHORT)FsDpInfo->DriverNameLength;
    driverString.MaximumLength = (USHORT)FsDpInfo->DriverNameLength;

    status = ObReferenceObjectByName(&driverString,
                                     OBJ_CASE_INSENSITIVE,
                                     NULL,                 // access state
                                     0,                    // access mask
                                     IoDriverObjectType,
                                     KernelMode,
                                     NULL,                 // parse context
                                     &driverObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );


    if (FileObject->Vpb != NULL && FileObject->Vpb->DeviceObject != NULL) {

        //
        // Check the disk filesystem stack.
        //

        if (IopVerifyDriverObjectOnStack(FileObject->Vpb->DeviceObject, driverObject)) {

            FsDpInfo->DriverInPath = TRUE;
            KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );
            ObDereferenceObject(driverObject);
            return STATUS_SUCCESS;
        }
    }

    //
    // Check the storage stack or non disk filesystem stack.
    //

    FsDpInfo->DriverInPath = IopVerifyDriverObjectOnStack(FileObject->DeviceObject, driverObject);
    KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );

    ObDereferenceObject(driverObject);
    return STATUS_SUCCESS;
}

BOOLEAN
IopVerifyDriverObjectOnStack(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PDRIVER_OBJECT DriverObject
    )
/*++

Routine Description:

    This routine returns true if a driver specified by driverObject is present in the IO path
    for the deviceobject.

Arguments:

    DeviceObject - DeviceObject to which IO is issued.
    Driverobject - DriverObject to be checked.

Return Value:

    TRUE if the driverObject is in the IO path

--*/
{
    PDEVICE_OBJECT  currentDeviceObject;

    currentDeviceObject = IopGetDeviceAttachmentBase(DeviceObject);

    while (currentDeviceObject) {
        if (currentDeviceObject->DriverObject == DriverObject) {
            return TRUE;
        }
        currentDeviceObject = currentDeviceObject->AttachedDevice;
    }
    return FALSE;
}

VOID
IopIncrementDeviceObjectHandleCount(
    IN  PDEVICE_OBJECT  DeviceObject
    )
{
  IopInterlockedIncrementUlong( LockQueueIoDatabaseLock,
                                &DeviceObject->ReferenceCount );
}

VOID
IopDecrementDeviceObjectHandleCount(
    IN  PDEVICE_OBJECT  DeviceObject
    )
{
    IopDecrementDeviceObjectRef(DeviceObject, FALSE, FALSE);
}

NTSTATUS
IopInitializeIrpStackProfiler(
    VOID
    )
/*++

Routine Description:

    This routine initializes the Irp stack profiler.

Arguments:


Return Value:

    NTSTATUS

--*/
{
    LARGE_INTEGER   dueTime;

    RtlZeroMemory(IopIrpStackProfiler.Profile, MAX_LOOKASIDE_IRP_STACK_COUNT * sizeof(ULONG));

    KeInitializeTimer(&IopIrpStackProfiler.Timer);
    KeInitializeDpc(&IopIrpStackProfiler.Dpc, IopIrpStackProfilerTimer, &IopIrpStackProfiler);

    dueTime.QuadPart = - IOP_PROFILE_TIME_PERIOD * 10 * 1000 * 1000;  // 1 Minute with a recurring period of a minute
    IopIrpStackProfiler.TriggerCount = 0;

    KeSetTimerEx(&IopIrpStackProfiler.Timer, dueTime, IOP_PROFILE_TIME_PERIOD * 1000, &IopIrpStackProfiler.Dpc);

    return STATUS_SUCCESS;
}

VOID
IopIrpStackProfilerTimer(
    IN struct _KDPC *Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )
/*++

Routine Description:

    This routine is the timer DPC routine.

Arguments:

    DeferredContext - Pointer to the profiler structure.

Return Value:

    None

--*/
{
    PIOP_IRP_STACK_PROFILER profiler = DeferredContext;
    LONG                    i;
    ULONG                   totalIrpsCounted;

    UNREFERENCED_PARAMETER (Dpc);
    UNREFERENCED_PARAMETER (SystemArgument1);
    UNREFERENCED_PARAMETER (SystemArgument2);

    //
    // If profile stack count is now enabled, we wait for ProfileDuration * 60 secs to stop
    // it. Once stopped, IopProcessIrpStackProfiler will tally the counts and find the top two
    // stack counts.
    //

    if (profiler->Flags & IOP_PROFILE_STACK_COUNT) {
        totalIrpsCounted = 0;
        for (i = 0; i < MAX_LOOKASIDE_IRP_STACK_COUNT; i++) {
            totalIrpsCounted += profiler->Profile[i];
        }
        if (totalIrpsCounted > NUM_SAMPLE_IRPS) {
            profiler->Flags &= ~IOP_PROFILE_STACK_COUNT; // Stop the profiling.
            IopProcessIrpStackProfiler();
        }
        return;
    }

    //
    // Every IOP_PROFILE_TRIGGER_INTERVAL * 60 seconds we turn on the profiling
    //

    profiler->TriggerCount++;
    if ((profiler->TriggerCount % IOP_PROFILE_TRIGGER_INTERVAL) == 0) {
        profiler->Flags |= IOP_PROFILE_STACK_COUNT;  // Enable profiling
    }
}

VOID
IopProcessIrpStackProfiler(
    VOID
    )
/*++

Routine Description:

    This routine profiles and resets the per-processor counters. It sets the variables
    IopLargeIrpStackLocations and IopSmallIrpStackLocations with the values learned from
    counters.

Arguments:

    None

Return Value:

    None

--*/
{
    PIOP_IRP_STACK_PROFILER profiler = &IopIrpStackProfiler;
    ULONG                   i;
    LONG                    bucket = 0;
    ULONG                   stackCount = 0;
    LONG                    numRequests;

    numRequests = 0;
    for (i = BASE_STACK_COUNT; i < MAX_LOOKASIDE_IRP_STACK_COUNT; i++) {
        numRequests += profiler->Profile[i];
        profiler->Profile[i] = 0;
        if (numRequests > bucket) {
            stackCount = i;
            bucket = numRequests;
        }
        numRequests = 0;
    }


    //
    // If the top allocation is less than the minimum threshold do nothing.
    //

    if (bucket < MIN_IRP_THRESHOLD) {
        return;
    }

    //
    // Update the global variables. This should cause IoAllocateIrp to start using the new IRPs
    // right away.
    //

    if (IopLargeIrpStackLocations != stackCount) {
        IopLargeIrpStackLocations = stackCount;
    }
}

BOOLEAN
IopReferenceVerifyVpb(
    IN  PDEVICE_OBJECT  DeviceObject,
    OUT PVPB            *Vpb,
    OUT PDEVICE_OBJECT  *FsDeviceObject
    )
/*++

Routine Description:

    This routine tests if a device object is mounted and if so retrieves
    the fs device object and returns it along with the VPB. Its called
    from IoVerifyVolume. It takes a reference so that the VPB and the fsDeviceObject
    don't vanish.

Arguments:

    DeviceObject    - DeviceObject of the drive we have to verify.
    Vpb             - The vpb is returned here.
    FsDeviceObject  - The fs deviceobject is returned here.

Return Value:

    TRUE if mounted, FALSE otherwise.

--*/
{
    KIRQL   irql;
    BOOLEAN isMounted = FALSE;
    PVPB    vpb;

    IoAcquireVpbSpinLock(&irql);

    *Vpb = NULL;
    *FsDeviceObject = NULL;

    vpb = DeviceObject->Vpb;

    if (vpb && vpb->Flags & VPB_MOUNTED) {
        *FsDeviceObject = vpb->DeviceObject;
        isMounted = TRUE;
        *Vpb = vpb;
        vpb->ReferenceCount++;
    }

    IoReleaseVpbSpinLock(irql);

    return isMounted;
}

