/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    threadobj.c

Abstract:

    This module implements the machine independent functions to manipulate
    the kernel thread object. Functions are provided to initialize, ready,
    alert, test alert, boost priority, enable APC queuing, disable APC
    queuing, confine, set affinity, set priority, suspend, resume, alert
    resume, terminate, read thread state, freeze, unfreeze, query data
    alignment handling mode, force resume, and enter and leave critical
    regions for thread objects.

--*/

#include "ki.h"

#pragma alloc_text(INIT, KeInitializeThread)
#pragma alloc_text(PAGE, KeInitThread)
#pragma alloc_text(PAGE, KeUninitThread)

NTSTATUS
KeInitThread (
    __out PKTHREAD Thread,
    __in_opt PVOID KernelStack,
    __in PKSYSTEM_ROUTINE SystemRoutine,
    __in_opt PKSTART_ROUTINE StartRoutine,
    __in_opt PVOID StartContext,
    __in_opt PCONTEXT ContextFrame,
    __in_opt PVOID Teb,
    __in PKPROCESS Process
    )

/*++

Routine Description:

    This function initializes a thread object. The priority, affinity,
    and initial quantum are taken from the parent process object.

    N.B. This routine is carefully written so that if an access violation
        occurs while reading the specified context frame, then no kernel
        data structures will have been modified. It is the responsibility
        of the caller to handle the exception and provide necessary clean
        up.

    N.B. It is assumed that the thread object is zeroed.

Arguments:

    Thread - Supplies a pointer to a dispatcher object of type thread.

    KernelStack - Supplies a pointer to the base of a kernel stack on which
        the context frame for the thread is to be constructed.

    SystemRoutine - Supplies a pointer to the system function that is to be
        called when the thread is first scheduled for execution.

    StartRoutine - Supplies an optional pointer to a function that is to be
        called after the system has finished initializing the thread. This
        parameter is specified if the thread is a system thread and will
        execute totally in kernel mode.

    StartContext - Supplies an optional pointer to an arbitrary data structure
        which will be passed to the StartRoutine as a parameter. This
        parameter is specified if the thread is a system thread and will
        execute totally in kernel mode.

    ContextFrame - Supplies an optional pointer a context frame which contains
        the initial user mode state of the thread. This parameter is specified
        if the thread is a user thread and will execute in user mode. If this
        parameter is not specified, then the Teb parameter is ignored.

    Teb - Supplies an optional pointer to the user mode thread environment
        block. This parameter is specified if the thread is a user thread and
        will execute in user mode. This parameter is ignored if the ContextFrame
        parameter is not specified.

    Process - Supplies a pointer to a control object of type process.

Return Value:

    None.

--*/

{

    LONG Index;
    BOOLEAN KernelStackAllocated = FALSE;
    PKTIMER Timer;
    PKWAIT_BLOCK WaitBlock;

    //
    // Initialize the standard dispatcher object header and set the initial
    // state of the thread object.
    //

    Thread->Header.Type = ThreadObject;
    Thread->Header.Size = sizeof(KTHREAD) / sizeof(LONG);
    InitializeListHead(&Thread->Header.WaitListHead);

    //
    // Initialize the owned mutant listhead.
    //

    InitializeListHead(&Thread->MutantListHead);

    //
    // Initialize the thread field of all builtin wait blocks.
    //

    for (Index = 0; Index < (THREAD_WAIT_OBJECTS + 1); Index += 1) {
        Thread->WaitBlock[Index].Thread = Thread;
    }

    //
    // Initialize the alerted, preempted, debugactive, autoalignment,
    // kernel stack resident, enable kernel stack swap, and process
    // ready queue boolean values.
    //
    // N.B. Only nonzero values are initialized.
    //

    Thread->AutoAlignment = Process->AutoAlignment;
    Thread->EnableStackSwap = TRUE;
    Thread->KernelStackResident = TRUE;
    Thread->SwapBusy = FALSE;

    //
    // Initialize the thread lock and priority adjustment reason.
    //

    KeInitializeSpinLock(&Thread->ThreadLock);
    Thread->AdjustReason = AdjustNone;

    //
    // Set the system service table pointer to the address of the static
    // system service descriptor table. If the thread is later converted
    // to a Win32 thread this pointer will be change to a pointer to the
    // shadow system service descriptor table.
    //

#if defined(_AMD64_)

    Thread->ServiceTable = KeServiceDescriptorTable[SYSTEM_SERVICE_INDEX].Base;
    Thread->KernelLimit = KeServiceDescriptorTable[SYSTEM_SERVICE_INDEX].Limit;

#else

    Thread->ServiceTable = (PVOID)&KeServiceDescriptorTable[0];

#endif

    //
    // Initialize the APC state pointers, the current APC state, the saved
    // APC state, and enable APC queuing.
    //

    Thread->ApcStatePointer[0] = &Thread->ApcState;
    Thread->ApcStatePointer[1] = &Thread->SavedApcState;
    InitializeListHead(&Thread->ApcState.ApcListHead[KernelMode]);
    InitializeListHead(&Thread->ApcState.ApcListHead[UserMode]);
    Thread->ApcState.Process = Process;
    Thread->Process = Process;
    Thread->ApcQueueable = TRUE;

    //
    // Initialize the kernel mode suspend APC and the suspend semaphore object.
    // and the builtin wait timeout timer object.
    //

    KeInitializeApc(&Thread->SuspendApc,
                    Thread,
                    OriginalApcEnvironment,
                    (PKKERNEL_ROUTINE)KiSuspendNop,
                    (PKRUNDOWN_ROUTINE)KiSuspendRundown,
                    KiSuspendThread,
                    KernelMode,
                    NULL);

    KeInitializeSemaphore(&Thread->SuspendSemaphore, 0L, 2L);

    //
    // Initialize the builtin timer wait block.
    //
    // N.B. This is the only time the wait block is initialized since this
    //      information is constant.
    //

    Timer = &Thread->Timer;
    KeInitializeTimer(Timer);
    WaitBlock = &Thread->WaitBlock[TIMER_WAIT_BLOCK];
    WaitBlock->Object = Timer;
    WaitBlock->WaitKey = (CSHORT)STATUS_TIMEOUT;
    WaitBlock->WaitType = WaitAny;
    WaitBlock->WaitListEntry.Flink = &Timer->Header.WaitListHead;
    WaitBlock->WaitListEntry.Blink = &Timer->Header.WaitListHead;

    //
    // Initialize the APC queue spinlock.
    //

    KeInitializeSpinLock(&Thread->ApcQueueLock);

    //
    // Initialize the Thread Environment Block (TEB) pointer (can be NULL).
    //

    Thread->Teb = Teb;

    //
    // Allocate a kernel stack if necessary and set the initial kernel stack,
    // stack base, and stack limit.
    //

    if (KernelStack == NULL) {
        KernelStack = MmCreateKernelStack(FALSE, Process->IdealNode);
        if (KernelStack == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        KernelStackAllocated = TRUE;
    }

    Thread->InitialStack = KernelStack;
    Thread->StackBase = KernelStack;
    Thread->StackLimit = (PVOID)((ULONG_PTR)KernelStack - KERNEL_STACK_SIZE);

    //
    // Initialize the thread context.
    //

    try {
        KiInitializeContextThread(Thread,
                                  SystemRoutine,
                                  StartRoutine,
                                  StartContext,
                                  ContextFrame);

    } except (EXCEPTION_EXECUTE_HANDLER) {
        if (KernelStackAllocated) {
            MmDeleteKernelStack(Thread->StackBase, FALSE);
            Thread->InitialStack = NULL;
        }

        return GetExceptionCode();
    }

    //
    // Set the base thread priority, the thread priority, the thread affinity,
    // the thread quantum, and the scheduling state.
    //

    Thread->State = Initialized;
    return STATUS_SUCCESS;
}

VOID
KeUninitThread (
    __inout PKTHREAD Thread
    )

/*++

Routine Description:

    This function frees the thread kernel stack and must be called before
    the thread is started.

Arguments:

    Thread - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    None.
--*/

{

    MmDeleteKernelStack(Thread->StackBase, FALSE);
    Thread->InitialStack = NULL;
    return;
}

VOID
KeStartThread (
    __inout PKTHREAD Thread
    )

/*++

Routine Description:

    This function initializes remaining thread fields and inserts the thread
    in the thread's process list. From this point on the thread must run.

Arguments:

    Thread - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    None.

--*/

{

#if defined(_AMD64_)

    KLOCK_QUEUE_HANDLE ListHandle;

#endif

    KLOCK_QUEUE_HANDLE LockHandle;
    PKPROCESS Process;

#if !defined(NT_UP)

    ULONG IdealProcessor;
    KAFFINITY PreferredSet;

#if defined(NT_SMT)

    KAFFINITY TempSet;

#endif

#endif

    //
    // Set thread disable boost and IOPL.
    //

    Process = Thread->ApcState.Process;
    Thread->DisableBoost = Process->DisableBoost;

#if defined(_X86_)

    Thread->Iopl = Process->Iopl;

#endif

    //
    // Initialize the thread quantum and set system affinity false.
    //

    Thread->Quantum = Process->QuantumReset;
    Thread->QuantumReset = Process->QuantumReset;
    Thread->SystemAffinityActive = FALSE;

    //
    // Raise IRQL to SYNCH_LEVEL and acquire the process lock.
    //

    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Process->ProcessLock, &LockHandle);

    //
    // Set the thread priority and affinity.
    //

    Thread->BasePriority = Process->BasePriority;
    Thread->Priority = Thread->BasePriority;
    Thread->Affinity = Process->Affinity;
    Thread->UserAffinity = Process->Affinity;

    //
    // Initialize the ideal processor number and node for the thread.
    //
    // N.B. It is guaranteed that the process affinity intersects the process
    //      ideal node affinity.
    //

#if defined(NT_UP)

    Thread->IdealProcessor = 0;
    Thread->UserIdealProcessor = 0;

#else

    //
    // Initialize the ideal processor number.
    //
    // N.B. It is guaranteed that the process affinity intersects the process
    //      ideal node affinity.
    //
    // N.B. The preferred set, however, must be reduced by the process affinity.
    //

    IdealProcessor = Process->ThreadSeed;
    PreferredSet = KeNodeBlock[Process->IdealNode]->ProcessorMask & Process->Affinity;

    //
    // If possible bias the ideal processor to a different SMT set than the
    // last thread.
    //

#if defined(NT_SMT)

    TempSet = ~KiProcessorBlock[IdealProcessor]->MultiThreadProcessorSet;
    if ((PreferredSet & TempSet) != 0) {
        PreferredSet &= TempSet;
    }

#endif

    //
    // Find an ideal processor for thread and update the process thread seed.
    //

    IdealProcessor = KeFindNextRightSetAffinity(IdealProcessor, PreferredSet);
    Process->ThreadSeed = (UCHAR)IdealProcessor;

    ASSERT((Thread->UserAffinity & AFFINITY_MASK(IdealProcessor)) != 0);

    Thread->UserIdealProcessor = (UCHAR)IdealProcessor;
    Thread->IdealProcessor = (UCHAR)IdealProcessor;

#endif

    //
    // If the thread is the first entry in the process thread list and the
    // process is not the idle process, then insert the process in the kernel
    // process list.
    //

#if defined(_AMD64_)

    if ((IsListEmpty(&Process->ThreadListHead) == TRUE) &&
        (Process != &KiInitialProcess.Pcb)) {

        KeAcquireInStackQueuedSpinLockAtDpcLevel(&KiProcessListLock,
                                                 &ListHandle);

        InsertTailList(&KiProcessListHead, &Process->ProcessListEntry);
        KeReleaseInStackQueuedSpinLockFromDpcLevel(&ListHandle);
    }

#endif

    //
    // Lock the dispatcher database.
    //

    KiLockDispatcherDatabaseAtSynchLevel();

    //
    // Insert the thread in the process thread list and increment the kernel
    // stack count.
    //
    // N.B. The process is not swappable until its first thread has been
    //      initialized.
    //

    InsertTailList(&Process->ThreadListHead, &Thread->ThreadListEntry);

    ASSERT(Process->StackCount != MAXULONG_PTR);

    Process->StackCount += 1;

    //
    // Unlock the dispatcher database, release the process lock, and lower
    // IRQL to its previous value.
    //

    KiUnlockDispatcherDatabaseFromSynchLevel();
    KeReleaseInStackQueuedSpinLock(&LockHandle);
    return;
}

NTSTATUS
KeInitializeThread (
    __out PKTHREAD Thread,
    __in_opt PVOID KernelStack,
    __in PKSYSTEM_ROUTINE SystemRoutine,
    __in_opt PKSTART_ROUTINE StartRoutine,
    __in_opt PVOID StartContext,
    __in_opt PCONTEXT ContextFrame,
    __in_opt PVOID Teb,
    __in PKPROCESS Process
    )

/*++

Routine Description:

    This function initializes a thread object. The priority, affinity,
    and initial quantum are taken from the parent process object. The
    thread object is inserted at the end of the thread list for the
    parent process.

    N.B. This routine is carefully written so that if an access violation
        occurs while reading the specified context frame, then no kernel
        data structures will have been modified. It is the responsibility
        of the caller to handle the exception and provide necessary clean
        up.

    N.B. It is assumed that the thread object is zeroed.

Arguments:

    Thread - Supplies a pointer to a dispatcher object of type thread.

    KernelStack - Supplies a pointer to the base of a kernel stack on which
        the context frame for the thread is to be constructed.

    SystemRoutine - Supplies a pointer to the system function that is to be
        called when the thread is first scheduled for execution.

    StartRoutine - Supplies an optional pointer to a function that is to be
        called after the system has finished initializing the thread. This
        parameter is specified if the thread is a system thread and will
        execute totally in kernel mode.

    StartContext - Supplies an optional pointer to an arbitrary data structure
        which will be passed to the StartRoutine as a parameter. This
        parameter is specified if the thread is a system thread and will
        execute totally in kernel mode.

    ContextFrame - Supplies an optional pointer a context frame which contains
        the initial user mode state of the thread. This parameter is specified
        if the thread is a user thread and will execute in user mode. If this
        parameter is not specified, then the Teb parameter is ignored.

    Teb - Supplies an optional pointer to the user mode thread environment
        block. This parameter is specified if the thread is a user thread and
        will execute in user mode. This parameter is ignored if the ContextFrame
        parameter is not specified.

    Process - Supplies a pointer to a control object of type process.

Return Value:

    NTSTATUS - Status of operation

--*/

{

    NTSTATUS Status;

    Status = KeInitThread(Thread,
                          KernelStack,
                          SystemRoutine,
                          StartRoutine,
                          StartContext,
                          ContextFrame,
                          Teb,
                          Process);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    KeStartThread(Thread);
    return STATUS_SUCCESS;
}

BOOLEAN
KeAlertThread (
    __inout PKTHREAD Thread,
    __in KPROCESSOR_MODE AlertMode
    )

/*++

Routine Description:

    This function attempts to alert a thread and cause its execution to
    be continued if it is currently in an alertable Wait state. Otherwise
    it just sets the alerted variable for the specified processor mode.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

    AlertMode - Supplies the processor mode for which the thread is
        to be alerted.

Return Value:

    The previous state of the alerted variable for the specified processor
    mode.

--*/

{

    BOOLEAN Alerted;
    KLOCK_QUEUE_HANDLE LockHandle;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to SYNCH_LEVEL, acquire the thread APC queue lock, and lock
    // the dispatcher database.
    //

    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Thread->ApcQueueLock, &LockHandle);
    KiLockDispatcherDatabaseAtSynchLevel();

    //
    // Capture the current state of the alerted variable for the specified
    // processor mode.
    //

    Alerted = Thread->Alerted[AlertMode];

    //
    // If the alerted state for the specified processor mode is Not-Alerted,
    // then attempt to alert the thread.
    //

    if (Alerted == FALSE) {

        //
        // If the thread is currently in a Wait state, the Wait is alertable,
        // and the specified processor mode is less than or equal to the Wait
        // mode, then the thread is unwaited with a status of "alerted".
        //

        if ((Thread->State == Waiting) && (Thread->Alertable == TRUE) &&
            (AlertMode <= Thread->WaitMode)) {
            KiUnwaitThread(Thread, STATUS_ALERTED, ALERT_INCREMENT);

        } else {
            Thread->Alerted[AlertMode] = TRUE;
        }
    }

    //
    // Unlock the dispatcher database from SYNCH_LEVEL, release the thread
    // APC queue lock, exit the scheduler, and return the previous alerted
    // state for the specified mode.
    //

    KiUnlockDispatcherDatabaseFromSynchLevel();
    KeReleaseInStackQueuedSpinLockFromDpcLevel(&LockHandle);
    KiExitDispatcher(LockHandle.OldIrql);
    return Alerted;
}

ULONG
KeAlertResumeThread (
    __inout PKTHREAD Thread
    )

/*++

Routine Description:

    This function attempts to alert a thread in kernel mode and cause its
    execution to be continued if it is currently in an alertable Wait state.
    In addition, a resume operation is performed on the specified thread.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    The previous suspend count.

--*/

{

    KLOCK_QUEUE_HANDLE LockHandle;
    ULONG OldCount;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to SYNCH_LEVEL, acquire the thread APC queue lock, and lock
    // the dispatcher database.
    //

    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Thread->ApcQueueLock, &LockHandle);
    KiLockDispatcherDatabaseAtSynchLevel();

    //
    // If the kernel mode alerted state is FALSE, then attempt to alert
    // the thread for kernel mode.
    //

    if (Thread->Alerted[KernelMode] == FALSE) {

        //
        // If the thread is currently in a Wait state and the Wait is alertable,
        // then the thread is unwaited with a status of "alerted". Else set the
        // kernel mode alerted variable.
        //

        if ((Thread->State == Waiting) && (Thread->Alertable == TRUE)) {
            KiUnwaitThread(Thread, STATUS_ALERTED, ALERT_INCREMENT);

        } else {
            Thread->Alerted[KernelMode] = TRUE;
        }
    }

    //
    // Capture the current suspend count.
    //

    OldCount = Thread->SuspendCount;

    //
    // If the thread is currently suspended, then decrement its suspend count.
    //

    if (OldCount != 0) {
        Thread->SuspendCount -= 1;

        //
        // If the resultant suspend count is zero and the freeze count is
        // zero, then resume the thread by releasing its suspend semaphore.
        //

        if ((Thread->SuspendCount == 0) && (Thread->FreezeCount == 0)) {
            Thread->SuspendSemaphore.Header.SignalState += 1;
            KiWaitTest(&Thread->SuspendSemaphore, RESUME_INCREMENT);
        }
    }

    //
    // Unlock the dispatcher database from SYNCH_LEVEL, release the thread
    // APC queue lock, exit the scheduler, and return the previous suspend
    // count.
    //

    KiUnlockDispatcherDatabaseFromSynchLevel();
    KeReleaseInStackQueuedSpinLockFromDpcLevel(&LockHandle);
    KiExitDispatcher(LockHandle.OldIrql);
    return OldCount;
}

VOID
KeBoostPriorityThread (
    __inout PKTHREAD Thread,
    __in KPRIORITY Increment
    )

/*++

Routine Description:

    This function boosts the priority of the specified thread using the
    same algorithm used when a thread gets a boost from a wait operation.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

    Increment - Supplies the priority increment that is to be applied to
        the thread's priority.

Return Value:

    None.

--*/

{

    KIRQL OldIrql;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to dispatcher level and lock dispatcher database.
    //

    KiLockDispatcherDatabase(&OldIrql);

    //
    // If the thread does not run at a realtime priority level, then boost
    // the thread priority.
    //

    if (Thread->Priority < LOW_REALTIME_PRIORITY) {
        KiBoostPriorityThread(Thread, Increment);
    }

    //
    // Unlock dispatcher database and lower IRQL to its previous
    // value.
    //

    KiUnlockDispatcherDatabase(OldIrql);
    return;
}

ULONG
KeForceResumeThread (
    __inout PKTHREAD Thread
    )

/*++

Routine Description:

    This function forces resumption of thread execution if the thread is
    suspended. If the specified thread is not suspended, then no operation
    is performed.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    The sum of the previous suspend count and the freeze count.

--*/

{

    KLOCK_QUEUE_HANDLE LockHandle;
    ULONG OldCount;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to SYNCH_LEVEL and acquire the thread APC queue lock.
    //

    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Thread->ApcQueueLock,
                                               &LockHandle);

    //
    // Capture the current suspend count.
    //

    OldCount = Thread->SuspendCount + Thread->FreezeCount;

    //
    // If the thread is currently suspended, then force resumption of
    // thread execution.
    //

    if (OldCount != 0) {
        Thread->FreezeCount = 0;
        Thread->SuspendCount = 0;
        KiLockDispatcherDatabaseAtSynchLevel();
        Thread->SuspendSemaphore.Header.SignalState += 1;
        KiWaitTest(&Thread->SuspendSemaphore, RESUME_INCREMENT);
        KiUnlockDispatcherDatabaseFromSynchLevel();
    }

    //
    // Unlock he thread APC queue lock, exit the scheduler, and return the
    // previous suspend count.
    //

    KeReleaseInStackQueuedSpinLockFromDpcLevel(&LockHandle);
    KiExitDispatcher(LockHandle.OldIrql);
    return OldCount;
}

VOID
KeFreezeAllThreads (
    VOID
    )

/*++

Routine Description:

    This function suspends the execution of all thread in the current
    process except the current thread. If the freeze count overflows
    the maximum suspend count, then a condition is raised.

Arguments:

    None.

Return Value:

    None.

--*/

{

    PKTHREAD CurrentThread;
    PLIST_ENTRY ListHead;
    PLIST_ENTRY NextEntry;
    PKPROCESS Process;
    KLOCK_QUEUE_HANDLE ProcessHandle;
    PKTHREAD Thread;
    KLOCK_QUEUE_HANDLE ThreadHandle;
    ULONG OldCount;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Set the address of the current thread object and the current process
    // object.
    //

    CurrentThread = KeGetCurrentThread();
    Process = CurrentThread->ApcState.Process;

    //
    // Raise IRQL to SYNCH_LEVEL and acquire the process lock.
    //

    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Process->ProcessLock,
                                               &ProcessHandle);

    //
    // If the freeze count of the current thread is not zero, then there
    // is another thread that is trying to freeze this thread. Unlock the
    // the process lock and lower IRQL to its previous value, allow the
    // suspend APC to occur, then raise IRQL to SYNCH_LEVEL and lock the
    // process lock.
    //

    while (CurrentThread->FreezeCount != 0) {
        KeReleaseInStackQueuedSpinLock(&ProcessHandle);
        KeAcquireInStackQueuedSpinLockRaiseToSynch(&Process->ProcessLock,
                                                   &ProcessHandle);
    }

    KeEnterCriticalRegion();

    //
    // Freeze all threads except the current thread.
    //

    ListHead = &Process->ThreadListHead;
    NextEntry = ListHead->Flink;
    do {

        //
        // Get the address of the next thread.
        //

        Thread = CONTAINING_RECORD(NextEntry, KTHREAD, ThreadListEntry);

        //
        // Acquire the thread APC queue lock.
        //

        KeAcquireInStackQueuedSpinLockAtDpcLevel(&Thread->ApcQueueLock,
                                                 &ThreadHandle);

        //
        // If the thread is not the current thread and APCs are queueable,
        // then attempt to suspend the thread.
        //

        if ((Thread != CurrentThread) && (Thread->ApcQueueable == TRUE)) {

            //
            // Increment the freeze count. If the thread was not previously
            // suspended, then queue the thread's suspend APC.
            //
            // N.B. The APC MUST be queued using the internal interface so
            //      the system argument fields of the APC do not get written.
            //

            OldCount = Thread->FreezeCount;

            ASSERT(OldCount != MAXIMUM_SUSPEND_COUNT);

            Thread->FreezeCount += 1;
            if ((OldCount == 0) && (Thread->SuspendCount == 0)) {
                if (Thread->SuspendApc.Inserted == TRUE) {
                    KiLockDispatcherDatabaseAtSynchLevel();
                    Thread->SuspendSemaphore.Header.SignalState -= 1;
                    KiUnlockDispatcherDatabaseFromSynchLevel();

                } else {
                    Thread->SuspendApc.Inserted = TRUE;
                    KiInsertQueueApc(&Thread->SuspendApc, RESUME_INCREMENT);
                }
            }
        }

        //
        // Release the thread APC queue lock.
        //

        KeReleaseInStackQueuedSpinLockFromDpcLevel(&ThreadHandle);
        NextEntry = NextEntry->Flink;
    } while (NextEntry != ListHead);

    //
    // Release the process lock and exit the scheduler.
    //

    KeReleaseInStackQueuedSpinLockFromDpcLevel(&ProcessHandle);
    KiExitDispatcher(ProcessHandle.OldIrql);
    return;
}

LOGICAL
KeQueryAutoAlignmentThread (
    __in PKTHREAD Thread
    )

/*++

Routine Description:

    This function returns the data alignment handling mode for the specified
    thread.

Arguments:

    None.

Return Value:

    A value of TRUE is returned if data alignment exceptions are being
    automatically handled by the kernel. Otherwise, a value of FALSE
    is returned.

--*/

{

    ASSERT_THREAD(Thread);

    return Thread->AutoAlignment;
}

LONG
KeQueryBasePriorityThread (
    __in PKTHREAD Thread
    )

/*++

Routine Description:

    This function returns the base priority increment of the specified
    thread.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    The base priority increment of the specified thread.

--*/

{

    LONG Increment;
    KIRQL OldIrql;
    PKPROCESS Process;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to SYNCH level and acquire the thread lock.
    //

    Process = Thread->Process;
    OldIrql = KeRaiseIrqlToSynchLevel();
    KiAcquireThreadLock(Thread);

    //
    // If priority saturation occured the last time the thread base priority
    // was set, then return the saturation increment value. Otherwise, compute
    // the increment value as the difference between the thread base priority
    // and the process base priority.
    //
           
    Increment = Thread->BasePriority - Process->BasePriority;
    if (Thread->Saturation != 0) {
        Increment = ((HIGH_PRIORITY + 1) / 2) * Thread->Saturation;
    }

    //
    // Release the thread lock, lower IRQL to its previous value, and
    // return the previous thread base priority increment.
    //

    KiReleaseThreadLock(Thread);
    KeLowerIrql(OldIrql);
    return Increment;
}

KPRIORITY
KeQueryPriorityThread (
    __in PKTHREAD Thread
    )

/*++

Routine Description:

    This function returns the current priority of the specified thread.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    The current priority of the specified thread.

--*/

{

    ASSERT_THREAD(Thread);

    return Thread->Priority;
}

ULONG
KeQueryRuntimeThread (
    __in PKTHREAD Thread,
    __out PULONG UserTime
    )

/*++

Routine Description:

    This function returns the kernel and user runtime for the specified
    thread.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

    UserTime - Supplies a pointer to a variable that receives the user
        runtime for the specified thread.

Return Value:

    The kernel runtime for the specfied thread is returned.

--*/

{

    ASSERT_THREAD(Thread);

    *UserTime = Thread->UserTime;
    return Thread->KernelTime;
}

BOOLEAN
KeReadStateThread (
    __in PKTHREAD Thread
    )

/*++

Routine Description:

    This function reads the current signal state of a thread object.

Arguments:

    Thread - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    The current signal state of the thread object.

--*/

{

    ASSERT_THREAD(Thread);

    //
    // Return current signal state of thread object.
    //

    return (BOOLEAN)Thread->Header.SignalState;
}

VOID
KeReadyThread (
    __inout PKTHREAD Thread
    )

/*++

Routine Description:

    This function readies a thread for execution. If the thread's process
    is currently not in the balance set, then the thread is inserted in the
    thread's process' ready queue. Else if the thread is higher priority than
    another thread that is currently running on a processor then the thread
    is selected for execution on that processor. Else the thread is inserted
    in the dispatcher ready queue selected by its priority.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    None.

--*/

{

    KIRQL OldIrql;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to dispatcher level, lock dispatcher database, ready the
    // specified thread for execution, unlock the dispatcher database, and
    // lower IRQL to its previous value.
    //

    KiLockDispatcherDatabase(&OldIrql);
    KiReadyThread(Thread);
    KiUnlockDispatcherDatabase(OldIrql);
    return;
}

ULONG
KeResumeThread (
    __inout PKTHREAD Thread
    )

/*++

Routine Description:

    This function resumes the execution of a suspended thread. If the
    specified thread is not suspended, then no operation is performed.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    The previous suspend count.

--*/

{

    KLOCK_QUEUE_HANDLE LockHandle;
    ULONG OldCount;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to SYNCH_LEVEL and lock the thread APC queue.
    //

    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Thread->ApcQueueLock,
                                               &LockHandle);

    //
    // Capture the current suspend count.
    //

    OldCount = Thread->SuspendCount;

    //
    // If the thread is currently suspended, then decrement its suspend count.
    //

    if (OldCount != 0) {
        Thread->SuspendCount -= 1;

        //
        // If the resultant suspend count is zero and the freeze count is
        // zero, then resume the thread by releasing its suspend semaphore.
        //

        if ((Thread->SuspendCount == 0) && (Thread->FreezeCount == 0)) {
            KiLockDispatcherDatabaseAtSynchLevel();
            Thread->SuspendSemaphore.Header.SignalState += 1;
            KiWaitTest(&Thread->SuspendSemaphore, RESUME_INCREMENT);
            KiUnlockDispatcherDatabaseFromSynchLevel();
        }
    }

    //
    // Release the thread APC queue, exit the scheduler, and return the
    // previous suspend count.
    //

    KeReleaseInStackQueuedSpinLockFromDpcLevel(&LockHandle);
    KiExitDispatcher(LockHandle.OldIrql);
    return OldCount;
}

VOID
KeRevertToUserAffinityThread (
    VOID
    )

/*++

Routine Description:

    This function setss the affinity of the current thread to its user
    affinity.

Arguments:

    None.

Return Value:

    None.

--*/

{

    PKTHREAD CurrentThread;
    PKTHREAD NewThread;
    KIRQL OldIrql;
    PKPRCB Prcb;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to dispatcher level and lock dispatcher database.
    //

    CurrentThread = KeGetCurrentThread();

    ASSERT(CurrentThread->SystemAffinityActive != FALSE);

    KiLockDispatcherDatabase(&OldIrql);

    //
    // Set the current affinity to the user affinity and the ideal processor
    // to the user ideal processor.
    //

    CurrentThread->Affinity = CurrentThread->UserAffinity;

#if !defined(NT_UP)

    CurrentThread->IdealProcessor = CurrentThread->UserIdealProcessor;

#endif

    CurrentThread->SystemAffinityActive = FALSE;

    //
    // If the current processor is not in the new affinity set and another
    // thread has not already been selected for execution on the current
    // processor, then select a new thread for the current processor.
    //

    Prcb = KeGetCurrentPrcb();
    if ((Prcb->SetMember & CurrentThread->Affinity) == 0) {
        KiAcquirePrcbLock(Prcb);
        if (Prcb->NextThread == NULL) {
            NewThread = KiSelectNextThread(Prcb);
            NewThread->State = Standby;
            Prcb->NextThread = NewThread;
        }

        KiReleasePrcbLock(Prcb);
    }

    //
    // Unlock dispatcher database and lower IRQL to its previous value.
    //

    KiUnlockDispatcherDatabase(OldIrql);
    return;
}

VOID
KeRundownThread (
    VOID
    )

/*++

Routine Description:

    This function is called by the executive to rundown thread structures
    which must be guarded by the dispatcher database lock and which must
    be processed before actually terminating the thread. An example of such
    a structure is the mutant ownership list that is anchored in the kernel
    thread object.

Arguments:

    None.

Return Value:

    None.

--*/

{

    PKMUTANT Mutant;
    PLIST_ENTRY NextEntry;
    KIRQL OldIrql;
    PKTHREAD Thread;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // If the mutant list is empty, then return immediately.
    //

    Thread = KeGetCurrentThread();
    if (IsListEmpty(&Thread->MutantListHead)) {
        return;
    }

    //
    // Raise IRQL to dispatcher level and lock dispatcher database.
    //

    KiLockDispatcherDatabase(&OldIrql);

    //
    // Scan the list of owned mutant objects and release the mutant objects
    // with an abandoned status. If the mutant is a kernel mutex, then bugcheck.
    //

    NextEntry = Thread->MutantListHead.Flink;
    while (NextEntry != &Thread->MutantListHead) {
        Mutant = CONTAINING_RECORD(NextEntry, KMUTANT, MutantListEntry);
        if (Mutant->ApcDisable != 0) {
            KeBugCheckEx(THREAD_TERMINATE_HELD_MUTEX,
                         (ULONG_PTR)Thread,
                         (ULONG_PTR)Mutant, 0, 0);
        }

        RemoveEntryList(&Mutant->MutantListEntry);
        Mutant->Header.SignalState = 1;
        Mutant->Abandoned = TRUE;
        Mutant->OwnerThread = (PKTHREAD)NULL;
        if (IsListEmpty(&Mutant->Header.WaitListHead) != TRUE) {
            KiWaitTest(Mutant, MUTANT_INCREMENT);
        }

        NextEntry = Thread->MutantListHead.Flink;
    }

    //
    // Release dispatcher database lock and lower IRQL to its previous value.
    //

    KiUnlockDispatcherDatabase(OldIrql);
    return;
}

KAFFINITY
KeSetAffinityThread (
    __inout PKTHREAD Thread,
    __in KAFFINITY Affinity
    )

/*++

Routine Description:

    This function sets the affinity of a specified thread to a new value.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

    Affinity - Supplies the new of set of processors on which the thread
        can run.

Return Value:

    The previous affinity of the specified thread.

--*/

{

    KAFFINITY OldAffinity;
    KIRQL OldIrql;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to dispatcher level and lock dispatcher database.
    //

    KiLockDispatcherDatabase(&OldIrql);

    //
    // Set the thread affinity to the specified value.
    //

    OldAffinity = KiSetAffinityThread(Thread, Affinity);

    //
    // Unlock dispatcher database, lower IRQL to its previous value, and
    // return the previous user affinity.
    //

    KiUnlockDispatcherDatabase(OldIrql);
    return OldAffinity;
}

VOID
KeSetSystemAffinityThread (
    __in KAFFINITY Affinity
    )

/*++

Routine Description:

    This function set the system affinity of the current thread.

Arguments:

    Affinity - Supplies the new of set of processors on which the thread
        can run.

Return Value:

    None.

--*/

{

    PKTHREAD CurrentThread;

#if !defined(NT_UP)

    ULONG IdealProcessor;
    PKNODE Node;
    KAFFINITY TempSet;

#endif

    PKTHREAD NewThread;
    KIRQL OldIrql;
    PKPRCB Prcb;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    ASSERT((Affinity & KeActiveProcessors) != 0);

    //
    // Raise IRQL to dispatcher level and lock dispatcher database.
    //

    CurrentThread = KeGetCurrentThread();
    KiLockDispatcherDatabase(&OldIrql);

    //
    // Set the current affinity to the specified affinity and set system
    // affinity active.
    //

    CurrentThread->Affinity = Affinity;
    CurrentThread->SystemAffinityActive = TRUE;

    //
    // If the ideal processor is not a member of the new affinity set, then
    // recompute the ideal processor.
    //
    // N.B. System affinity is only set temporarily, and therefore, the
    //      ideal processor is set to a convenient value if it is not
    //      already a member of the new affinity set.
    //

#if !defined(NT_UP)

    if ((Affinity & AFFINITY_MASK(CurrentThread->IdealProcessor)) == 0) {
        TempSet = Affinity & KeActiveProcessors;
        Node = KiProcessorBlock[CurrentThread->IdealProcessor]->ParentNode;
        if ((TempSet & Node->ProcessorMask) != 0) {
            TempSet &= Node->ProcessorMask;
        }

        KeFindFirstSetLeftAffinity(TempSet, &IdealProcessor);
        CurrentThread->IdealProcessor = (UCHAR)IdealProcessor;
    }

#endif

    //
    // If the current processor is not in the new affinity set and another
    // thread has not already been selected for execution on the current
    // processor, then select a new thread for the current processor.
    //

    Prcb = KeGetCurrentPrcb();
    if ((Prcb->SetMember & CurrentThread->Affinity) == 0) {
        KiAcquirePrcbLock(Prcb);
        if (Prcb->NextThread == NULL) {
            NewThread = KiSelectNextThread(Prcb);
            NewThread->State = Standby;
            Prcb->NextThread = NewThread;
        }

        KiReleasePrcbLock(Prcb);
    }

    //
    // Unlock dispatcher database and lower IRQL to its previous value.
    //

    KiUnlockDispatcherDatabase(OldIrql);
    return;
}

LONG
KeSetBasePriorityThread (
    __inout PKTHREAD Thread,
    __in LONG Increment
    )

/*++

Routine Description:

    This function sets the base priority of the specified thread to a
    new value.  The new base priority for the thread is the process base
    priority plus the increment.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

    Increment - Supplies the base priority increment of the subject thread.

        N.B. If the absolute value of the increment is such that saturation
             of the base priority is forced, then subsequent changes to the
             parent process base priority will not change the base priority
             of the thread.

Return Value:

    The previous base priority increment of the specified thread.

--*/

{

    KPRIORITY NewBase;
    KPRIORITY NewPriority;
    KPRIORITY OldBase;
    LONG OldIncrement;
    KIRQL OldIrql;
    PKPROCESS Process;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to dispatcher level and lock dispatcher database.
    //

    Process = Thread->Process;
    KiLockDispatcherDatabase(&OldIrql);

    //
    // Acquire the thread lock, capture the base priority of the specified
    // thread, and determine whether saturation if being forced.
    //

    KiAcquireThreadLock(Thread);
    OldBase = Thread->BasePriority;
    OldIncrement = OldBase - Process->BasePriority;
    if (Thread->Saturation != 0) {
        OldIncrement = ((HIGH_PRIORITY + 1) / 2) * Thread->Saturation;
    }

    Thread->Saturation = FALSE;
    if (abs(Increment) >= (HIGH_PRIORITY + 1) / 2) {
        Thread->Saturation = (Increment > 0) ? 1 : -1;
    }

    //
    // Set the base priority of the specified thread. If the thread's process
    // is in the realtime class, then limit the change to the realtime class.
    // Otherwise, limit the change to the variable class.
    //

    NewBase = Process->BasePriority + Increment;
    if (Process->BasePriority >= LOW_REALTIME_PRIORITY) {
        if (NewBase < LOW_REALTIME_PRIORITY) {
            NewBase = LOW_REALTIME_PRIORITY;

        } else if (NewBase > HIGH_PRIORITY) {
            NewBase = HIGH_PRIORITY;
        }

        //
        // Set the new priority of the thread to the new base priority.
        //

        NewPriority = NewBase;

    } else {
        if (NewBase >= LOW_REALTIME_PRIORITY) {
            NewBase = LOW_REALTIME_PRIORITY - 1;

        } else if (NewBase <= LOW_PRIORITY) {
            NewBase = 1;
        }

        //
        // Compute the new thread priority.
        //

        if (Thread->Saturation != 0) {
            NewPriority = NewBase;

        } else {

            //
            // Compute the new thread priority.
            //

            NewPriority = KiComputeNewPriority(Thread, 0);
            NewPriority += (NewBase - OldBase);
            if (NewPriority >= LOW_REALTIME_PRIORITY) {
                NewPriority = LOW_REALTIME_PRIORITY - 1;

            } else if (NewPriority <= LOW_PRIORITY) {
                NewPriority = 1;
            }
        }
    }

    //
    // Set the new base priority and clear the priority decrement. If the
    // new priority is not equal to the old priority, then set the new thread
    // priority.
    //

    Thread->PriorityDecrement = 0;
    Thread->BasePriority = (SCHAR)NewBase;
    if (NewPriority != Thread->Priority) {
        Thread->Quantum = Thread->QuantumReset;
        KiSetPriorityThread(Thread, NewPriority);
    }

    //
    // Release the thread lock, unlock the dispatcher database, lower IRQL to
    // its previous value, and return the previous thread base priority.
    //

    KiReleaseThreadLock(Thread);
    KiUnlockDispatcherDatabase(OldIrql);
    return OldIncrement;
}

LOGICAL
KeSetAutoAlignmentThread (
    __inout PKTHREAD Thread,
    __in LOGICAL Enable
    )

/*++

Routine Description:

    This function sets the data alignment handling mode for the specified
    thread.

Arguments:

    Thread - Supplies a pointer to a dispatcher object of type thread.

    Enable - Supplies a boolean value that determines the handling of data
        alignment exceptions for the specified thread.

Return Value:

    The previous value of auto alignment for the specified thread is returned
    as the function value.

--*/

{

    ASSERT_THREAD(Thread);

    //
    // Capture the previous data alignment handling mode and set the
    // specified data alignment mode.
    //

    if (Enable != FALSE) {
        return InterlockedBitTestAndSet(&Thread->ThreadFlags,
                                        KTHREAD_AUTO_ALIGNMENT_BIT);

    } else {
        return InterlockedBitTestAndReset(&Thread->ThreadFlags,
                                          KTHREAD_AUTO_ALIGNMENT_BIT);
    }
}

LOGICAL
KeSetDisableBoostThread (
    __inout PKTHREAD Thread,
    __in LOGICAL Disable
    )

/*++

Routine Description:

    This function sets the disable priority boost state for the specified
    thread.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

    Disable - Supplies a logical value that determines whether priority
        boosts are disabled or enabled.

Return Value:

    The previous value of the disable boost for the specfied thread is
    returned as the function value.

--*/

{

    ASSERT_THREAD(Thread);

    //
    // Capture the current state of the disable boost variable and set its
    // state to the specified value.
    //

    if (Disable != FALSE) {
        return InterlockedBitTestAndSet(&Thread->ThreadFlags,
                                        KTHREAD_DISABLE_BOOST_BIT);

    } else {
        return InterlockedBitTestAndReset(&Thread->ThreadFlags,
                                          KTHREAD_DISABLE_BOOST_BIT);
    }
}

UCHAR
KeSetIdealProcessorThread (
    __inout PKTHREAD Thread,
    __in UCHAR Processor
    )

/*++

Routine Description:

    This function sets the ideal processor for the specified thread execution.

    N.B. If the specified processor is less than the number of processors in
         the system and is a member of the specified thread's current affinity
         set, then the ideal processor is set. Otherwise, no operation is
         performed.

Arguments:

    Thread - Supplies a pointer to the thread whose ideal processor number is
        set to the specfied value.

    Processor - Supplies the number of the ideal processor.

Return Value:

    The previous ideal processor number.

--*/

{

    UCHAR OldProcessor;
    KIRQL OldIrql;

    ASSERT(Processor <= MAXIMUM_PROCESSORS);

    //
    // Raise IRQL, lock the dispatcher database, and capture the previous
    // ideal processor value.
    //

    KiLockDispatcherDatabase(&OldIrql);
    OldProcessor = Thread->UserIdealProcessor;

    //
    // If the specified processor is less than the number of processors in the
    // system and is a member of the specified thread's current affinity set,
    // then the ideal processor is set. Otherwise, no operation is performed.
    //

    if ((Processor < KeNumberProcessors) &&
        ((Thread->Affinity & AFFINITY_MASK(Processor)) != 0))  {

        Thread->IdealProcessor = Processor;
        if (Thread->SystemAffinityActive == FALSE) {
            Thread->UserIdealProcessor = Processor;
        }
    }

    //
    // Unlock dispatcher database, lower IRQL to its previous value, and
    // return the previous ideal processor.
    // 
    //

    KiUnlockDispatcherDatabase(OldIrql);
    return OldProcessor;
}

BOOLEAN
KeSetKernelStackSwapEnable (
    __in BOOLEAN Enable
    )

/*++

Routine Description:

    This function sets the kernel stack swap enable value for the current
    thread and returns the old swap enable value.

Arguments:

    Enable - Supplies the new kernel stack swap enable value.

Return Value:

    The previous kernel stack swap enable value.

--*/

{

    BOOLEAN OldState;
    PKTHREAD Thread;

    //
    // Capture the previous kernel stack swap enable value, set the new
    // swap enable value, and return the old swap enable value for the
    // current thread;
    //

    Thread = KeGetCurrentThread();
    OldState = Thread->EnableStackSwap;
    Thread->EnableStackSwap = Enable;
    return OldState;
}

KPRIORITY
KeSetPriorityThread (
    __inout PKTHREAD Thread,
    __in KPRIORITY Priority
    )

/*++

Routine Description:

    This function sets the priority of the specified thread to a new value.
    If the new thread priority is lower than the old thread priority, then
    rescheduling may take place if the thread is currently running on, or
    about to run on, a processor.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

    Priority - Supplies the new priority of the subject thread.

Return Value:

    The previous priority of the specified thread.

--*/

{

    KIRQL OldIrql;
    KPRIORITY OldPriority;

    ASSERT_THREAD(Thread);

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    ASSERT((Priority <= HIGH_PRIORITY) && (Priority >= LOW_PRIORITY));

    ASSERT(KeIsExecutingDpc() == FALSE);

    //
    // Raise IRQL to dispatcher level and lock dispatcher database.
    //
    // Acquire the thread lock, capture the current thread priority, set the
    // thread priority to the the new value, and replenish the thread quantum.
    // It is assumed that the priority would not be set unless the thread had
    // already lost it initial quantum.
    //

    KiLockDispatcherDatabase(&OldIrql);
    KiAcquireThreadLock(Thread);
    OldPriority = Thread->Priority;
    Thread->PriorityDecrement = 0;
    if (Priority != Thread->Priority) {
        Thread->Quantum = Thread->QuantumReset;
        if ((Thread->BasePriority != 0) &&
            (Priority == 0)) {

            Priority = 1;
        }

        KiSetPriorityThread(Thread, Priority);
    }

    //
    // Release the thread lock, unlock the dispatcher database, lower IRQL to
    // its previous value, and return the previous thread priority.
    //

    KiReleaseThreadLock(Thread);
    KiUnlockDispatcherDatabase(OldIrql);
    return OldPriority;
}

VOID
KeSetPriorityZeroPageThread (
    __in KPRIORITY Priority
    )

/*++

Routine Description:

    This function sets the priority and base priority of the current thread
    to a new value. If the new thread priority is lower than the old thread
    priority, then rescheduling may take place if the thread is currently
    running on, or about to run on, a processor.

    N.B. This function is for use only by memory managment to change the
         priority of the zero page thread.

    N.B. The priority can only to be set to a value of zero or one.

Arguments:

    Priority - Supplies the new priority of the subject thread.

Return Value:

    None.

--*/

{

    KIRQL OldIrql;
    PKTHREAD Thread;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    ASSERT((Priority == 0) || (Priority == 1));

    ASSERT(KeIsExecutingDpc() == FALSE);

    //
    // Raise IRQL to dispatcher level and lock dispatcher database.
    //
    // Acquire the thread lock, capture the current thread priority, set the
    // thread priority to the the new value, and replenish the thread quantum.
    // It is assumed that the priority would not be set unless the thread had
    // already lost it initial quantum.
    //

    Thread = KeGetCurrentThread();
    KiLockDispatcherDatabase(&OldIrql);
    KiAcquireThreadLock(Thread);
    Thread->BasePriority = (SCHAR)Priority;
    Thread->PriorityDecrement = 0;
    if (Priority != Thread->Priority) {
        Thread->Quantum = Thread->QuantumReset;
        KiSetPriorityThread(Thread, Priority);
    }

    //
    // Release the thread lock, unlock the dispatcher database, and lower
    // IRQL to its previous value.
    //

    KiReleaseThreadLock(Thread);
    KiUnlockDispatcherDatabase(OldIrql);
    return;
}

ULONG
KeSuspendThread (
    __inout PKTHREAD Thread
    )

/*++

Routine Description:

    This function suspends the execution of a thread. If the suspend count
    overflows the maximum suspend count, then a condition is raised.

Arguments:

    Thread  - Supplies a pointer to a dispatcher object of type thread.

Return Value:

    The previous suspend count.

--*/

{

    KLOCK_QUEUE_HANDLE LockHandle;
    ULONG OldCount;

    ASSERT_THREAD(Thread);
    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to SYNCH_LEVEL and acquire the thread APC queue lock.
    //

    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Thread->ApcQueueLock, &LockHandle);

    //
    // Capture the current suspend count.
    //
    // If the suspend count is at its maximum value, then unlock the
    // dispatcher database, unlock the thread APC queue lock, lower IRQL
    // to its previous value, and raise an error condition.
    //

    OldCount = Thread->SuspendCount;
    if (OldCount == MAXIMUM_SUSPEND_COUNT) {
        KeReleaseInStackQueuedSpinLock(&LockHandle);
        ExRaiseStatus(STATUS_SUSPEND_COUNT_EXCEEDED);
    }

    //
    // Don't suspend the thread if APC queuing is disabled. In this case the
    // thread is being deleted.
    //

    if (Thread->ApcQueueable == TRUE) {

        //
        // Increment the suspend count. If the thread was not previously
        // suspended, then queue the thread's suspend APC.
        //
        // N.B. The APC MUST be queued using the internal interface so
        //      the system argument fields of the APC do not get written.
        //

        Thread->SuspendCount += 1;
        if ((OldCount == 0) && (Thread->FreezeCount == 0)) {
            if (Thread->SuspendApc.Inserted == TRUE) {
                KiLockDispatcherDatabaseAtSynchLevel();
                Thread->SuspendSemaphore.Header.SignalState -= 1;
                KiUnlockDispatcherDatabaseFromSynchLevel();

            } else {
                Thread->SuspendApc.Inserted = TRUE;
                KiInsertQueueApc(&Thread->SuspendApc, RESUME_INCREMENT);
            }
        }
    }

    //
    // Release the thread APC queue lock, exit the scheduler, and  return
    // the old count.
    //

    KeReleaseInStackQueuedSpinLockFromDpcLevel(&LockHandle);
    KiExitDispatcher(LockHandle.OldIrql);
    return OldCount;
}

VOID
KeTerminateThread (
    __in KPRIORITY Increment
    )

/*++

Routine Description:

    This function terminates the execution of the current thread, sets the
    signal state of the thread to Signaled, and attempts to satisfy as many
    Waits as possible. The scheduling state of the thread is set to terminated,
    and a new thread is selected to run on the current processor. There is no
    return from this function.

Arguments:

    None.

Return Value:

    None.

--*/

{

#if defined(_AMD64_)

    KLOCK_QUEUE_HANDLE ListHandle;

#endif

    PSINGLE_LIST_ENTRY ListHead;
    KLOCK_QUEUE_HANDLE LockHandle;
    PKPROCESS Process;
    PKQUEUE Queue;
    PKTHREAD Thread;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Check if a kernel stack expand callout is active.
    //

#if defined(_AMD64_)

    KeCheckIfStackExpandCalloutActive();

#endif

    //
    // Raise IRQL to SYNCH_LEVEL, acquire the process lock, and set swap busy.
    //

    Thread = KeGetCurrentThread();
    Process = Thread->ApcState.Process;
    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Process->ProcessLock,
                                               &LockHandle);

    KiSetContextSwapBusy(Thread);

    //
    // If the thread is the last entry in the process thread list, then
    // remove the process from the kernel process list.
    //

#if defined(_AMD64_)

    if (Thread->ThreadListEntry.Flink == Thread->ThreadListEntry.Blink) {
        KeAcquireInStackQueuedSpinLockAtDpcLevel(&KiProcessListLock,
                                                 &ListHandle);

        RemoveEntryList(&Process->ProcessListEntry);
        KeReleaseInStackQueuedSpinLockFromDpcLevel(&ListHandle);
    }

#endif

    //
    // Sum the kernel and user time of the thread into the process kernel
    // and user times.
    //

    Process->KernelTime += Thread->KernelTime;
    Process->UserTime += Thread->UserTime;

    //
    // Sum the thread I/O statistics into the process I/O statistics.
    //

#if defined(_WIN64)

    ((PEPROCESS)Process)->ReadOperationCount.QuadPart += Thread->ReadOperationCount;
    ((PEPROCESS)Process)->WriteOperationCount.QuadPart += Thread->WriteOperationCount;
    ((PEPROCESS)Process)->OtherOperationCount.QuadPart += Thread->OtherOperationCount;
    ((PEPROCESS)Process)->ReadTransferCount.QuadPart += Thread->ReadTransferCount;
    ((PEPROCESS)Process)->WriteTransferCount.QuadPart += Thread->WriteTransferCount;
    ((PEPROCESS)Process)->OtherTransferCount.QuadPart += Thread->OtherTransferCount;

#endif

    //
    // Insert the thread in the reaper list.
    //
    // N.B. This code has knowledge of the reaper data structures and how
    //      worker threads are implemented.
    //

    ListHead = InterlockedPushEntrySingleList(&PsReaperListHead,
                                              (PSINGLE_LIST_ENTRY)&((PETHREAD)Thread)->ReaperLink);

    //
    // Acquire the dispatcher database and check if a reaper work item should
    // be queued.
    //

    KiLockDispatcherDatabaseAtSynchLevel();
    if (ListHead == NULL) {
        KiInsertQueue(&ExWorkerQueue[HyperCriticalWorkQueue].WorkerQueue,
                      &PsReaperWorkItem.List,
                      FALSE);
    }

    //
    // If the current thread is processing a queue entry, then remove
    // the thread from the queue object thread list and attempt to
    // activate another thread that is blocked on the queue object.
    //

    Queue = Thread->Queue;
    if (Queue != NULL) {
        RemoveEntryList(&Thread->QueueListEntry);
        KiActivateWaiterQueue(Queue);
    }

    //
    // Set the state of the current thread object to Signaled, and attempt
    // to satisfy as many Waits as possible.
    //

    Thread->Header.SignalState = TRUE;
    if (IsListEmpty(&Thread->Header.WaitListHead) != TRUE) {
        KiWaitTestWithoutSideEffects(Thread, Increment);
    }

    //
    // Remove thread from its parent process thread list. 
    //

    RemoveEntryList(&Thread->ThreadListEntry);

    //
    // Release the process lock, but don't lower the IRQL.
    //

    KeReleaseInStackQueuedSpinLockFromDpcLevel(&LockHandle);

    //
    // Set thread scheduling state to terminated, decrement the process'
    // stack count, and initiate an outswap of the process if the stack
    // count is zero.
    //

    Thread->State = Terminated;

    ASSERT(Process->StackCount != 0);

    ASSERT(Process->State == ProcessInMemory);

    Process->StackCount -= 1;
    if ((Process->StackCount == 0) &&
        (IsListEmpty(&Process->ThreadListHead) == FALSE)) {

        Process->State = ProcessOutTransition;
        InterlockedPushEntrySingleList(&KiProcessOutSwapListHead,
                                       &Process->SwapListEntry);

        KiSetInternalEvent(&KiSwapEvent, KiSwappingThread);
    }

    //
    // Rundown any architectural specific structures
    //

    KiRundownThread(Thread);

    //
    // Unlock the dispatcher database and get off the processor for the last
    // time.
    //

    KiUnlockDispatcherDatabaseFromSynchLevel();
    KiSwapThread(Thread, KeGetCurrentPrcb());
    return;
}

BOOLEAN
KeTestAlertThread (
    __in KPROCESSOR_MODE AlertMode
    )

/*++

Routine Description:

    This function tests to determine if the alerted variable for the
    specified processor mode has a value of TRUE or whether a user mode
    APC should be delivered to the current thread.

Arguments:

    AlertMode - Supplies the processor mode which is to be tested
        for an alerted condition.

Return Value:

    The previous state of the alerted variable for the specified processor
    mode.

--*/

{

    BOOLEAN Alerted;
    KLOCK_QUEUE_HANDLE LockHandle;
    PKTHREAD Thread;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to SYNCH_LEVEL and acquire the thread APC queue lock.
    //

    Thread = KeGetCurrentThread();
    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Thread->ApcQueueLock,
                                               &LockHandle);

    //
    // If the current thread is alerted for the specified processor mode,
    // then clear the alerted state. Else if the specified processor mode
    // is user and the current thread's user mode APC queue contains an
    // entry, then set user APC pending.
    //

    Alerted = Thread->Alerted[AlertMode];
    if (Alerted == TRUE) {
        Thread->Alerted[AlertMode] = FALSE;

    } else if ((AlertMode == UserMode) &&
              (IsListEmpty(&Thread->ApcState.ApcListHead[UserMode]) != TRUE)) {

        Thread->ApcState.UserApcPending = TRUE;
    }

    //
    // Release the thread APC queue lock, lower IRQL to its previous value,
    // and return the previous alerted state for the specified mode.
    //

    KeReleaseInStackQueuedSpinLock(&LockHandle);
    return Alerted;
}

VOID
KeThawAllThreads (
    VOID
    )

/*++

Routine Description:

    This function resumes the execution of all suspended froozen threads
    in the current process.

Arguments:

    None.

Return Value:

    None.

--*/

{

    PLIST_ENTRY ListHead;
    PLIST_ENTRY NextEntry;
    ULONG OldCount;
    PKPROCESS Process;
    KLOCK_QUEUE_HANDLE ProcessHandle;
    PKTHREAD Thread;
    KLOCK_QUEUE_HANDLE ThreadHandle;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    //
    // Raise IRQL to SYNCH_LEVEL and acquire the process lock.
    //

    Process = KeGetCurrentThread()->ApcState.Process;
    KeAcquireInStackQueuedSpinLockRaiseToSynch(&Process->ProcessLock,
                                               &ProcessHandle);

    //
    // Thaw the execution of all threads in the current process that have
    // been frozen.
    //

    ListHead = &Process->ThreadListHead;
    NextEntry = ListHead->Flink;
    do {

        //
        // Get the address of the next thread.
        //

        Thread = CONTAINING_RECORD(NextEntry, KTHREAD, ThreadListEntry);

        //
        // Acquire the thread APC queue lock.
        //

        KeAcquireInStackQueuedSpinLockAtDpcLevel(&Thread->ApcQueueLock,
                                                 &ThreadHandle);

        //
        // Thaw thread if its execution was previously froozen.
        //

        OldCount = Thread->FreezeCount;
        if (OldCount != 0) {
            Thread->FreezeCount -= 1;

            //
            // If the resultant suspend count is zero and the freeze count is
            // zero, then resume the thread by releasing its suspend semaphore.
            //

            if ((Thread->SuspendCount == 0) && (Thread->FreezeCount == 0)) {
                KiLockDispatcherDatabaseAtSynchLevel();
                Thread->SuspendSemaphore.Header.SignalState += 1;
                KiWaitTest(&Thread->SuspendSemaphore, RESUME_INCREMENT);
                KiUnlockDispatcherDatabaseFromSynchLevel();
            }
        }

        //
        // Release the thread APC queue lock.
        //

        KeReleaseInStackQueuedSpinLockFromDpcLevel(&ThreadHandle);
        NextEntry = NextEntry->Flink;
    } while (NextEntry != ListHead);

    //
    // Release the process lock, exit the scheduler, and leave critical
    // region.
    //

    KeReleaseInStackQueuedSpinLockFromDpcLevel(&ProcessHandle);
    KiExitDispatcher(ProcessHandle.OldIrql);
    KeLeaveCriticalRegion();
    return;
}

