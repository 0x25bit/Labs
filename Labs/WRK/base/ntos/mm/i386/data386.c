/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

   data386.c

Abstract:

    This module contains the private hardware specific global storage for
    the memory management subsystem.

--*/

#include "mi.h"

//
// A zero Pte.
//

const MMPTE ZeroPte = { 0 };

//
// A kernel zero PTE.
//

const MMPTE ZeroKernelPte = {0x0};

MMPTE MmPteGlobal = {0x0}; // Set global bit later if processor supports Global Page

//
// Note - MM_PTE_GLOBAL_MASK is or'ed into ValidKernelPte during
// initialization if the processor supports Global Page.  Use
// ValidKernelPteLocal if you don't want the global bit (ie: for session
// space).
//

MMPTE ValidKernelPte = { MM_PTE_VALID_MASK |
                         MM_PTE_WRITE_MASK |
                         MM_PTE_DIRTY_MASK |
                         MM_PTE_ACCESS_MASK };

const MMPTE ValidKernelPteLocal = { MM_PTE_VALID_MASK |
                                    MM_PTE_WRITE_MASK |
                                    MM_PTE_DIRTY_MASK |
                                    MM_PTE_ACCESS_MASK };


const MMPTE ValidUserPte = { MM_PTE_VALID_MASK |
                             MM_PTE_WRITE_MASK |
                             MM_PTE_OWNER_MASK |
                             MM_PTE_DIRTY_MASK |
                             MM_PTE_ACCESS_MASK };


const MMPTE ValidPtePte = { MM_PTE_VALID_MASK |
                            MM_PTE_WRITE_MASK |
                            MM_PTE_DIRTY_MASK |
                            MM_PTE_ACCESS_MASK };


const MMPTE ValidPdePde = { MM_PTE_VALID_MASK |
                            MM_PTE_WRITE_MASK |
                            MM_PTE_DIRTY_MASK |
                            MM_PTE_ACCESS_MASK };


MMPTE ValidKernelPde = { MM_PTE_VALID_MASK |
                         MM_PTE_WRITE_MASK |
                         MM_PTE_DIRTY_MASK |
                         MM_PTE_ACCESS_MASK };

const MMPTE ValidKernelPdeLocal = { MM_PTE_VALID_MASK |
                                    MM_PTE_WRITE_MASK |
                                    MM_PTE_DIRTY_MASK |
                                    MM_PTE_ACCESS_MASK };

// NOTE - MM_PTE_GLOBAL_MASK  or'ed in later if processor supports Global Page


MMPTE DemandZeroPde = { MM_READWRITE << 5 };


const MMPTE DemandZeroPte = { MM_READWRITE << 5 };


const MMPTE TransitionPde = { MM_PTE_WRITE_MASK |
                              MM_PTE_OWNER_MASK |
                              MM_PTE_TRANSITION_MASK |
                              MM_READWRITE << 5 };

#if !defined (_X86PAE_)
MMPTE PrototypePte = { 0xFFFFF000 |
                       MM_PTE_PROTOTYPE_MASK |
                       MM_READWRITE << 5 };
#else
MMPTE PrototypePte = { (MI_PTE_LOOKUP_NEEDED << 32) |
                       MM_PTE_PROTOTYPE_MASK |
                       MM_READWRITE << 5 };
#endif


//
// PTE which generates an access violation when referenced.
//

const MMPTE NoAccessPte = {MM_NOACCESS << 5};

//
// Pool start and end.
//

PVOID MmNonPagedPoolStart;

PVOID MmNonPagedPoolEnd = (PVOID) MM_NONPAGED_POOL_END;

PVOID MmPagedPoolStart = (PVOID) MM_DEFAULT_PAGED_POOL_START;

PVOID MmPagedPoolEnd;

PMMWSL MmWorkingSetList;

ULONG MiMaximumWorkingSet =
       ((ULONG)((ULONG)2*1024*1024*1024 - 64*1024) >> PAGE_SHIFT); //2Gb-64K.

//
// Color tables for free and zeroed pages.
//

PMMCOLOR_TABLES MmFreePagesByColor[2];

//
// Color tables for modified pages destined for the paging file.
//

MMPFNLIST MmModifiedPageListByColor[MM_MAXIMUM_NUMBER_OF_COLORS] = {
                            0, ModifiedPageList, MM_EMPTY_LIST, MM_EMPTY_LIST};


//
// Count of the number of modified pages destined for the paging file.
//

ULONG MmTotalPagesForPagingFile = 0;

//
// Pte reserved for mapping pages for the debugger.
//

PMMPTE MmDebugPte = (MiGetPteAddress(MM_DEBUG_VA));

//
// 16 PTEs reserved for mapping MDLs (64k max).
//

PMMPTE MmCrashDumpPte = (MiGetPteAddress(MM_CRASH_DUMP_VA));

//
// Number of additional system PTEs present.
//

ULONG_PTR MiUseMaximumSystemSpace;
ULONG_PTR MiUseMaximumSystemSpaceEnd;

//
// Size of extended system cache.
//

#if defined (_X86PAE_)
PMMPTE MmSystemCacheWorkingSetListPte;
#endif

ULONG MiMaximumSystemCacheSizeExtra;

PVOID MiSystemCacheStartExtra;

PVOID MiSystemCacheEndExtra;

ULONG_PTR MmBootImageSize;

