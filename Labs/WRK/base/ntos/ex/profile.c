/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    profile.c

Abstract:

   This module implements the executive profile object. Functions are provided
   to create, start, stop, and query profile objects.

--*/

#include "exp.h"

//
// Executive profile object.
//

typedef struct _EPROFILE {
    PKPROCESS Process;
    PVOID RangeBase;
    SIZE_T RangeSize;
    PVOID Buffer;
    ULONG BufferSize;
    ULONG BucketSize;
    PKPROFILE ProfileObject;
    PVOID LockedBufferAddress;
    PMDL Mdl;
    ULONG Segment;
    KPROFILE_SOURCE ProfileSource;
    KAFFINITY Affinity;
} EPROFILE, *PEPROFILE;

//
// Address of event object type descriptor.
//

POBJECT_TYPE ExProfileObjectType;

KMUTEX ExpProfileStateMutex;

#ifdef ALLOC_DATA_PRAGMA
#pragma const_seg("PAGECONST")
#endif

const ULONG ExpCurrentProfileUsage = 0;

#ifdef ALLOC_DATA_PRAGMA
#pragma const_seg("INITCONST")
#endif

const GENERIC_MAPPING ExpProfileMapping = {
    STANDARD_RIGHTS_READ | PROFILE_CONTROL,
    STANDARD_RIGHTS_WRITE | PROFILE_CONTROL,
    STANDARD_RIGHTS_EXECUTE | PROFILE_CONTROL,
    PROFILE_ALL_ACCESS
};

#ifdef ALLOC_DATA_PRAGMA
#pragma const_seg()
#endif

#define ACTIVE_PROFILE_LIMIT 8

#pragma alloc_text(INIT, ExpProfileInitialization)
#pragma alloc_text(PAGE, ExpProfileDelete)
#pragma alloc_text(PAGE, NtCreateProfile)
#pragma alloc_text(PAGE, NtStartProfile)
#pragma alloc_text(PAGE, NtStopProfile)
#pragma alloc_text(PAGE, NtSetIntervalProfile)
#pragma alloc_text(PAGE, NtQueryIntervalProfile)
#pragma alloc_text(PAGE, NtQueryPerformanceCounter)

BOOLEAN
ExpProfileInitialization (
    VOID
    )

/*++

Routine Description:

    This function creates the profile object type descriptor at system
    initialization and stores the address of the object type descriptor
    in global storage.

Arguments:

    None.

Return Value:

    A value of TRUE is returned if the profile object type descriptor is
    successfully initialized. Otherwise a value of FALSE is returned.

--*/

{

    OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
    NTSTATUS Status;
    UNICODE_STRING TypeName;

    //
    // Initialize mutex for synchronizing start and stop operations.
    //

    KeInitializeMutex (&ExpProfileStateMutex, MUTEX_LEVEL_EX_PROFILE);

    //
    // Initialize string descriptor.
    //

    RtlInitUnicodeString(&TypeName, L"Profile");

    //
    // Create event object type descriptor.
    //

    RtlZeroMemory(&ObjectTypeInitializer,sizeof(ObjectTypeInitializer));
    ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
    ObjectTypeInitializer.InvalidAttributes = OBJ_OPENLINK;
    ObjectTypeInitializer.PoolType = NonPagedPool;
    ObjectTypeInitializer.DefaultNonPagedPoolCharge = sizeof(EPROFILE);
    ObjectTypeInitializer.ValidAccessMask = PROFILE_ALL_ACCESS;
    ObjectTypeInitializer.DeleteProcedure = ExpProfileDelete;
    ObjectTypeInitializer.GenericMapping = ExpProfileMapping;

    Status = ObCreateObjectType(&TypeName,
                                &ObjectTypeInitializer,
                                (PSECURITY_DESCRIPTOR)NULL,
                                &ExProfileObjectType);

    //
    // If the event object type descriptor was successfully created, then
    // return a value of TRUE. Otherwise return a value of FALSE.
    //

    return (BOOLEAN)(NT_SUCCESS(Status));
}

VOID
ExpProfileDelete (
    IN PVOID Object
    )

/*++

Routine Description:


    This routine is called by the object management procedures whenever
    the last reference to a profile object has been removed.  This routine
    stops profiling, returns locked buffers and pages, dereferences the
    specified process and returns.

Arguments:

    Object - a pointer to the body of the profile object.

Return Value:

    None.

--*/

{
    PEPROFILE Profile;
    BOOLEAN   State;
    PEPROCESS ProcessAddress;

    Profile = (PEPROFILE)Object;

    if (Profile->LockedBufferAddress != NULL) {

        //
        // Stop profiling and unlock the buffers and deallocate pool.
        //

        State = KeStopProfile (Profile->ProfileObject);
        ASSERT (State != FALSE);

        MmUnmapLockedPages (Profile->LockedBufferAddress, Profile->Mdl);
        MmUnlockPages (Profile->Mdl);
        ExFreePool (Profile->ProfileObject);
    }

    if (Profile->Process != NULL) {
        ProcessAddress = CONTAINING_RECORD(Profile->Process, EPROCESS, Pcb);
        ObDereferenceObject ((PVOID)ProcessAddress);
    }

    return;
}

NTSTATUS
NtCreateProfile (
    __out PHANDLE ProfileHandle,
    __in HANDLE Process OPTIONAL,
    __in PVOID RangeBase,
    __in SIZE_T RangeSize,
    __in ULONG BucketSize,
    __in PULONG Buffer,
    __in ULONG BufferSize,
    __in KPROFILE_SOURCE ProfileSource,
    __in KAFFINITY Affinity
    )

/*++

Routine Description:

    This function creates a profile object.

Arguments:

    ProfileHandle - Supplies a pointer to a variable that will receive
                    the profile object handle.

    Process - Optionally, supplies the handle to the process whose
              address space to profile.  If the value is NULL (0), then
              all address spaces are included in the profile.

    RangeBase - Supplies the address of the first byte of the address
                  space for which profiling information is to be collected.


    RangeSize - Supplies the size of the range to profile in the
                address space.  RangeBase and RangeSize are interpreted
                such that RangeBase <= address < RangeBase+RangeSize
                will generate a profile hit.

    BucketSize - Supplies the LOG base 2 of the size of the profiling
                 bucket.  Thus, BucketSize = 2 yields four-byte
                 buckets, BucketSize = 7 yields 128-byte buckets.
                 All profile hits in a given bucket will increment
                 the corresponding counter in Buffer.  Buckets
                 cannot be smaller than a ULONG.  The acceptable range
                 of this value is 2 to 30 inclusive.

    Buffer - Supplies an array of ULONGs.  Each ULONG is a hit counter,
             which records the number of hits of the corresponding
             bucket.

    BufferSize - Size in bytes of Buffer.

    ProfileSource - Supplies the source for the profile interrupt

    Affinity - Supplies the processor set for the profile interrupt

--*/

{

    PEPROFILE Profile;
    HANDLE Handle;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;
    PEPROCESS ProcessAddress;
    OBJECT_ATTRIBUTES ObjectAttributes;
    BOOLEAN HasPrivilege = FALSE;
    ULONG Segment = FALSE;
#ifdef i386
    USHORT PowerOf2;
#endif

    //
    // Verify that the base and size arguments are reasonable.
    //

    if (BufferSize == 0) {
        return STATUS_INVALID_PARAMETER_7;
    }

    //
    //        Improper use of bucket size.  If bucket size is zero, and
    //        RangeBase < 64K, then create a profile object to attach
    //        to a non-flat code segment.  In this case, RangeBase is
    //        the non-flat CS for this profile object.
    //

#ifdef i386

    if ((BucketSize == 0) && (RangeBase < (PVOID)(64 * 1024))) {

        if (BufferSize < sizeof(ULONG)) {
            return STATUS_INVALID_PARAMETER_7;
        }

        Segment = (ULONG)RangeBase;
        RangeBase = 0;
        BucketSize = RangeSize / (BufferSize / sizeof(ULONG));

        //
        // Convert Bucket size of log2(BucketSize)
        //
        PowerOf2 = 0;
        BucketSize = BucketSize - 1;
        while (BucketSize >>= 1) {
            PowerOf2++;
        }

        BucketSize = PowerOf2 + 1;

        if (BucketSize < 2) {
            BucketSize = 2;
        }
    }

#endif

    if ((BucketSize > 31) || (BucketSize < 2)) {
        return STATUS_INVALID_PARAMETER;
    }

    if ((RangeSize >> (BucketSize - 2)) > BufferSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (((ULONG_PTR)RangeBase + RangeSize) < RangeSize) {
        return STATUS_BUFFER_OVERFLOW;
    }

    //
    // Establish an exception handler, probe the output handle address, and
    // attempt to create a profile object. If the probe fails, then return the
    // exception code as the service status. Otherwise return the status value
    // returned by the object insertion routine.
    //

    try {
        //
        // Get previous processor mode and probe output handle address if
        // necessary.
        //

        PreviousMode = KeGetPreviousMode ();

        if (PreviousMode != KernelMode) {
            ProbeForWriteHandle(ProfileHandle);

            ProbeForWrite(Buffer,
                          BufferSize,
                          sizeof(ULONG));
        }

    //
    // If an exception occurs during the probe of the output handle address,
    // then always handle the exception and return the exception code as the
    // status value.
    //

    } except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    if (!ARGUMENT_PRESENT(Process)) {

        //
        // Don't attach segmented profile objects to all processes
        //

        if (Segment) {
            return STATUS_INVALID_PARAMETER;
        }

        //
        // Profile all processes. Make sure that the specified
        // address range is in system space, unless SeSystemProfilePrivilege.
        //

        if (RangeBase <= MM_HIGHEST_USER_ADDRESS) {

            //
            // Check for privilege before allowing a user to profile
            // all processes and USER addresses.
            //

            if (PreviousMode != KernelMode) {
                HasPrivilege =  SeSinglePrivilegeCheck(
                                    SeSystemProfilePrivilege,
                                    PreviousMode
                                    );

                if (!HasPrivilege) {
#if DBG
                    DbgPrint("SeSystemProfilePrivilege needed to profile all USER addresses.\n");
#endif //DBG
                    return( STATUS_PRIVILEGE_NOT_HELD );
                }

            }
        }

        ProcessAddress = NULL;


    } else {

        //
        // Reference the specified process.
        //

        Status = ObReferenceObjectByHandle ( Process,
                                             PROCESS_QUERY_INFORMATION,
                                             PsProcessType,
                                             PreviousMode,
                                             (PVOID *)&ProcessAddress,
                                             NULL );

        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    InitializeObjectAttributes( &ObjectAttributes,
                                NULL,
                                OBJ_EXCLUSIVE,
                                NULL,
                                NULL );

    Status = ObCreateObject( KernelMode,
                             ExProfileObjectType,
                             &ObjectAttributes,
                             PreviousMode,
                             NULL,
                             sizeof(EPROFILE),
                             0,
                             sizeof(EPROFILE) + sizeof(KPROFILE),
                             (PVOID *)&Profile);

    //
    // If the profile object was successfully allocated, initialize
    // the profile object.
    //
    if (NT_SUCCESS(Status)) {


        if (ProcessAddress != NULL) {
            Profile->Process = &ProcessAddress->Pcb;
        } else {
            Profile->Process = NULL;
        }

        Profile->RangeBase = RangeBase;
        Profile->RangeSize = RangeSize;
        Profile->Buffer = Buffer;
        Profile->BufferSize = BufferSize;
        Profile->BucketSize = BucketSize;
        Profile->LockedBufferAddress = NULL;
        Profile->Segment = Segment;
        Profile->ProfileSource = ProfileSource;
        Profile->Affinity = Affinity;

        Status = ObInsertObject(Profile,
                                NULL,
                                PROFILE_CONTROL,
                                0,
                                (PVOID *)NULL,
                                &Handle);
        //
        // If the profile object was successfully inserted in the current
        // process' handle table, then attempt to write the profile object
        // handle value. If the write attempt fails, then do not report
        // an error. When the caller attempts to access the handle value,
        // an access violation will occur.
        //
        if (NT_SUCCESS(Status)) {
            try {
                *ProfileHandle = Handle;
            } except(EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    } else {
        //
        // We failed, remove our reference to the process object.
        //

        if (ProcessAddress != NULL) {
            ObDereferenceObject (ProcessAddress);
        }
    }

    //
    // Return service status.
    //

    return Status;
}

NTSTATUS
NtStartProfile (
    __in HANDLE ProfileHandle
    )

/*++

Routine Description:

    The NtStartProfile routine starts the collecting data for the
    specified profile object.  This involved allocating nonpaged
    pool to lock the specified buffer in memory, creating a kernel
    profile object and starting collecting on that profile object.

Arguments:

    ProfileHandle - Supplies the profile handle to start profiling on.

--*/

{

    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;
    PEPROFILE Profile;
    PKPROFILE ProfileObject;
    PVOID LockedVa;
    BOOLEAN State;
    PMDL Mdl;

    PreviousMode = KeGetPreviousMode();

    Status = ObReferenceObjectByHandle (ProfileHandle,
                                        PROFILE_CONTROL,
                                        ExProfileObjectType,
                                        PreviousMode,
                                        &Profile,
                                        NULL);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    //
    // Acquire the profile state mutex so two threads can't
    // operate on the same profile object simultaneously.
    //

    KeWaitForSingleObject (&ExpProfileStateMutex,
                           Executive,
                           KernelMode,
                           FALSE,
                           (PLARGE_INTEGER)NULL);

    //
    // Make sure profiling is not already enabled.
    //

    if (Profile->LockedBufferAddress != NULL) {
        KeReleaseMutex (&ExpProfileStateMutex, FALSE);
        ObDereferenceObject (Profile);
        return STATUS_PROFILING_NOT_STOPPED;
    }

    if (ExpCurrentProfileUsage == ACTIVE_PROFILE_LIMIT) {
        KeReleaseMutex (&ExpProfileStateMutex, FALSE);
        ObDereferenceObject (Profile);
        return STATUS_PROFILING_AT_LIMIT;
    }

    ProfileObject = ExAllocatePoolWithTag (NonPagedPool,
                                    MmSizeOfMdl(Profile->Buffer,
                                                Profile->BufferSize) +
                                        sizeof(KPROFILE),
                                        'forP');

    if (ProfileObject == NULL) {
        KeReleaseMutex (&ExpProfileStateMutex, FALSE);
        ObDereferenceObject (Profile);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Mdl = (PMDL)(ProfileObject + 1);
    Profile->Mdl = Mdl;
    Profile->ProfileObject = ProfileObject;

    //
    // Probe and lock the specified buffer.
    //

    MmInitializeMdl(Mdl, Profile->Buffer, Profile->BufferSize);

    LockedVa = NULL;

    try {

        MmProbeAndLockPages (Mdl,
                             PreviousMode,
                             IoWriteAccess );

    } except (EXCEPTION_EXECUTE_HANDLER) {

        KeReleaseMutex (&ExpProfileStateMutex, FALSE);
        ExFreePool (ProfileObject);
        ObDereferenceObject (Profile);
        return GetExceptionCode();
    }

    //
    // Since kernel space is specified below, this call cannot raise
    // an exception.
    //

    LockedVa = MmMapLockedPagesSpecifyCache (Profile->Mdl,
                                             KernelMode,
                                             MmCached,
                                             NULL,
                                             FALSE,
                                             NormalPagePriority);

    if (LockedVa == NULL) {
        KeReleaseMutex (&ExpProfileStateMutex, FALSE);

        MmUnlockPages (Mdl);
        ExFreePool (ProfileObject);
        ObDereferenceObject (Profile);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Initialize the profile object.
    //

    KeInitializeProfile (ProfileObject,
                         Profile->Process,
                         Profile->RangeBase,
                         Profile->RangeSize,
                         Profile->BucketSize,
                         Profile->Segment,
                         Profile->ProfileSource,
                         Profile->Affinity);

    State = KeStartProfile (ProfileObject, LockedVa);
    ASSERT (State != FALSE);

    Profile->LockedBufferAddress = LockedVa;

    KeReleaseMutex (&ExpProfileStateMutex, FALSE);
    ObDereferenceObject (Profile);

    return STATUS_SUCCESS;
}

NTSTATUS
NtStopProfile (
    __in HANDLE ProfileHandle
    )

/*++

Routine Description:

    The NtStopProfile routine stops collecting data for the
    specified profile object.  This involves stopping the data
    collection on the profile object, unlocking the locked buffers,
    and deallocating the pool for the MDL and profile object.

Arguments:

    ProfileHandle - Supplies a the profile handle to stop profiling.

--*/

{

    PEPROFILE Profile;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;
    BOOLEAN State;
    PKPROFILE ProfileObject;
    PMDL Mdl;
    PVOID LockedBufferAddress;

    PreviousMode = KeGetPreviousMode();

    Status = ObReferenceObjectByHandle( ProfileHandle,
                                        PROFILE_CONTROL,
                                        ExProfileObjectType,
                                        PreviousMode,
                                        (PVOID *)&Profile,
                                        NULL);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    KeWaitForSingleObject( &ExpProfileStateMutex,
                           Executive,
                           KernelMode,
                           FALSE,
                           (PLARGE_INTEGER)NULL);

    //
    // Check to see if profiling is not active.
    //

    if (Profile->LockedBufferAddress == NULL) {
        KeReleaseMutex (&ExpProfileStateMutex, FALSE);
        ObDereferenceObject (Profile);
        return STATUS_PROFILING_NOT_STARTED;
    }

    //
    // Stop profiling and unlock the buffer.
    //

    State = KeStopProfile (Profile->ProfileObject);
    ASSERT (State != FALSE);

    LockedBufferAddress = Profile->LockedBufferAddress;
    Profile->LockedBufferAddress = NULL;
    Mdl = Profile->Mdl;
    ProfileObject = Profile->ProfileObject;
    KeReleaseMutex (&ExpProfileStateMutex, FALSE);

    MmUnmapLockedPages (LockedBufferAddress, Mdl);
    MmUnlockPages (Mdl);
    ExFreePool (ProfileObject);
    ObDereferenceObject (Profile);
    return STATUS_SUCCESS;
}

NTSTATUS
NtSetIntervalProfile (
    __in ULONG Interval,
    __in KPROFILE_SOURCE Source
    )

/*++

Routine Description:

    This routine allows the system-wide interval (and thus the profiling
    rate) for profiling to be set.

Arguments:

    Interval - Supplies the sampling interval in 100ns units.

    Source - Specifies the profile source to be set.

--*/

{

    KeSetIntervalProfile (Interval, Source);
    return STATUS_SUCCESS;
}

NTSTATUS
NtQueryIntervalProfile (
    __in KPROFILE_SOURCE ProfileSource,
    __out PULONG Interval
    )

/*++

Routine Description:

    This routine queries the system-wide interval (and thus the profiling
    rate) for profiling.

Arguments:

    Source - Specifies the profile source to be queried.

    Interval - Returns the sampling interval in 100ns units.

--*/

{
    ULONG CapturedInterval;
    KPROCESSOR_MODE PreviousMode;

    PreviousMode = KeGetPreviousMode ();
    if (PreviousMode != KernelMode) {

        //
        // Probe accessibility of user's buffer.
        //

        try {
            ProbeForWriteUlong (Interval);

        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            // If an exception occurs during the probe or capture
            // of the initial values, then handle the exception and
            // return the exception code as the status value.
            //

            return GetExceptionCode();
        }
    }

    CapturedInterval = KeQueryIntervalProfile (ProfileSource);

    if (PreviousMode != KernelMode) {
        try {
            *Interval = CapturedInterval;

        } except (EXCEPTION_EXECUTE_HANDLER) {
            NOTHING;
        }
    }
    else {
        *Interval = CapturedInterval;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NtQueryPerformanceCounter (
    __out PLARGE_INTEGER PerformanceCounter,
    __out_opt PLARGE_INTEGER PerformanceFrequency
    )

/*++

Routine Description:

    This function returns current value of performance counter and,
    optionally, the frequency of the performance counter.

    Performance frequency is the frequency of the performance counter
    in Hertz, i.e., counts/second.  Note that this value is implementation
    dependent.  If the implementation does not have hardware to support
    performance timing, the value returned is 0.

Arguments:

    PerformanceCounter - supplies the address of a variable to receive
        the current Performance Counter value.

    PerformanceFrequency - Optionally, supplies the address of a
        variable to receive the performance counter frequency.

Return Value:

    STATUS_ACCESS_VIOLATION or STATUS_SUCCESS.

--*/

{
    KPROCESSOR_MODE PreviousMode;
    LARGE_INTEGER KernelPerformanceFrequency;

    PreviousMode = KeGetPreviousMode();
    if (PreviousMode != KernelMode) {

        //
        // Probe accessibility of user's buffer.
        //

        try {
            ProbeForWriteSmallStructure (PerformanceCounter,
                                         sizeof (LARGE_INTEGER),
                                         sizeof (ULONG));

            if (ARGUMENT_PRESENT(PerformanceFrequency)) {
                ProbeForWriteSmallStructure (PerformanceFrequency,
                                             sizeof (LARGE_INTEGER),
                                             sizeof (ULONG));
            }

            *PerformanceCounter = KeQueryPerformanceCounter (&KernelPerformanceFrequency);

            if (ARGUMENT_PRESENT(PerformanceFrequency)) {
                *PerformanceFrequency = KernelPerformanceFrequency;
            }

        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            // If an exception occurs during the probe or capture
            // of the initial values, then handle the exception and
            // return the exception code as the status value.
            //

            return GetExceptionCode();
        }

    } else {
        *PerformanceCounter = KeQueryPerformanceCounter (&KernelPerformanceFrequency);
        if (ARGUMENT_PRESENT(PerformanceFrequency)) {
            *PerformanceFrequency = KernelPerformanceFrequency;
        }
    }

    return STATUS_SUCCESS;
}

