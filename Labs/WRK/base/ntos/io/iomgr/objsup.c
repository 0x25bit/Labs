/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    objsup.c

Abstract:

    This module contains the object support routine for the NT I/O system.

--*/

#include "iomgr.h"

NTSTATUS
IopSetDeviceSecurityDescriptors(
    IN PDEVICE_OBJECT           OriginalDeviceObject,
    IN PDEVICE_OBJECT           DeviceObject,
    IN PSECURITY_INFORMATION    SecurityInformation,
    IN PSECURITY_DESCRIPTOR     SecurityDescriptor,
    IN POOL_TYPE                PoolType,
    IN PGENERIC_MAPPING         GenericMapping
    );

NTSTATUS
IopSetDeviceSecurityDescriptor(
    IN PDEVICE_OBJECT           DeviceObject,
    IN PSECURITY_INFORMATION    SecurityInformation,
    IN PSECURITY_DESCRIPTOR     SecurityDescriptor,
    IN POOL_TYPE                PoolType,
    IN PGENERIC_MAPPING         GenericMapping
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, IopCloseFile)
#pragma alloc_text(PAGE, IopDeleteFile)
#pragma alloc_text(PAGE, IopDeleteDevice)
#pragma alloc_text(PAGE, IopDeleteDriver)
#pragma alloc_text(PAGE, IopGetSetSecurityObject)
#pragma alloc_text(PAGE, IopSetDeviceSecurityDescriptors)
#pragma alloc_text(PAGE, IopSetDeviceSecurityDescriptor)
#endif

VOID
IopCloseFile(
    IN PEPROCESS Process OPTIONAL,
    IN PVOID Object,
    IN ULONG GrantedAccess,
    IN ULONG_PTR ProcessHandleCount,
    IN ULONG_PTR SystemHandleCount
    )

/*++

Routine Description:

    This routine is invoked whenever a handle to a file is deleted.  If the
    handle being deleted is the last handle to the file (the ProcessHandleCount
    parameter is one), then all locks for the file owned by the specified
    process must be released.

    Likewise, if the SystemHandleCount is one then this is the last handle
    for this for file object across all processes.  For this case, the file
    system is notified so that it can perform any necessary cleanup on the
    file.

Arguments:

    Process - A pointer to the process that closed the handle.

    Object - A pointer to the file object that the handle referenced.

    GrantedAccess - Access that was granted to the object through the handle.

    ProcessHandleCount - Count of handles outstanding to the object for the
        process specified by the Process argument.  If the count is one
        then this is the last handle to this file by that process.

    SystemHandleCount - Count of handles outstanding to the object for the
        entire system.  If the count is one then this is the last handle
        to this file in the system.

Return Value:

    None.

--*/

{
    PIRP irp;
    PIO_STACK_LOCATION irpSp;
    PDEVICE_OBJECT deviceObject;
    PFAST_IO_DISPATCH fastIoDispatch;
    NTSTATUS status;
    KEVENT event;
    PFILE_OBJECT fileObject;
    KIRQL irql;

    UNREFERENCED_PARAMETER( GrantedAccess );

    PAGED_CODE();

    //
    // If the handle count is not one then this is not the last close of
    // this file for the specified process so there is nothing to do.
    //

    if (ProcessHandleCount != 1) {
        return;
    }

    fileObject = (PFILE_OBJECT) Object;

    if (fileObject->LockOperation && SystemHandleCount != 1) {

        IO_STATUS_BLOCK localIoStatus;

        //
        // This is the last handle for the specified process and the process
        // called the NtLockFile or NtUnlockFile system services at least once.
        // Also, this is not the last handle for this file object system-wide
        // so unlock all of the pending locks for this process.  Note that
        // this check causes an optimization so that if this is the last
        // system-wide handle to this file object the cleanup code will take
        // care of releasing any locks on the file rather than having to
        // send the file system two different packets to get them shut down.

        //
        // Get the address of the target device object and the Fast I/O dispatch
        //

        if (!(fileObject->Flags & FO_DIRECT_DEVICE_OPEN)) {
            deviceObject = IoGetRelatedDeviceObject( fileObject );
        } else {
            deviceObject = IoGetAttachedDevice( fileObject->DeviceObject );
        }
        fastIoDispatch = deviceObject->DriverObject->FastIoDispatch;

        //
        // If this file is open for synchronous I/O, wait until this thread
        // owns it exclusively since there may still be a thread using it.
        // This occurs when a system service owns the file because it owns
        // the semaphore, but the I/O completion code has already dereferenced
        // the file object itself.  Without waiting here for the same semaphore
        // there would be a race condition in the service who owns it now. The
        // service needs to be able to access the object w/o it going away after
        // its wait for the file event is satisfied.
        //

        if (fileObject->Flags & FO_SYNCHRONOUS_IO) {

            BOOLEAN interrupted;

            if (!IopAcquireFastLock( fileObject )) {
                (VOID) IopAcquireFileObjectLock( fileObject,
                                                 KernelMode,
                                                 FALSE,
                                                 &interrupted );
            }
        }

        //
        // Turbo unlock support.  If the fast Io Dispatch specifies a fast lock
        // routine then we'll first try and calling it with the specified lock
        // parameters.  If this is all successful then we do not need to do
        // the Irp based unlock all call.
        //

        if (fastIoDispatch &&
            fastIoDispatch->FastIoUnlockAll &&
            fastIoDispatch->FastIoUnlockAll( fileObject,
                                             PsGetCurrentProcess(),
                                             &localIoStatus,
                                             deviceObject )) {

            NOTHING;

        } else {

            //
            // Initialize the local event that will be used to synchronize access
            // to the driver completing this I/O operation.
            //

            KeInitializeEvent( &event, SynchronizationEvent, FALSE );

            //
            // Reset the event in the file object.
            //

            KeClearEvent( &fileObject->Event );

            //
            // Allocate and initialize the I/O Request Packet (IRP) for this
            // operation.
            //

            irp = IopAllocateIrpMustSucceed( deviceObject->StackSize );
            irp->Tail.Overlay.OriginalFileObject = fileObject;
            irp->Tail.Overlay.Thread = PsGetCurrentThread();
            irp->RequestorMode = KernelMode;

            //
            // Fill in the service independent parameters in the IRP.
            //

            irp->UserEvent = &event;
            irp->UserIosb = &irp->IoStatus;
            irp->Flags = IRP_SYNCHRONOUS_API;
            irp->Overlay.AsynchronousParameters.UserApcRoutine = (PIO_APC_ROUTINE) NULL;

            //
            // Get a pointer to the stack location for the first driver.  This will
            // be used to pass the original function codes and parameters.  No
            // function-specific parameters are required for this operation.
            //

            irpSp = IoGetNextIrpStackLocation( irp );
            irpSp->MajorFunction = IRP_MJ_LOCK_CONTROL;
            irpSp->MinorFunction = IRP_MN_UNLOCK_ALL;
            irpSp->FileObject = fileObject;

            //
            //  Reference the fileobject again for the IRP (cleared on completion)
            //

            ObReferenceObject( fileObject );

            //
            // Insert the packet at the head of the IRP list for the thread.
            //

            IopQueueThreadIrp( irp );

            //
            // Invoke the driver at its appropriate dispatch entry with the IRP.
            //

            status = IoCallDriver( deviceObject, irp );

            //
            // If no error was incurred, wait for the I/O operation to complete.
            //

            if (status == STATUS_PENDING) {
                (VOID) KeWaitForSingleObject( &event,
                                              UserRequest,
                                              KernelMode,
                                              FALSE,
                                              (PLARGE_INTEGER) NULL );
            }
        }

        //
        // If this operation was a synchronous I/O operation, release the
        // semaphore so that the file can be used by other threads.
        //

        if (fileObject->Flags & FO_SYNCHRONOUS_IO) {
            IopReleaseFileObjectLock( fileObject );
        }
    }

    if (SystemHandleCount == 1) {

        //
        // The last handle to this file object for all of the processes in the
        // system has just been closed, so invoke the driver's "cleanup" handler
        // for this file.  This is the file system's opportunity to remove any
        // share access information for the file, to indicate that if the file
        // is opened for a caching operation and this is the last file object
        // to the file, then it can do whatever it needs with memory management
        // to cleanup any information.
        //
        // Begin by getting the address of the target device object.
        //

        if (!(fileObject->Flags & FO_DIRECT_DEVICE_OPEN)) {
            deviceObject = IoGetRelatedDeviceObject( fileObject );
        } else {
            deviceObject = IoGetAttachedDevice( fileObject->DeviceObject );
        }

        //
        // Ensure that the I/O system believes that this file has a handle
        // associated with it in case it doesn't actually get one from the
        // Object Manager.  This is done because sometimes the Object Manager
        // actually creates a handle, but the I/O system never finds out
        // about it so it attempts to send two cleanups for the same file.
        //

        fileObject->Flags |= FO_HANDLE_CREATED;

        //
        // If this file is open for synchronous I/O, wait until this thread
        // owns it exclusively since there may still be a thread using it.
        // This occurs when a system service owns the file because it owns
        // the semaphore, but the I/O completion code has already dereferenced
        // the file object itself.  Without waiting here for the same semaphore
        // there would be a race condition in the service who owns it now. The
        // service needs to be able to access the object w/o it going away after
        // its wait for the file event is satisfied.
        // Note : We need to do this only if IopCloseFile is not called from
        // IopDeleteFile
        //

        if (Process && fileObject->Flags & FO_SYNCHRONOUS_IO) {

            BOOLEAN interrupted;

            if (!IopAcquireFastLock( fileObject )) {
                (VOID) IopAcquireFileObjectLock( fileObject,
                                                 KernelMode,
                                                 FALSE,
                                                 &interrupted );
            }
        }

        //
        // Initialize the local event that will be used to synchronize access
        // to the driver completing this I/O operation.
        //

        KeInitializeEvent( &event, SynchronizationEvent, FALSE );

        //
        // Reset the event in the file object.
        //

        KeClearEvent( &fileObject->Event );

        //
        // Allocate and initialize the I/O Request Packet (IRP) for this
        // operation.
        //

        irp = IopAllocateIrpMustSucceed( deviceObject->StackSize );
        irp->Tail.Overlay.OriginalFileObject = fileObject;
        irp->Tail.Overlay.Thread = PsGetCurrentThread();
        irp->RequestorMode = KernelMode;

        //
        // Fill in the service independent parameters in the IRP.
        //

        irp->UserEvent = &event;
        irp->UserIosb = &irp->IoStatus;
        irp->Overlay.AsynchronousParameters.UserApcRoutine = (PIO_APC_ROUTINE) NULL;
        irp->Flags = IRP_SYNCHRONOUS_API | IRP_CLOSE_OPERATION;

        //
        // Get a pointer to the stack location for the first driver.  This will
        // be used to pass the original function codes and parameters.  No
        // function-specific parameters are required for this operation.
        //

        irpSp = IoGetNextIrpStackLocation( irp );
        irpSp->MajorFunction = IRP_MJ_CLEANUP;
        irpSp->FileObject = fileObject;

        //
        // Insert the packet at the head of the IRP list for the thread.
        //

        IopQueueThreadIrp( irp );

        //
        // Update the operation count statistic for the current process for
        // operations other than read and write.
        //

        IopUpdateOtherOperationCount();

        //
        // Invoke the driver at its appropriate dispatch entry with the IRP.
        //

        status = IoCallDriver( deviceObject, irp );

        //
        // If no error was incurred, wait for the I/O operation to complete.
        //

        if (status == STATUS_PENDING) {
            (VOID) KeWaitForSingleObject( &event,
                                          UserRequest,
                                          KernelMode,
                                          FALSE,
                                          (PLARGE_INTEGER) NULL );
        }

        //
        // The following code tears down the IRP by hand since it may not
        // either be possible to it to be completed (because this code was
        // invoked as APC_LEVEL in the first place - or because the reference
        // count on the object cannot be incremented due to this routine
        // being invoked by the delete file procedure below).  Cleanup IRPs
        // therefore use close semantics (the close operation flag is set
        // in the IRP) so that the I/O complete request routine itself sets
        // the event to the Signaled state.
        //

        KeRaiseIrql( APC_LEVEL, &irql );
        IopDequeueThreadIrp( irp );
        KeLowerIrql( irql );

        //
        // Also, free the IRP.
        //

        IoFreeIrp( irp );

        //
        // If this operation was a synchronous I/O operation, release the
        // semaphore so that the file can be used by other threads.
        // Note : We need to do this only if IopCloseFile is not called from
        // IopDeleteFile
        //

        if (Process && fileObject->Flags & FO_SYNCHRONOUS_IO) {
            IopReleaseFileObjectLock( fileObject );
        }
    }

    return;
}

VOID
IopDeleteFile(
    IN PVOID    Object
    )

/*++

Routine Description:

    This routine is invoked when the last handle to a specific file handle is
    being closed and the file object is going away.  It is the responsibility
    of this routine to perform the following functions:

        o  Notify the device driver that the file object is open on that the
           file is being closed.

        o  Dereference the user's error port for the file object, if there
           is one associated with the file object.

        o  Decrement the device object reference count.

Arguments:

    Object - Pointer to the file object being deleted.

Return Value:

    None.

--*/

{
    PIRP irp;
    PIO_STACK_LOCATION irpSp;
    PDEVICE_OBJECT deviceObject;
    PDEVICE_OBJECT fsDevice = NULL;
    IO_STATUS_BLOCK ioStatusBlock;
    KIRQL irql;
    NTSTATUS status;
    PFILE_OBJECT fileObject;
    KEVENT event;
    PVPB vpb;
    BOOLEAN referenceCountDecremented;

    //
    // Obtain a pointer to the file object.
    //

    fileObject = (PFILE_OBJECT) Object;

    //
    // Get a pointer to the first device driver which should be notified that
    // this file is going away.  If the device driver field is NULL, then this
    // file is being shut down due to an error attempting to get it open in the
    // first place, so do not do any further processing.
    //

    if (fileObject->DeviceObject) {
        if (!(fileObject->Flags & FO_DIRECT_DEVICE_OPEN)) {
            deviceObject = IoGetRelatedDeviceObject( fileObject );
        } else {
            deviceObject = IoGetAttachedDevice( fileObject->DeviceObject );
        }

        // 
        // On IopDeleteFile path the lock should always be ours as there should be
        // no one else using this object.
        //

        ASSERT (!(fileObject->Flags & FO_SYNCHRONOUS_IO) ||
                 (InterlockedExchange( (PLONG) &fileObject->Busy, (ULONG) TRUE ) == FALSE ));

        //
        // If this file has never had a file handle created for it, and yet
        // it exists, invoke the close file procedure so that the file system
        // gets the cleanup IRP it is expecting before sending the close IRP.
        //

        if (!(fileObject->Flags & (FO_HANDLE_CREATED | FO_FILE_OPEN_CANCELLED))) {
            IopCloseFile( (PEPROCESS) NULL,
                          Object,
                          0,
                          1,
                          1 );
        }

        //
        // Reset a local event that can be used to wait for the device driver
        // to close the file.
        //

        KeInitializeEvent( &event, SynchronizationEvent, FALSE );

        //
        // Reset the event in the file object.
        //

        KeClearEvent( &fileObject->Event );

        //
        // Allocate an I/O Request Packet (IRP) to be used in communicating with
        // the appropriate device driver that the file is being closed.  Notice
        // that the allocation of this packet is done without charging quota so
        // that the operation will not fail.  This is done because there is no
        // way to return an error to the caller at this point.
        //

        irp = IoAllocateIrp( deviceObject->StackSize, FALSE );
        if (!irp) {
            irp = IopAllocateIrpMustSucceed( deviceObject->StackSize );
        }

        //
        // Get a pointer to the stack location for the first driver.  This is
        // where the function codes and parameters are placed.
        //

        irpSp = IoGetNextIrpStackLocation( irp );

        //
        // Fill in the IRP, indicating that this file object is being deleted.
        //

        irpSp->MajorFunction = IRP_MJ_CLOSE;
        irpSp->FileObject = fileObject;
        irp->UserIosb = &ioStatusBlock;
        irp->UserEvent = &event;
        irp->Tail.Overlay.OriginalFileObject = fileObject;
        irp->Tail.Overlay.Thread = PsGetCurrentThread();
        irp->AssociatedIrp.SystemBuffer = (PVOID) NULL;
        irp->Flags = IRP_CLOSE_OPERATION | IRP_SYNCHRONOUS_API;

        //
        // Place this packet in the thread's I/O pending queue.
        //

        IopQueueThreadIrp( irp );

        //
        // Decrement the reference count on the VPB, if necessary.  We
        // have to do this BEFORE handing the Irp to the file system
        // because of a trick the file systems play with close, and
        // believe me, you really don't want to know what it is.
        //
        // Since there is not a error path here (close cannot fail),
        // and the file system is the only ome who can actually synchronize
        // with the actual completion of close processing, the file system
        // is the one responsible for Vpb deletion.
        //

        vpb = fileObject->Vpb;


        if (vpb && !(fileObject->Flags & FO_DIRECT_DEVICE_OPEN)) {
            IopInterlockedDecrementUlong( LockQueueIoVpbLock,
                                          (PLONG) &vpb->ReferenceCount );

            //
            // Bump the handle count of the filesystem volume device object.
            // This will prevent the filesystem filter stack from being torn down
            // until after the close IRP completes.
            //

            fsDevice = vpb->DeviceObject;
            if (fsDevice) {
                IopInterlockedIncrementUlong( LockQueueIoDatabaseLock,
                                              &fsDevice->ReferenceCount );
            }
        }

        //
        // If this device object has stated for a fact that it knows it will
        // never have the final non-zero reference count among the other
        // device objects associated with our driver object, then decrement
        // our reference count here BEFORE calling the file system.  This
        // is required because for a special class of device objects, the
        // file system may delete them.
        //

        if (fileObject->DeviceObject->Flags & DO_NEVER_LAST_DEVICE) {
            IopInterlockedDecrementUlong( LockQueueIoDatabaseLock,
                                          &fileObject->DeviceObject->ReferenceCount );

            referenceCountDecremented = TRUE;
        } else {
            referenceCountDecremented = FALSE;
        }

        //
        // Give the device driver the packet.  If this request does not work,
        // there is nothing that can be done about it.  This is unfortunate
        // because the driver may have had problems that it was about to
        // report about other operations (e.g., write behind failures, etc.)
        // that it can no longer report.  The reason is that this routine
        // is really initially invoked by NtClose, which has already closed
        // the caller's handle, and that's what the return status from close
        // indicates:  the handle has successfully been closed.
        //

        status = IoCallDriver( deviceObject, irp );

        if (status == STATUS_PENDING) {
            (VOID) KeWaitForSingleObject( &event,
                                          Executive,
                                          KernelMode,
                                          FALSE,
                                          (PLARGE_INTEGER) NULL );
        }

        //
        // Perform any completion operations that need to be performed on
        // the IRP that was used for this request.  This is done here as
        // as opposed to in normal completion code because there is a race
        // condition between when this routine executes if it was invoked
        // from a special kernel APC (e.g., some IRP was just completed and
        // dereferenced this file object for the last time), and when the
        // special kernel APC because of this packet's completion executing.
        //
        // This problem is solved by not having to queue a special kernel
        // APC routine for completion of this packet.  Rather, it is treated
        // much like a synchronous paging I/O operation, except that the
        // packet is not even freed during I/O completion.  This is because
        // the packet is still in this thread's queue, and there is no way
        // to get it out except at APC_LEVEL.  Unfortunately, the part of
        // I/O completion that needs to dequeue the packet is running at
        // DISPATCH_LEVEL.
        //
        // Hence, the packet must be removed from the queue (synchronized,
        // of course), and then it must be freed.
        //

        KeRaiseIrql( APC_LEVEL, &irql );
        IopDequeueThreadIrp( irp );
        KeLowerIrql( irql );

        IoFreeIrp( irp );

        //
        // Free the file name string buffer if there was one.
        //

        if (fileObject->FileName.Length != 0) {
            ExFreePool( fileObject->FileName.Buffer );
        }

        //
        // If there was an completion port associated w/this file object, dereference
        // it now, and deallocate the completion context pool.
        //

        if (fileObject->CompletionContext) {
            ObDereferenceObject( fileObject->CompletionContext->Port );
            ExFreePool( fileObject->CompletionContext );
        }

        //
        // Free the file context control structure if it exists.
        //

        if (fileObject->Flags & FO_FILE_OBJECT_HAS_EXTENSION) {
            FsRtlPTeardownPerFileObjectContexts(fileObject);
        }

        //
        // Get a pointer to the real device object so its reference count
        // can be decremented.
        //

        deviceObject = fileObject->DeviceObject;

        //
        // Decrement the reference count on the device object.  Note that
        // if the driver has been marked for an unload operation, and the
        // reference count goes to zero, then the driver may need to be
        // unloaded at this point.
        //
        // Note: only do this if the reference count was not already done
        // above.  The device object may be gone in this case.
        //

        if (!referenceCountDecremented) {

            IopDecrementDeviceObjectRef(
                deviceObject,
                FALSE,
                !ObIsObjectDeletionInline(Object)
                );
        }

        //
        // Decrement the filesystem's volume device object handle count
        // so that deletes can proceed.
        //

        if (fsDevice && vpb) {
            IopDecrementDeviceObjectRef(fsDevice,
                                         FALSE,
                                         !ObIsObjectDeletionInline(Object)
                                         );
        }
    }
}

VOID
IopDeleteDriver(
    IN PVOID    Object
    )

/*++

Routine Description:

    This routine is invoked when the reference count for a driver object
    becomes zero.  That is, the last reference for the driver has gone away.
    This routine ensures that the object is cleaned up and the driver
    unloaded.

Arguments:

    Object - Pointer to the driver object whose reference count has gone
        to zero.

Return value:

    None.

--*/

{
    PDRIVER_OBJECT driverObject = (PDRIVER_OBJECT) Object;
    PIO_CLIENT_EXTENSION extension;
    PIO_CLIENT_EXTENSION nextExtension;

    PAGED_CODE();

    ASSERT( !driverObject->DeviceObject );

    //
    // Free any client driver object extensions.
    //

    extension = driverObject->DriverExtension->ClientDriverExtension;
    while (extension != NULL) {

        nextExtension = extension->NextExtension;
        ExFreePool( extension );
        extension = nextExtension;
    }

    //
    // If there is a driver section then unload the driver.
    //

    if (driverObject->DriverSection != NULL) {
        //
        // Make sure any DPC's that may be running inside the driver have completed
        //
        KeFlushQueuedDpcs ();

        MmUnloadSystemImage( driverObject->DriverSection );

        PpDriverObjectDereferenceComplete(driverObject);
    }

    //
    // Free the pool associated with the name of the driver.
    //

    if (driverObject->DriverName.Buffer) {
        ExFreePool( driverObject->DriverName.Buffer );
    }

    //
    // Free the pool associated with the service key name of the driver.
    //

    if (driverObject->DriverExtension->ServiceKeyName.Buffer) {
        ExFreePool( driverObject->DriverExtension->ServiceKeyName.Buffer );
    }

    //
    // Free the pool associated with the FsFilterCallbacks structure.
    //

    if (driverObject->DriverExtension->FsFilterCallbacks) {
        ExFreePool( driverObject->DriverExtension->FsFilterCallbacks );
    }
}

VOID
IopDeleteDevice(
    IN PVOID    Object
    )

/*++

Routine Description:

    This routine is invoked when the reference count for a device object
    becomes zero.  That is, the last reference for the device has gone away.
    This routine ensures that the object is cleaned up and the driver object
    is dereferenced.

Arguments:

    Object - Pointer to the driver object whose reference count has gone
        to zero.

Return value:

    None.

--*/

{
    PDEVICE_OBJECT deviceObject = (PDEVICE_OBJECT) Object;
    PVPB vpb = NULL;

    PAGED_CODE();

    IopDestroyDeviceNode(deviceObject->DeviceObjectExtension->DeviceNode);

    //
    // If there's still a VPB attached then free it.
    //

    vpb = InterlockedExchangePointer(&(deviceObject->Vpb), vpb);

    if(vpb != NULL) {

        ASSERTMSG("Unreferenced device object to be deleted is still in use",
                  ((vpb->Flags & (VPB_MOUNTED | VPB_LOCKED)) == 0));

        ASSERT(vpb->ReferenceCount == 0);
        ExFreePool(vpb);
    }
    if (deviceObject->DriverObject != NULL) {
        ObDereferenceObject( deviceObject->DriverObject );
    }
}


PDEVICE_OBJECT
IopGetDevicePDO(
    IN PDEVICE_OBJECT DeviceObject
    )
/*++

Routine Description:

    Call this routine to obtain the Base PDO for a device object

Arguments:

    DeviceObject - pointer to device object to get PDO for

ReturnValue:

    PDO if DeviceObject is attached to a PDO, otherwise NULL
    The returned PDO is reference-counted

--*/
{
    PDEVICE_OBJECT deviceBaseObject;
    KIRQL irql;

    ASSERT(DeviceObject);

    irql = KeAcquireQueuedSpinLock( LockQueueIoDatabaseLock );
    deviceBaseObject = IopGetDeviceAttachmentBase(DeviceObject);
    if ((deviceBaseObject->Flags & DO_BUS_ENUMERATED_DEVICE) != 0) {
        //
        // we have determined that this is attached to a PDO
        //
        ObReferenceObject( deviceBaseObject );

    } else {
        //
        // not a PDO
        //
        deviceBaseObject = NULL;
    }
    KeReleaseQueuedSpinLock( LockQueueIoDatabaseLock, irql );

    return deviceBaseObject;
}


NTSTATUS
IopSetDeviceSecurityDescriptor(
    IN PDEVICE_OBJECT           DeviceObject,
    IN PSECURITY_INFORMATION    SecurityInformation,
    IN PSECURITY_DESCRIPTOR     SecurityDescriptor,
    IN POOL_TYPE                PoolType,
    IN PGENERIC_MAPPING         GenericMapping
    )
/*++

Routine Description:

    This routine sets the security descriptor on a single device object

Arguments:

    DeviceObject - pointer to base device object

    SecurityInformation - Fields of SD to change
    SecurityDescriptor  - New security descriptor
    PoolType            - Pool type for allocations
    GenericMapping      - Generic mapping for this object

ReturnValue:

    success, or error while setting the descriptor for the device object.

--*/
{

    PSECURITY_DESCRIPTOR OldDescriptor;
    PSECURITY_DESCRIPTOR NewDescriptor;
    PSECURITY_DESCRIPTOR CachedDescriptor;
    NTSTATUS Status;
    PKTHREAD CurrentThread;

    PAGED_CODE();

    //
    // In order to preserve some protected fields in the SD (like the SACL) we need to make sure that only one
    // thread updates it at any one time. If we didn't do this another modification could wipe out a SACL
    // an administrator was adding.
    //
    CurrentThread = KeGetCurrentThread ();
    while (1) {

        //
        // Reference the security descriptor
        //

        KeEnterCriticalRegionThread(CurrentThread);
        ExAcquireResourceSharedLite( &IopSecurityResource, TRUE );

        OldDescriptor = DeviceObject->SecurityDescriptor;
        if (OldDescriptor != NULL) {
            ObReferenceSecurityDescriptor( OldDescriptor, 1 );
        }

        ExReleaseResourceLite( &IopSecurityResource );
        KeLeaveCriticalRegionThread(CurrentThread);

        NewDescriptor = OldDescriptor;

        Status = SeSetSecurityDescriptorInfo( NULL,
                                              SecurityInformation,
                                              SecurityDescriptor,
                                              &NewDescriptor,
                                              PoolType,
                                              GenericMapping );
        //
        //  If we successfully set the new security descriptor then we
        //  need to log it in our database and get yet another pointer
        //  to the finally security descriptor
        //
        if ( NT_SUCCESS( Status )) {
            Status = ObLogSecurityDescriptor( NewDescriptor,
                                              &CachedDescriptor,
                                              1 );
            ExFreePool( NewDescriptor );
            if ( NT_SUCCESS( Status )) {
                //
                // Now we need to see if anyone else update this security descriptor inside the
                // gap where we didn't hold the lock. If they did then we just try it all again.
                //
                KeEnterCriticalRegionThread(CurrentThread);
                ExAcquireResourceExclusiveLite( &IopSecurityResource, TRUE );

                if (DeviceObject->SecurityDescriptor == OldDescriptor) {
                    //
                    // Do the swap
                    //
                    DeviceObject->SecurityDescriptor = CachedDescriptor;

                    ExReleaseResourceLite( &IopSecurityResource );
                    KeLeaveCriticalRegionThread(CurrentThread);

                    //
                    // If there was an original object then we need to work out how many
                    // cached references there were (if any) and return them.
                    //
                    ObDereferenceSecurityDescriptor( OldDescriptor, 2 );
                    break;
                } else {

                    ExReleaseResourceLite( &IopSecurityResource );
                    KeLeaveCriticalRegionThread(CurrentThread);

                    ObDereferenceSecurityDescriptor( OldDescriptor, 1 );
                    ObDereferenceSecurityDescriptor( CachedDescriptor, 1);
                }

            } else {

                //
                //  Dereference old SecurityDescriptor
                //

                ObDereferenceSecurityDescriptor( OldDescriptor, 1 );
                break;
            }
        } else {

            //
            //  Dereference old SecurityDescriptor
            //
            if (OldDescriptor != NULL) {
                ObDereferenceSecurityDescriptor( OldDescriptor, 1 );
            }
            break;
        }
    }

    //
    //  And return to our caller
    //

    return( Status );
}


NTSTATUS
IopSetDeviceSecurityDescriptors(
    IN PDEVICE_OBJECT           OriginalDeviceObject,
    IN PDEVICE_OBJECT           DeviceObject,
    IN PSECURITY_INFORMATION    SecurityInformation,
    IN PSECURITY_DESCRIPTOR     SecurityDescriptor,
    IN POOL_TYPE                PoolType,
    IN PGENERIC_MAPPING         GenericMapping
    )
/*++

Routine Description:

    This routine sets the security descriptor on all the devices on the
    device stack for a PNP device. Ideally when the object manager asks the
    IO manager to set the security descriptor of a device object the IO manager
    should set the descriptor only on that device object. This is the classical
    behavior.
    Unfortunately for PNP devices there may be multiple devices on a device
    stack with names.
    If the descriptor is applied to only one of the devices on the stack its
    opens up a security hole as there may be other devices with name on the
    stack which can be opened by a random program.
    To protect against this the descriptor is applied to all device objects on
    the stack.
    Its important that to protect compatibility we need to return the same
    status as what would have been returned if only the requested device
    object's descriptor was set.

Arguments:

    OriginalDeviceObject - Pointer to the device object passed by the object manager
    DeviceObject - pointer to base device object (first one to set)
    SecurityInformation )_ passed directly from IopGetSetSecurityObject
    SecurityDescriptor  )
    PoolType            )
    GenericMapping      )

ReturnValue:

    success, or error while setting the descriptor for the original device object.

--*/
{
    PDEVICE_OBJECT NewDeviceObject = NULL;
    NTSTATUS status;
    NTSTATUS returnStatus = STATUS_SUCCESS;

    ASSERT(DeviceObject);

    PAGED_CODE();

    //
    // pre-reference this object to match the dereference later
    //

    ObReferenceObject( DeviceObject );

    do {

        //
        // Reference the existing security descriptor so it can't be reused.
        // We will only do the final security change if nobody else changes
        // the security while we don't hold the lock. Doing this prevents
        // privileged information being lost like the SACL.
        //

        //
        // Save away and return the device status only for the main device object
        // For example if OldSecurityDescriptor is NULL the IO manager should
        // return STATUS_NO_SECURITY_ON_OBJECT.
        //

        status = IopSetDeviceSecurityDescriptor( DeviceObject,
                                                 SecurityInformation,
                                                 SecurityDescriptor,
                                                 PoolType,
                                                 GenericMapping );


        if (DeviceObject == OriginalDeviceObject) {
            returnStatus = status;
        }


        //
        // We don't need to acquire the database lock because
        // we have a handle to this device stack.
        //

        NewDeviceObject = DeviceObject->AttachedDevice;
        if ( NewDeviceObject != NULL ) {
            ObReferenceObject( NewDeviceObject );
        }

        ObDereferenceObject( DeviceObject );
        DeviceObject = NewDeviceObject;

    } while (NewDeviceObject);

    return returnStatus;
}


NTSTATUS
IopGetSetSecurityObject(
    IN PVOID Object,
    IN SECURITY_OPERATION_CODE OperationCode,
    IN PSECURITY_INFORMATION SecurityInformation,
    IN OUT PSECURITY_DESCRIPTOR SecurityDescriptor,
    IN OUT PULONG CapturedLength,
    IN OUT PSECURITY_DESCRIPTOR *ObjectsSecurityDescriptor,
    IN POOL_TYPE PoolType,
    IN PGENERIC_MAPPING GenericMapping
    )

/*++

Routine Description:

    This routine is invoked to either query or set the security descriptor
    for a file, directory, volume, or device.  It implements these functions
    by either performing an in-line check if the file is a device or a
    volume, or an I/O Request Packet (IRP) is generated and given to the
    driver to perform the operation.

Arguments:

    Object - Pointer to the file or device object representing the open object.

    SecurityInformation - Information about what is being done to or obtained
        from the object's security descriptor.

    SecurityDescriptor - Supplies the base security descriptor and returns
        the final security descriptor.  Note that if this buffer is coming
        from user space, it has already been probed by the object manager
        to length "CapturedLength", otherwise it points to kernel space and
        should not be probed.  It must, however, be referenced in a try
        clause.

    CapturedLength - For a query operation this specifies the size, in
        bytes, of the output security descriptor buffer and on return
        contains the number of bytes needed to store the complete security
        descriptor.  If the length needed is greater than the length
        supplied the operation will fail.  This parameter is ignored for
        the set and delete operations.  It is expected to point into
        system space, ie, it need not be probed and it will not change.

    ObjectsSecurityDescriptor - Supplies and returns the object's security
        descriptor.

    PoolType - Specifies from which type of pool memory is to be allocated.

    GenericMapping - Supplies the generic mapping for the object type.

Return Value:

    The final status of the operation is returned as the function value.

--*/

{
    NTSTATUS status;
    PFILE_OBJECT fileObject;
    PDEVICE_OBJECT deviceObject;
    PDEVICE_OBJECT devicePDO = NULL;
    BOOLEAN synchronousIo;
    PSECURITY_DESCRIPTOR oldSecurityDescriptor, CachedSecurityDescriptor;
    PETHREAD CurrentThread;

    UNREFERENCED_PARAMETER( ObjectsSecurityDescriptor );
    UNREFERENCED_PARAMETER( PoolType );

    PAGED_CODE();


    //
    // Begin by determining whether the security operation is to be performed
    // in this routine or by the driver.  This is based upon whether the
    // object represents a device object, or it represents a file object
    // to a device, or a file on the device. If the open is a direct device
    // open then use the device object.
    //

    if (((PDEVICE_OBJECT) (Object))->Type == IO_TYPE_DEVICE) {
        deviceObject = (PDEVICE_OBJECT) Object;
        fileObject = (PFILE_OBJECT) NULL;
    } else {
        fileObject = (PFILE_OBJECT) Object;
        deviceObject = fileObject->DeviceObject;
    }

    if (!fileObject ||
        (!fileObject->FileName.Length && !fileObject->RelatedFileObject) ||
        (fileObject->Flags & FO_DIRECT_DEVICE_OPEN)) {

        //
        // This security operation is for the device itself, either through
        // a file object, or directly to the device object.  For the latter
        // case, assignment operations are also possible.  Also note that
        // this may be a stream file object, which do not have security.
        // The security for a stream file is actually represented by the
        // security descriptor on the file itself, or the volume, or the
        // device.
        //

        if (OperationCode == AssignSecurityDescriptor) {

            //
            // Simply assign the security descriptor to the device object,
            // if this is a device object.
            //

            status = STATUS_SUCCESS;

            if (fileObject == NULL || !(fileObject->Flags & FO_STREAM_FILE)) {

                status = ObLogSecurityDescriptor( SecurityDescriptor,
                                                  &CachedSecurityDescriptor,
                                                  1 );
                ExFreePool (SecurityDescriptor);
                if (NT_SUCCESS( status )) {

                    CurrentThread = PsGetCurrentThread ();
                    KeEnterCriticalRegionThread(&CurrentThread->Tcb);
                    ExAcquireResourceExclusiveLite( &IopSecurityResource, TRUE );

                    deviceObject->SecurityDescriptor = CachedSecurityDescriptor;

                    ExReleaseResourceLite( &IopSecurityResource );
                    KeLeaveCriticalRegionThread(&CurrentThread->Tcb);
                }
            }

        } else if (OperationCode == SetSecurityDescriptor) {

            //
            // This is a set operation.  The SecurityInformation parameter
            // determines what part of the SecurityDescriptor is going to
            // be applied to the ObjectsSecurityDescriptor.
            //

            //
            // if this deviceObject is attached to a PDO then we want
            // to modify the security on the PDO and apply it up the
            // device chain. This applies to PNP device objects only. See
            // comment in IopSetDeviceSecurityDescriptors
            //
            devicePDO = IopGetDevicePDO(deviceObject);

            if (devicePDO) {

                //
                // set PDO and all attached device objects
                //

                status = IopSetDeviceSecurityDescriptors(
                                deviceObject,
                                devicePDO,
                                SecurityInformation,
                                SecurityDescriptor,
                                PoolType,
                                GenericMapping );

                ObDereferenceObject( devicePDO );

            } else {

                //
                // set this device object only
                //

                status = IopSetDeviceSecurityDescriptor( deviceObject,
                                                         SecurityInformation,
                                                         SecurityDescriptor,
                                                         PoolType,
                                                         GenericMapping );

            }

        } else if (OperationCode == QuerySecurityDescriptor) {

            //
            // This is a get operation.  The SecurityInformation parameter
            // determines what part of the SecurityDescriptor is going to
            // be returned from the ObjectsSecurityDescriptor.
            //

            CurrentThread = PsGetCurrentThread ();
            KeEnterCriticalRegionThread(&CurrentThread->Tcb);
            ExAcquireResourceSharedLite( &IopSecurityResource, TRUE );

            oldSecurityDescriptor = deviceObject->SecurityDescriptor;
            if (oldSecurityDescriptor != NULL) {
                ObReferenceSecurityDescriptor( oldSecurityDescriptor, 1 );
            }

            ExReleaseResourceLite( &IopSecurityResource );
            KeLeaveCriticalRegionThread(&CurrentThread->Tcb);

            status = SeQuerySecurityDescriptorInfo( SecurityInformation,
                                                    SecurityDescriptor,
                                                    CapturedLength,
                                                    &oldSecurityDescriptor );

            if (oldSecurityDescriptor != NULL) {
                ObDereferenceSecurityDescriptor( oldSecurityDescriptor, 1 );
            }

        } else {

            //
            // This is a delete operation.  Simply indicate that everything
            // worked just fine.
            //

            status = STATUS_SUCCESS;

        }

    } else if (OperationCode == DeleteSecurityDescriptor) {

        //
        // This is a delete operation for the security descriptor on a file
        // object.  This function will be performed by the file system once
        // the FCB itself is deleted.  Simply indicate that the operation
        // was successful.
        //

        status = STATUS_SUCCESS;

    } else {

        PIRP irp;
        IO_STATUS_BLOCK localIoStatus;
        KEVENT event;
        PIO_STACK_LOCATION irpSp;
        KPROCESSOR_MODE requestorMode;

        //
        // This file object does not refer to the device itself.  Rather, it
        // refers to either a file or a directory on the device.  This means
        // that the request must be passed to the file system for processing.
        // Note that the only requests that are passed through in this manner
        // are SET or QUERY security operations.  DELETE operations have
        // already been taken care of above since the file system which just
        // drop the storage on the floor when it really needs to, and ASSIGN
        // operations are irrelevant to file systems since they never
        // generate one because they never assign the security descriptor
        // to the object in the first place, they just assign it to the FCB.
        //

        CurrentThread = PsGetCurrentThread ();
        requestorMode = KeGetPreviousModeByThread(&CurrentThread->Tcb);

        //
        // Begin by referencing the object by pointer.   Note that the object
        // handle has already been checked for the appropriate access by the
        // object system caller.  This reference must be performed because
        // standard I/O completion will dereference the object.
        //

        ObReferenceObject( fileObject );

        //
        // Make a special check here to determine whether this is a synchronous
        // I/O operation.  If it is, then wait here until the file is owned by
        // the current thread.  If this is not a (serialized) synchronous I/O
        // operation, then initialize the local event.
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
            KeInitializeEvent( &event, SynchronizationEvent, FALSE );
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
        // Allocate and initialize the I/O Request Packet (IRP) for this
        // operation.  The allocation is performed with an exception handler
        // in case the caller does not have enough quota to allocate the packet.

        irp = IoAllocateIrp( deviceObject->StackSize, !synchronousIo );
        if (!irp) {

            //
            // An IRP could not be allocated.  Cleanup and return an
            // appropriate error status code.
            //

            IopAllocateIrpCleanup( fileObject, (PKEVENT) NULL );

            return STATUS_INSUFFICIENT_RESOURCES;
        }
        irp->Tail.Overlay.OriginalFileObject = fileObject;
        irp->Tail.Overlay.Thread = CurrentThread;
        irp->RequestorMode = requestorMode;

        //
        // Fill in the service independent parameters in the IRP.
        //

        if (fileObject->Flags & FO_SYNCHRONOUS_IO) {
            irp->UserEvent = (PKEVENT) NULL;
        } else {
            irp->UserEvent = &event;
            irp->Flags = IRP_SYNCHRONOUS_API;
        }
        irp->UserIosb = &localIoStatus;
        irp->Overlay.AsynchronousParameters.UserApcRoutine = (PIO_APC_ROUTINE) NULL;

        //
        // Get a pointer to the stack location for the first driver.  This will
        // be used to pass the original function codes and parameters.
        //

        irpSp = IoGetNextIrpStackLocation( irp );

        //
        // Now determine whether this is a set or a query operation.
        //

        if (OperationCode == QuerySecurityDescriptor) {

            //
            // This is a query operation.  Fill in the appropriate fields in
            // the stack location for the packet, as well as the fixed part
            // of the packet.  Note that each of these parameters has been
            // captured as well, so there is no need to perform any probing.
            // The only exception is the UserBuffer memory may change, but
            // that is the file system's responsibility to check.  Note that
            // it has already been probed, so the pointer is at least not
            // in an address space that the caller should not be accessing
            // because of mode.
            //

            irpSp->MajorFunction = IRP_MJ_QUERY_SECURITY;
            irpSp->Parameters.QuerySecurity.SecurityInformation = *SecurityInformation;
            irpSp->Parameters.QuerySecurity.Length = *CapturedLength;
            irp->UserBuffer = SecurityDescriptor;

        } else {

            //
            // This is a set operation.  Fill in the appropriate fields in
            // the stack location for the packet.  Note that access to the
            // SecurityInformation parameter is safe, as the parameter was
            // captured by the caller.  Likewise, the SecurityDescriptor
            // refers to a captured copy of the descriptor.
            //

            irpSp->MajorFunction = IRP_MJ_SET_SECURITY;
            irpSp->Parameters.SetSecurity.SecurityInformation = *SecurityInformation;
            irpSp->Parameters.SetSecurity.SecurityDescriptor = SecurityDescriptor;

        }

        irpSp->FileObject = fileObject;

        //
        // Insert the packet at the head of the IRP list for the thread.
        //

        IopQueueThreadIrp( irp );

        //
        // Update the operation count statistic for the current process for
        // operations other than read and write.
        //

        IopUpdateOtherOperationCount();

        //
        // Everything has been properly set up, so simply invoke the driver.
        //

        status = IoCallDriver( deviceObject, irp );

        //
        // If this operation was a synchronous I/O operation, check the return
        // status to determine whether or not to wait on the file object.  If
        // the file object is to be waited on, wait for the operation to be
        // completed and obtain the final status from the file object itself.
        //

        if (synchronousIo) {
            if (status == STATUS_PENDING) {
                (VOID) KeWaitForSingleObject( &fileObject->Event,
                                              Executive,
                                              KernelMode,
                                              FALSE,
                                              (PLARGE_INTEGER) NULL );
                status = fileObject->FinalStatus;
            }
            IopReleaseFileObjectLock( fileObject );

        } else {

            //
            // This is a normal synchronous I/O operation, as opposed to a
            // serialized synchronous I/O operation.  For this case, wait
            // for the local event and return the final status information
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

        //
        // If this operation was just attempted on a file system or a device
        // driver of some kind that does not implement security, then return
        // a normal null security descriptor.
        //

        if (status == STATUS_INVALID_DEVICE_REQUEST) {

            //
            // The file system does not implement a security policy.  Determine
            // what type of operation this was and implement the correct
            // semantics for the file system.
            //

            if (OperationCode == QuerySecurityDescriptor) {

                //
                // The operation is a query.  If the caller's buffer is too
                // small, then indicate that this is the case and let him know
                // what size buffer is required.  Otherwise, attempt to return
                // a null security descriptor.
                //

               try {
                    status = SeAssignWorldSecurityDescriptor(
                                 SecurityDescriptor,
                                 CapturedLength,
                                 SecurityInformation
                                 );

                } except( EXCEPTION_EXECUTE_HANDLER ) {

                    //
                    // An exception was incurred while attempting to
                    // access the caller's buffer.  Clean everything
                    // up and return an appropriate status code.
                    //

                    status = GetExceptionCode();
                }

            } else {

                //
                // This was an operation other than a query.  Simply indicate
                // that the operation was successful.
                //

                status = STATUS_SUCCESS;
            }

        } else if (OperationCode == QuerySecurityDescriptor) {

            //
            // The final return status from the file system was something
            // other than invalid device request.  This means that the file
            // system actually implemented the query.  Copy the size of the
            // returned data, or the size of the buffer required in order
            // to query the security descriptor.  Note that once again the
            // assignment is performed inside of an exception handler in case
            // the caller's buffer is inaccessible.  Also note that in order
            // for the Information field of the I/O status block to be set,
            // the file system must return a warning status.  Return the
            // status that the caller expects if the buffer really is too
            // small.
            //

            if (status == STATUS_BUFFER_OVERFLOW) {
                status = STATUS_BUFFER_TOO_SMALL;
            }

            try {

                *CapturedLength = (ULONG) localIoStatus.Information;

            } except( EXCEPTION_EXECUTE_HANDLER ) {
                status = GetExceptionCode();
            }
        }
    }

    return status;
}

