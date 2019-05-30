/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    cachedat.c

Abstract:

    This module implements the Memory Management based cache management
    routines for the common Cache subsystem.

--*/

#include "cc.h"

//
//  Global SharedCacheMap lists and resource to synchronize access to it.
//
//

// extern KSPIN_LOCK CcMasterSpinLock;
LIST_ENTRY CcCleanSharedCacheMapList;
SHARED_CACHE_MAP_LIST_CURSOR CcDirtySharedCacheMapList;
SHARED_CACHE_MAP_LIST_CURSOR CcLazyWriterCursor;

//
//  Worker thread structures:
//
//      A spinlock to synchronize all three lists.
//      A count of the number of worker threads Cc will use
//      A count of the number of worker threads Cc in use
//      A listhead for preinitialized executive work items for Cc use.
//      A listhead for an express queue of WORK_QUEUE_ENTRYs
//      A listhead for a regular queue of WORK_QUEUE_ENTRYs
//      A listhead for a post-tick queue of WORK_QUEUE_ENTRYs
//
//      A flag indicating if we are throttling the queue to a single thread
//

// extern KSPIN_LOCK CcWorkQueueSpinLock;
ULONG CcNumberWorkerThreads = 0;
ULONG CcNumberActiveWorkerThreads = 0;
LIST_ENTRY CcIdleWorkerThreadList;
LIST_ENTRY CcExpressWorkQueue;
LIST_ENTRY CcRegularWorkQueue;
LIST_ENTRY CcPostTickWorkQueue;

BOOLEAN CcQueueThrottle = FALSE;

//
//  Store the current idle delay and target time to clean all.  We must calculate
//  the idle delay in terms of clock ticks for the lazy writer timeout.
//

ULONG CcIdleDelayTick;
LARGE_INTEGER CcNoDelay;
LARGE_INTEGER CcFirstDelay = {(ULONG)-(3*LAZY_WRITER_IDLE_DELAY), -1};
LARGE_INTEGER CcIdleDelay = {(ULONG)-LAZY_WRITER_IDLE_DELAY, -1};
LARGE_INTEGER CcCollisionDelay = {(ULONG)-LAZY_WRITER_COLLISION_DELAY, -1};
LARGE_INTEGER CcTargetCleanDelay = {(ULONG)-(LONG)(LAZY_WRITER_IDLE_DELAY * (LAZY_WRITER_MAX_AGE_TARGET + 1)), -1};

//
//  Spinlock for controlling access to Vacb and related global structures,
//  and a counter indicating how many Vcbs are active.
//

// extern KSPIN_LOCK CcVacbSpinLock;
ULONG_PTR CcNumberVacbs;

//
//  Pointer to the global Vacb vector.
//

PVACB CcVacbs;
PVACB CcBeyondVacbs;
LIST_ENTRY CcVacbLru;
LIST_ENTRY CcVacbFreeList;
ULONG CcMaxVacbLevelsSeen = 1;
ULONG CcVacbLevelEntries = 0;
PVACB *CcVacbLevelFreeList = NULL;
ULONG CcVacbLevelWithBcbsEntries = 0;
PVACB *CcVacbLevelWithBcbsFreeList = NULL;

//
//  Deferred write list and respective Thresholds
//

extern ALIGNED_SPINLOCK CcDeferredWriteSpinLock;
LIST_ENTRY CcDeferredWrites;
ULONG CcDirtyPageThreshold;
ULONG CcDirtyPageTarget;
ULONG CcPagesYetToWrite;
ULONG CcPagesWrittenLastTime = 0;
ULONG CcDirtyPagesLastScan = 0;
ULONG CcAvailablePagesThreshold = 100;
ULONG CcTotalDirtyPages = 0;

//
//  Captured system size
//

MM_SYSTEMSIZE CcCapturedSystemSize;

//
//  Number of outstanding aggressive zeroers in the system.  Used
//  to throttle the activity.
//

LONG CcAggressiveZeroCount;
LONG CcAggressiveZeroThreshold;

//
//  Tuning options du Jour
//

ULONG CcTune = 0;

//
//  Global structure controlling lazy writer algorithms
//

LAZY_WRITER LazyWriter;

GENERAL_LOOKASIDE CcTwilightLookasideList;

//
//  Global list of pinned Bcbs which may be examined for debug purposes
//

#if DBG

ULONG CcBcbCount;
LIST_ENTRY CcBcbList;

#endif

//
//  Throw away miss counter.
//

ULONG CcThrowAway;

//
//  Performance Counters
//

ULONG CcFastReadNoWait;
ULONG CcFastReadWait;
ULONG CcFastReadResourceMiss;
ULONG CcFastReadNotPossible;

ULONG CcFastMdlReadNoWait;
ULONG CcFastMdlReadWait;
ULONG CcFastMdlReadResourceMiss;
ULONG CcFastMdlReadNotPossible;

ULONG CcMapDataNoWait;
ULONG CcMapDataWait;
ULONG CcMapDataNoWaitMiss;
ULONG CcMapDataWaitMiss;

ULONG CcPinMappedDataCount;

ULONG CcPinReadNoWait;
ULONG CcPinReadWait;
ULONG CcPinReadNoWaitMiss;
ULONG CcPinReadWaitMiss;

ULONG CcCopyReadNoWait;
ULONG CcCopyReadWait;
ULONG CcCopyReadNoWaitMiss;
ULONG CcCopyReadWaitMiss;

ULONG CcMdlReadNoWait;
ULONG CcMdlReadWait;
ULONG CcMdlReadNoWaitMiss;
ULONG CcMdlReadWaitMiss;

ULONG CcReadAheadIos;

ULONG CcLazyWriteHotSpots;
ULONG CcLazyWriteIos;
ULONG CcLazyWritePages;
ULONG CcDataFlushes;
ULONG CcDataPages;

ULONG CcLostDelayedWrites;

PULONG CcMissCounter = &CcThrowAway;
