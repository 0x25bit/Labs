/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

   extsect.c

Abstract:

    This module contains the routines which implement the
    NtExtendSection service.

--*/

#include "mi.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,NtExtendSection)
#pragma alloc_text(PAGE,MmExtendSection)
#endif

extern SIZE_T MmAllocationFragment;

ULONG MiExtendedSubsectionsConvertedToDynamic;

#if DBG
VOID
MiSubsectionConsistent (
    IN PSUBSECTION Subsection
    )
/*++

Routine Description:

    This function checks to ensure the subsection is consistent.

Arguments:

    Subsection - Supplies a pointer to the subsection to be checked.

Return Value:

    None.

--*/

{
    ULONG Sectors;
    ULONG FullPtes;

    //
    // Compare the disk sectors (4K units) to the PTE allocation.
    //

    Sectors = Subsection->NumberOfFullSectors;
    if (Subsection->u.SubsectionFlags.SectorEndOffset) {
        Sectors += 1;
    }

    //
    // Calculate how many PTEs are needed to map this number of sectors.
    //

    FullPtes = Sectors >> (PAGE_SHIFT - MM4K_SHIFT);

    if (Sectors & ((1 << (PAGE_SHIFT - MM4K_SHIFT)) - 1)) {
        FullPtes += 1;
    }

    if (FullPtes != Subsection->PtesInSubsection) {
        DbgPrintEx (DPFLTR_MM_ID, DPFLTR_ERROR_LEVEL, "Mm: Subsection inconsistent (%x vs %x)\n",
            FullPtes,
            Subsection->PtesInSubsection);
        DbgBreakPoint ();
    }
}
#endif


NTSTATUS
NtExtendSection(
    __in HANDLE SectionHandle,
    __inout PLARGE_INTEGER NewSectionSize
    )

/*++

Routine Description:

    This function extends the size of the specified section.  If
    the current size of the section is greater than or equal to the
    specified section size, the size is not updated.

Arguments:

    SectionHandle - Supplies an open handle to a section object.

    NewSectionSize - Supplies the new size for the section object.

Return Value:

    NTSTATUS.

--*/

{
    KPROCESSOR_MODE PreviousMode;
    PVOID Section;
    NTSTATUS Status;
    LARGE_INTEGER CapturedNewSectionSize;

    PAGED_CODE();

    //
    // Check to make sure the new section size is accessible.
    //

    PreviousMode = KeGetPreviousMode();

    if (PreviousMode != KernelMode) {

        try {

            ProbeForWriteSmallStructure (NewSectionSize,
                                         sizeof(LARGE_INTEGER),
                                         PROBE_ALIGNMENT (LARGE_INTEGER));

            CapturedNewSectionSize = *NewSectionSize;

        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            // If an exception occurs during the probe or capture
            // of the initial values, then handle the exception and
            // return the exception code as the status value.
            //

            return GetExceptionCode ();
        }

    }
    else {

        CapturedNewSectionSize = *NewSectionSize;
    }

    //
    // Reference the section object.
    //

    Status = ObReferenceObjectByHandle (SectionHandle,
                                        SECTION_EXTEND_SIZE,
                                        MmSectionObjectType,
                                        PreviousMode,
                                        (PVOID *)&Section,
                                        NULL);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = MmExtendSection (Section, &CapturedNewSectionSize, FALSE);

    ObDereferenceObject (Section);

    //
    // Update the NewSectionSize field.
    //

    try {

        //
        // Return the captured section size.
        //

        *NewSectionSize = CapturedNewSectionSize;

    } except (EXCEPTION_EXECUTE_HANDLER) {
        NOTHING;
    }

    return Status;
}


LOGICAL
MiAppendSubsectionChain (
    IN PMSUBSECTION LastSubsection,
    IN PMSUBSECTION ExtendedSubsectionHead
    )

/*++

Routine Description:

    This non-pageable wrapper function extends the specified subsection chain.

Arguments:

    LastSubsection - Supplies the last subsection in the existing control area.

    ExtendedSubsectionHead - Supplies an anchor pointing to the first
                             subsection in the chain to append.

Return Value:

    TRUE if the chain was appended, FALSE if not.

--*/

{
    KIRQL OldIrql;
    PMSUBSECTION NewSubsection;

    ASSERT (ExtendedSubsectionHead->NextSubsection != NULL);

    ASSERT (ExtendedSubsectionHead->u.SubsectionFlags.SectorEndOffset == 0);

    NewSubsection = (PMSUBSECTION) ExtendedSubsectionHead->NextSubsection;

    LOCK_PFN (OldIrql);

    //
    // This subsection may be extending a range that is already
    // mapped by a VAD(s).  There is no way to tell how many VADs
    // already map it so if any do, just leave all the new subsections
    // marked as not reclaimable until the control area is deleted.
    //
    // If however, there are no *USER* references to this control area,
    // then the subsections can be marked as dynamic now.  Note other
    // portions of code (currently only prefetch) that issue "dereference
    // from this subsection to the end of file" are safe because these
    // portions create user sections first and so the first check below
    // will be FALSE.
    //

    if (LastSubsection->ControlArea->NumberOfUserReferences != 0) {

        //
        // The caller has not allocated prototype PTEs and they are required.
        // Return so the caller can allocate and retry.
        //

        if (NewSubsection->SubsectionBase == NULL) {
            ASSERT (NewSubsection->u.SubsectionFlags.SubsectionStatic == 0);
            UNLOCK_PFN (OldIrql);
            return FALSE;
        }
#if DBG
        do {
            ASSERT (NewSubsection->u.SubsectionFlags.SubsectionStatic == 1);
            NewSubsection = (PMSUBSECTION) NewSubsection->NextSubsection;
        } while (NewSubsection != NULL);
#endif
    }
    else if (NewSubsection->SubsectionBase != NULL) {

        //
        // The prototype PTEs are no longer required (user views went away)
        // even though they were when the caller first built the subsections.
        // Mark the subsections as dynamic now.
        //

        do {
            ASSERT (NewSubsection->u.SubsectionFlags.SubsectionStatic == 1);

            MI_SNAP_SUB (NewSubsection, 0x1);

            NewSubsection->u.SubsectionFlags.SubsectionStatic = 0;
            NewSubsection->u2.SubsectionFlags2.SubsectionConverted = 1;
            NewSubsection->NumberOfMappedViews = 1;

            MiRemoveViewsFromSection (NewSubsection, 
                                      NewSubsection->PtesInSubsection);

            MiExtendedSubsectionsConvertedToDynamic += 1;

            MI_SNAP_SUB (NewSubsection, 0x2);
            NewSubsection = (PMSUBSECTION) NewSubsection->NextSubsection;
        } while (NewSubsection != NULL);
    }

    LastSubsection->u.SubsectionFlags.SectorEndOffset = 0;

    LastSubsection->NumberOfFullSectors = ExtendedSubsectionHead->NumberOfFullSectors;

    //
    // A memory barrier is needed to ensure the writes initializing the
    // subsection fields are visible prior to linking the subsection into
    // the chain.  This is because some reads from these fields are done
    // lock free for improved performance.
    //

    KeMemoryBarrier ();

    LastSubsection->NextSubsection = ExtendedSubsectionHead->NextSubsection;

    UNLOCK_PFN (OldIrql);

    return TRUE;
}


NTSTATUS
MmExtendSection (
    IN PVOID SectionToExtend,
    IN OUT PLARGE_INTEGER NewSectionSize,
    IN ULONG IgnoreFileSizeChecking
    )

/*++

Routine Description:

    This function extends the size of the specified section.  If
    the current size of the section is greater than or equal to the
    specified section size, the size is not updated.

Arguments:

    Section - Supplies a pointer to a referenced section object.

    NewSectionSize - Supplies the new size for the section object.

    IgnoreFileSizeChecking -  Supplies the value TRUE is file size
                              checking should be ignored (i.e., it
                              is being called from a file system which
                              has already done the checks).  FALSE
                              if the checks still need to be made.

Return Value:

    NTSTATUS.

--*/

{
    LOGICAL Appended;
    PMMPTE ProtoPtes;
    MMPTE TempPte;
    PCONTROL_AREA ControlArea;
    PSEGMENT Segment;
    PSECTION Section;
    PSUBSECTION LastSubsection;
    PSUBSECTION Subsection;
    PMSUBSECTION ExtendedSubsection;
    MSUBSECTION ExtendedSubsectionHead;
    PMSUBSECTION LastExtendedSubsection;
    UINT64 RequiredPtes;
    UINT64 NumberOfPtes;
    UINT64 TotalNumberOfPtes;
    ULONG PtesUsed;
    ULONG UnusedPtes;
    UINT64 AllocationSize;
    UINT64 RunningSize;
    UINT64 EndOfFile;
    NTSTATUS Status;
    LARGE_INTEGER NumberOf4KsForEntireFile;
    LARGE_INTEGER Starting4K;
    LARGE_INTEGER NextSubsection4KStart;
    LARGE_INTEGER Last4KChunk;
    ULONG PartialSize;
    SIZE_T AllocationFragment;
    PKTHREAD CurrentThread;

    PAGED_CODE();

    Section = (PSECTION)SectionToExtend;

    //
    // Make sure the section is really extendable - physical and
    // image sections are not.
    //

    Segment = Section->Segment;
    ControlArea = Segment->ControlArea;

    if ((ControlArea->u.Flags.PhysicalMemory || ControlArea->u.Flags.Image) ||
         (ControlArea->FilePointer == NULL)) {
        return STATUS_SECTION_NOT_EXTENDED;
    }

    //
    // Acquire the section extension mutex, this blocks other threads from
    // updating the size at the same time.
    //

    CurrentThread = KeGetCurrentThread ();
    KeEnterCriticalRegionThread (CurrentThread);
    ExAcquireResourceExclusiveLite (&MmSectionExtendResource, TRUE);

    //
    // Calculate the number of prototype PTE chunks to build for this section.
    // A subsection is also allocated for each chunk as all the prototype PTEs
    // in any given chunk are initially encoded to point at the same subsection.
    //
    // The maximum total section size is 16PB (2^54).  This is because the
    // StartingSector4132 field in each subsection, ie: 2^42-1 bits of file
    // offset where the offset is in 4K (not pagesize) units.  Thus, a
    // subsection may describe a *BYTE* file start offset of maximum
    // 2^54 - 4K.
    //
    // Each subsection can span at most 16TB - 64K.  This is because the
    // NumberOfFullSectors and various other fields in the subsection are
    // ULONGs.  In reality, this is a nonissue as far as maximum section size
    // is concerned because any number of subsections can be chained together
    // and in fact, subsections are allocated to span less to facilitate
    // efficient dynamic prototype PTE trimming and reconstruction.
    //

    if (NewSectionSize->QuadPart > MI_MAXIMUM_SECTION_SIZE) {
        Status = STATUS_SECTION_TOO_BIG;
        goto ReleaseAndReturn;
    }

    NumberOfPtes = (NewSectionSize->QuadPart + PAGE_SIZE - 1) >> PAGE_SHIFT;

    if (ControlArea->u.Flags.WasPurged == 0) {

        if ((UINT64)NewSectionSize->QuadPart <= (UINT64)Section->SizeOfSection.QuadPart) {
            *NewSectionSize = Section->SizeOfSection;
            goto ReleaseAndReturnSuccess;
        }
    }

    //
    // If a file handle was specified, set the allocation size of the file.
    //

    if (IgnoreFileSizeChecking == FALSE) {

        //
        // Release the resource so we don't deadlock with the file
        // system trying to extend this section at the same time.
        //

        ExReleaseResourceLite (&MmSectionExtendResource);

        //
        // Get a different resource to single thread query/set operations.
        //

        ExAcquireResourceExclusiveLite (&MmSectionExtendSetResource, TRUE);

        //
        // Query the file size to see if this file really needs extending.
        //
        // If the specified size is less than the current size, return
        // the current size.
        //

        Status = FsRtlGetFileSize (ControlArea->FilePointer,
                                   (PLARGE_INTEGER)&EndOfFile);

        if (!NT_SUCCESS (Status)) {
            ExReleaseResourceLite (&MmSectionExtendSetResource);
            KeLeaveCriticalRegionThread (CurrentThread);
            return Status;
        }

        if ((UINT64)NewSectionSize->QuadPart > EndOfFile) {

            //
            // Don't allow section extension unless the section was originally
            // created with write access.  The check couldn't be done at create
            // time without breaking existing binaries, so the caller gets the
            // error at this point instead.
            //

            if (((Section->InitialPageProtection & PAGE_READWRITE) |
                (Section->InitialPageProtection & PAGE_EXECUTE_READWRITE)) == 0) {
                    ExReleaseResourceLite (&MmSectionExtendSetResource);
                    KeLeaveCriticalRegionThread (CurrentThread);
                    return STATUS_SECTION_NOT_EXTENDED;
            }

            //
            // Current file is smaller, attempt to set a new end of file.
            //

            EndOfFile = *(PUINT64)NewSectionSize;

            Status = FsRtlSetFileSize (ControlArea->FilePointer,
                                       (PLARGE_INTEGER)&EndOfFile);

            if (!NT_SUCCESS (Status)) {
                ExReleaseResourceLite (&MmSectionExtendSetResource);
                KeLeaveCriticalRegionThread (CurrentThread);
                return Status;
            }
        }

        if (Segment->ExtendInfo) {
            KeAcquireGuardedMutex (&MmSectionBasedMutex);
            if (Segment->ExtendInfo) {
                Segment->ExtendInfo->CommittedSize = EndOfFile;
            }
            KeReleaseGuardedMutex (&MmSectionBasedMutex);
        }

        //
        // Release the query/set resource and reacquire the extend section
        // resource.
        //

        ExReleaseResourceLite (&MmSectionExtendSetResource);
        ExAcquireResourceExclusiveLite (&MmSectionExtendResource, TRUE);
    }

    //
    // Find the last subsection.
    //

    ASSERT (ControlArea->u.Flags.GlobalOnlyPerSession == 0);

    if (((PMAPPED_FILE_SEGMENT)Segment)->LastSubsectionHint != NULL) {
        LastSubsection = (PSUBSECTION) ((PMAPPED_FILE_SEGMENT)Segment)->LastSubsectionHint;
    }
    else {
        if (ControlArea->u.Flags.Rom == 1) {
            LastSubsection = (PSUBSECTION)((PLARGE_CONTROL_AREA)ControlArea + 1);
        }
        else {
            LastSubsection = (PSUBSECTION)(ControlArea + 1);
        }
    }

    while (LastSubsection->NextSubsection != NULL) {
        ASSERT (LastSubsection->UnusedPtes == 0);
        LastSubsection = LastSubsection->NextSubsection;
    }

    MI_CHECK_SUBSECTION (LastSubsection);

    //
    // Does the structure need extending?
    //

    TotalNumberOfPtes = (((UINT64)Segment->SegmentFlags.TotalNumberOfPtes4132) << 32) | Segment->TotalNumberOfPtes;

    if (NumberOfPtes <= TotalNumberOfPtes) {

        //
        // The segment is already large enough, just update
        // the section size and return.
        //

        Section->SizeOfSection = *NewSectionSize;
        if (Segment->SizeOfSegment < (UINT64)NewSectionSize->QuadPart) {

            //
            // Only update if it is really bigger.
            //

            Segment->SizeOfSegment = *(PUINT64)NewSectionSize;

            Mi4KStartFromSubsection(&Starting4K, LastSubsection);

            Last4KChunk.QuadPart = (NewSectionSize->QuadPart >> MM4K_SHIFT) - Starting4K.QuadPart;

            ASSERT (Last4KChunk.HighPart == 0);

            LastSubsection->NumberOfFullSectors = Last4KChunk.LowPart;
            LastSubsection->u.SubsectionFlags.SectorEndOffset =
                                        NewSectionSize->LowPart & MM4K_MASK;
            MI_CHECK_SUBSECTION (LastSubsection);
        }
        goto ReleaseAndReturnSuccess;
    }

    //
    // Add new structures to the section - locate the last subsection
    // and add there.
    //

    RequiredPtes = NumberOfPtes - TotalNumberOfPtes;
    PtesUsed = 0;

    if (RequiredPtes < LastSubsection->UnusedPtes) {

        //
        // There are ample PTEs to extend the section already allocated.
        //

        PtesUsed = (ULONG) RequiredPtes;
        RequiredPtes = 0;

    }
    else {
        PtesUsed = LastSubsection->UnusedPtes;
        RequiredPtes -= PtesUsed;
    }

    LastSubsection->PtesInSubsection += PtesUsed;
    LastSubsection->UnusedPtes -= PtesUsed;
    Segment->SizeOfSegment += (UINT64)PtesUsed * PAGE_SIZE;

    TotalNumberOfPtes += PtesUsed;

    Segment->TotalNumberOfPtes = (ULONG) TotalNumberOfPtes;
    if (TotalNumberOfPtes >= 0x100000000) {
        Segment->SegmentFlags.TotalNumberOfPtes4132 = (ULONG_PTR)(TotalNumberOfPtes >> 32);
    }

    if (RequiredPtes == 0) {

        //
        // There is no extension necessary, update the high VBN.
        //

        Mi4KStartFromSubsection(&Starting4K, LastSubsection);

        Last4KChunk.QuadPart = (NewSectionSize->QuadPart >> MM4K_SHIFT) - Starting4K.QuadPart;

        ASSERT (Last4KChunk.HighPart == 0);

        LastSubsection->NumberOfFullSectors = Last4KChunk.LowPart;

        LastSubsection->u.SubsectionFlags.SectorEndOffset =
                                    NewSectionSize->LowPart & MM4K_MASK;
        MI_CHECK_SUBSECTION (LastSubsection);
    }
    else {

        //
        // An extension is required.
        //
        // Allocate the subsection(s) now.
        //
        // If there are any user views, then also allocate the prototype
        // PTEs now (because a user view may be for an extended VAD which
        // would already be encompassing this new extension).  If there
        // are no user views, then don't allocate the prototype PTEs until
        // the views are actually mapped (system views never pre-extend past
        // the end of the file).
        //

        NumberOf4KsForEntireFile.QuadPart = Segment->SizeOfSegment >> MM4K_SHIFT;
        AllocationSize = MI_ROUND_TO_SIZE (RequiredPtes * sizeof(MMPTE), PAGE_SIZE);

        AllocationFragment = MmAllocationFragment;

        RunningSize = 0;

        ExtendedSubsectionHead = *(PMSUBSECTION)LastSubsection;

        LastExtendedSubsection = &ExtendedSubsectionHead;

        ASSERT (LastExtendedSubsection->NextSubsection == NULL);

        SATISFY_OVERZEALOUS_COMPILER (NextSubsection4KStart.QuadPart = 0);

        do {

            //
            // Bound the size of each prototype PTE allocation so both :
            //  1. it can succeed even in cases where the pool is fragmented.
            //  2. later on last control area dereference, each subsection
            //     is converted to dynamic and can be pruned/recreated
            //     individually without losing (or requiring) contiguous pool.
            //

            if (AllocationSize - RunningSize > AllocationFragment) {
                PartialSize = (ULONG) AllocationFragment;
            }
            else {
                PartialSize = (ULONG) (AllocationSize - RunningSize);
            }

            //
            // Allocate an extended subsection.
            //

            ExtendedSubsection = (PMSUBSECTION) ExAllocatePoolWithTag (NonPagedPool,
                                                            sizeof(MSUBSECTION),
                                                            'dSmM');
            if (ExtendedSubsection == NULL) {
                goto ExtensionFailed;
            }

            ExtendedSubsection->SubsectionBase = NULL;
            ExtendedSubsection->NextSubsection = NULL;

            LastExtendedSubsection->NextSubsection = (PSUBSECTION) ExtendedSubsection;

            ASSERT (ControlArea->FilePointer != NULL);

            ExtendedSubsection->u.LongFlags = 0;

            ExtendedSubsection->ControlArea = ControlArea;

            ExtendedSubsection->PtesInSubsection = PartialSize / sizeof(MMPTE);
            ExtendedSubsection->UnusedPtes = 0;

            RunningSize += PartialSize;

            if (RunningSize > (RequiredPtes * sizeof(MMPTE))) {
                UnusedPtes = (ULONG)(RunningSize / sizeof(MMPTE) - RequiredPtes);
                ExtendedSubsection->PtesInSubsection -= UnusedPtes;
                ExtendedSubsection->UnusedPtes = UnusedPtes;
            }

            ExtendedSubsection->u.SubsectionFlags.Protection =
                    (unsigned) Segment->SegmentPteTemplate.u.Soft.Protection;

            ExtendedSubsection->DereferenceList.Flink = NULL;
            ExtendedSubsection->DereferenceList.Blink = NULL;
            ExtendedSubsection->NumberOfMappedViews = 0;
            ExtendedSubsection->u2.LongFlags2 = 0;


            //
            // Adjust the previous subsection to account for the new length.
            // Note that since the next allocation in this loop may fail,
            // the very first previous subsection changes are not rippled
            // to the chained subsection until the loop completes successfully.
            //

            if (LastExtendedSubsection == &ExtendedSubsectionHead) {

                Mi4KStartFromSubsection (&Starting4K, LastExtendedSubsection);

                Last4KChunk.QuadPart = NumberOf4KsForEntireFile.QuadPart -
                                            Starting4K.QuadPart;

                if (LastExtendedSubsection->u.SubsectionFlags.SectorEndOffset) {
                    Last4KChunk.QuadPart += 1;
                }

                ASSERT (Last4KChunk.HighPart == 0);

                LastExtendedSubsection->NumberOfFullSectors = Last4KChunk.LowPart;
                LastExtendedSubsection->u.SubsectionFlags.SectorEndOffset = 0;

                //
                // If the number of sectors doesn't completely fill the PTEs
                // (only possible when the page size is not MM4K), then
                // fill it now.
                //

                if (LastExtendedSubsection->NumberOfFullSectors & ((1 << (PAGE_SHIFT - MM4K_SHIFT)) - 1)) {
                    LastExtendedSubsection->NumberOfFullSectors += 1;
                }

                MI_CHECK_SUBSECTION (LastExtendedSubsection);

                Starting4K.QuadPart += LastExtendedSubsection->NumberOfFullSectors;
                NextSubsection4KStart.QuadPart = Starting4K.QuadPart;
            }
            else {
                NextSubsection4KStart.QuadPart += LastExtendedSubsection->NumberOfFullSectors;
            }

            //
            // Initialize the newly allocated subsection.
            //

            Mi4KStartForSubsection (&NextSubsection4KStart, ExtendedSubsection);

            if (RunningSize < AllocationSize) {

                //
                // Not the final subsection so all quantities are full pages.
                //

                ExtendedSubsection->NumberOfFullSectors =
                        (PartialSize / sizeof (MMPTE)) << (PAGE_SHIFT - MM4K_SHIFT);
                ExtendedSubsection->u.SubsectionFlags.SectorEndOffset = 0;
            }
            else {

                //
                // The final subsection so quantities are not always full pages.
                //

                Last4KChunk.QuadPart =
                    (NewSectionSize->QuadPart >> MM4K_SHIFT) - NextSubsection4KStart.QuadPart;

                ASSERT (Last4KChunk.HighPart == 0);

                ExtendedSubsection->NumberOfFullSectors = Last4KChunk.LowPart;

                ExtendedSubsection->u.SubsectionFlags.SectorEndOffset =
                                    NewSectionSize->LowPart & MM4K_MASK;
            }

            MI_CHECK_SUBSECTION (ExtendedSubsection);

            //
            // This subsection may be extending a range that is already
            // mapped by a VAD(s).  There is no way to tell how many VADs
            // already map it so just mark the entire subsection as not
            // reclaimable until the control area is deleted.
            //
            // This also saves other portions of code that issue "dereference
            // from this subsection to the end of file" as these subsections are
            // marked as static not dynamic (at least until segment dereference
            // time).
            //
            // When this chain is appended to the control area at the end of
            // this routine an attempt is made to convert the subsection chain
            // to dynamic if no user mapped views are active.
            //

            LastExtendedSubsection = ExtendedSubsection;

        } while (RunningSize < AllocationSize);

        if (ControlArea->NumberOfUserReferences == 0) {
            ASSERT (IgnoreFileSizeChecking == TRUE);
        }

        //
        // All the subsections have been allocated, try to append the
        // subsection chain without allocating prototype PTEs.  If user
        // views are present, the append will fail, at which point we will
        // attempt to allocate prototype PTEs and retry.
        //

        Appended = MiAppendSubsectionChain ((PMSUBSECTION)LastSubsection,
                                            &ExtendedSubsectionHead);

        if (Appended == FALSE) {

            RunningSize = 0;
    
            Subsection = (PSUBSECTION) &ExtendedSubsectionHead;
    
            do {
    
                if (AllocationSize - RunningSize > AllocationFragment) {
                    PartialSize = (ULONG) AllocationFragment;
                }
                else {
                    PartialSize = (ULONG) (AllocationSize - RunningSize);
                }
    
                RunningSize += PartialSize;
    
                ProtoPtes = (PMMPTE)ExAllocatePoolWithTag (PagedPool | POOL_MM_ALLOCATION,
                                                           PartialSize,
                                                           MMSECT);
    
                if (ProtoPtes == NULL) {
                    goto ExtensionFailed;
                }
    
                Subsection = Subsection->NextSubsection;
    
                Subsection->SubsectionBase = ProtoPtes;
                Subsection->u.SubsectionFlags.SubsectionStatic = 1;
    
                //
                // Fill in the prototype PTEs for this subsection.
                //
                // Set all the PTEs to the initial execute-read-write protection.
                // The section will control access to these and the segment
                // must provide a method to allow other users to map the file
                // for various protections.
                //
    
                TempPte.u.Long = MiGetSubsectionAddressForPte (Subsection);
                TempPte.u.Soft.Prototype = 1;
    
                TempPte.u.Soft.Protection = Segment->SegmentPteTemplate.u.Soft.Protection;
    
                MiFillMemoryPte (ProtoPtes, PartialSize / sizeof (MMPTE), TempPte.u.Long);
    
            } while (RunningSize < AllocationSize);
    
            ASSERT (ControlArea->DereferenceList.Flink == NULL);
    
            //
            // Link the newly created subsection chain into the existing list.
            // Note that any adjustments (NumberOfFullSectors, etc) made to
            // the temp copy of the last subsection in the existing control
            // area must be *CAREFULLY* copied to the real copy in the chain (the
            // entire structure cannot just be block copied) as other fields
            // in the real copy (ie: NumberOfMappedViews may be changed in
            // parallel by another thread).
            //
    
            Appended = MiAppendSubsectionChain ((PMSUBSECTION)LastSubsection,
                                                &ExtendedSubsectionHead);

            ASSERT (Appended == TRUE);
        }

        TotalNumberOfPtes += RequiredPtes;

        Segment->TotalNumberOfPtes = (ULONG) TotalNumberOfPtes;
        if (TotalNumberOfPtes >= 0x100000000) {
            Segment->SegmentFlags.TotalNumberOfPtes4132 = (ULONG_PTR)(TotalNumberOfPtes >> 32);
        }

        if (LastExtendedSubsection != &ExtendedSubsectionHead) {
            ((PMAPPED_FILE_SEGMENT)Segment)->LastSubsectionHint =
                    LastExtendedSubsection;
        }
    }

    Segment->SizeOfSegment = *(PUINT64)NewSectionSize;
    Section->SizeOfSection = *NewSectionSize;

ReleaseAndReturnSuccess:

    Status = STATUS_SUCCESS;

ReleaseAndReturn:

    ExReleaseResourceLite (&MmSectionExtendResource);
    KeLeaveCriticalRegionThread (CurrentThread);

    return Status;

ExtensionFailed:

    //
    // Required pool to extend the section could not be allocated.
    // Reset the subsection and control area fields to their
    // original values.
    //

    LastSubsection->PtesInSubsection -= PtesUsed;
    LastSubsection->UnusedPtes += PtesUsed;

    TotalNumberOfPtes -= PtesUsed;
    Segment->SegmentFlags.TotalNumberOfPtes4132 = 0;

    Segment->TotalNumberOfPtes = (ULONG) TotalNumberOfPtes;
    if (TotalNumberOfPtes >= 0x100000000) {
        Segment->SegmentFlags.TotalNumberOfPtes4132 = (ULONG_PTR)(TotalNumberOfPtes >> 32);
    }

    Segment->SizeOfSegment -= ((UINT64)PtesUsed * PAGE_SIZE);

    //
    // Free all the previous allocations and return an error.
    //

    LastSubsection = ExtendedSubsectionHead.NextSubsection;

    while (LastSubsection != NULL) {
        Subsection = LastSubsection->NextSubsection;
        if (LastSubsection->SubsectionBase != NULL) {
            ExFreePool (LastSubsection->SubsectionBase);
        }
        ExFreePool (LastSubsection);
        LastSubsection = Subsection;
    }

    Status = STATUS_INSUFFICIENT_RESOURCES;
    goto ReleaseAndReturn;
}

PMMPTE
FASTCALL
MiGetProtoPteAddressExtended (
    IN PMMVAD Vad,
    IN ULONG_PTR Vpn
    )

/*++

Routine Description:

    This function calculates the address of the prototype PTE
    for the corresponding virtual address.

Arguments:

    Vad - Supplies a pointer to the virtual address descriptor which
          encompasses the virtual address.

    Vpn - Supplies the virtual page number to locate a prototype PTE for.

Return Value:

    The corresponding prototype PTE address.

--*/

{
    PSUBSECTION Subsection;
    PCONTROL_AREA ControlArea;
    ULONG PteOffset;

    ControlArea = Vad->ControlArea;

    if (ControlArea->u.Flags.GlobalOnlyPerSession == 0) {
        Subsection = (PSUBSECTION)(ControlArea + 1);
    }
    else {
        Subsection = (PSUBSECTION)((PLARGE_CONTROL_AREA)ControlArea + 1);
    }

    //
    // Locate the subsection which contains the First Prototype PTE
    // for this VAD.
    //

    while ((Subsection->SubsectionBase == NULL) ||
           (Vad->FirstPrototypePte < Subsection->SubsectionBase) ||
           (Vad->FirstPrototypePte >=
               &Subsection->SubsectionBase[Subsection->PtesInSubsection])) {

        //
        // Get the next subsection.
        //

        Subsection = Subsection->NextSubsection;
        if (Subsection == NULL) {
            return NULL;
        }
    }

    ASSERT (Subsection->SubsectionBase != NULL);

    //
    // How many PTEs beyond this subsection must we go?
    //

    PteOffset = (ULONG) (((Vpn - Vad->StartingVpn) +
                 (ULONG)(Vad->FirstPrototypePte - Subsection->SubsectionBase)) -
                 Subsection->PtesInSubsection);

    ASSERT (PteOffset < 0xF0000000);

    PteOffset += Subsection->PtesInSubsection;

    //
    // Locate the subsection which contains the prototype PTEs.
    //

    while (PteOffset >= Subsection->PtesInSubsection) {
        PteOffset -= Subsection->PtesInSubsection;
        Subsection = Subsection->NextSubsection;
        if (Subsection == NULL) {
            return NULL;
        }
    }

    //
    // The PTEs are in this subsection.
    //

    ASSERT (Subsection->SubsectionBase != NULL);

    ASSERT (PteOffset < Subsection->PtesInSubsection);

    return &Subsection->SubsectionBase[PteOffset];

}

PSUBSECTION
FASTCALL
MiLocateSubsection (
    IN PMMVAD Vad,
    IN ULONG_PTR Vpn
    )

/*++

Routine Description:

    This function calculates the address of the subsection
    for the corresponding virtual address.

    This function only works for mapped files NOT mapped images.

Arguments:

    Vad - Supplies a pointer to the virtual address descriptor which
          encompasses the virtual address.

    Vpn - Supplies the virtual page number to locate a prototype PTE for.

Return Value:

    The corresponding prototype subsection.

--*/

{
    PSUBSECTION Subsection;
    PCONTROL_AREA ControlArea;
    ULONG PteOffset;

    ControlArea = Vad->ControlArea;

    if (ControlArea->u.Flags.Rom == 0) {
        Subsection = (PSUBSECTION)(ControlArea + 1);
    }
    else {
        Subsection = (PSUBSECTION)((PLARGE_CONTROL_AREA)ControlArea + 1);
    }

    if (ControlArea->u.Flags.Image) {

        if (ControlArea->u.Flags.GlobalOnlyPerSession == 1) {
            Subsection = (PSUBSECTION)((PLARGE_CONTROL_AREA)ControlArea + 1);
        }

        //
        // There is only one subsection, don't look any further.
        //

        return Subsection;
    }

    ASSERT (ControlArea->u.Flags.GlobalOnlyPerSession == 0);

    //
    // Locate the subsection which contains the First Prototype PTE
    // for this VAD.  Note all the SubsectionBase values must be non-NULL
    // for the subsection range spanned by the VAD because the VAD still
    // exists.  Carefully skip over preceding subsections not mapped by
    // this VAD because if no other VADs map them either, their base
    // can be NULL.
    //

    while ((Subsection->SubsectionBase == NULL) ||
           (Vad->FirstPrototypePte < Subsection->SubsectionBase) ||
           (Vad->FirstPrototypePte >=
               &Subsection->SubsectionBase[Subsection->PtesInSubsection])) {

        //
        // Get the next subsection.
        //

        Subsection = Subsection->NextSubsection;
        if (Subsection == NULL) {
            return NULL;
        }
    }

    ASSERT (Subsection->SubsectionBase != NULL);

    //
    // How many PTEs beyond this subsection must we go?
    //

    PteOffset = (ULONG)((Vpn - Vad->StartingVpn) +
         (ULONG)(Vad->FirstPrototypePte - Subsection->SubsectionBase));

    ASSERT (PteOffset < 0xF0000000);

    //
    // Locate the subsection which contains the prototype PTEs.
    //

    while (PteOffset >= Subsection->PtesInSubsection) {
        PteOffset -= Subsection->PtesInSubsection;
        Subsection = Subsection->NextSubsection;
        if (Subsection == NULL) {
            return NULL;
        }
        ASSERT (Subsection->SubsectionBase != NULL);
    }

    //
    // The PTEs are in this subsection.
    //

    ASSERT (Subsection->SubsectionBase != NULL);

    return Subsection;
}

