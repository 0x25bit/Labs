/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    modwrite.c

Abstract:

    This module contains the modified page writer for memory management.

--*/

#include "mi.h"

typedef enum _MODIFIED_WRITER_OBJECT {
    NormalCase,
    MappedPagesNeedWriting,
    ModifiedWriterMaximumObject
} MODIFIED_WRITER_OBJECT;

typedef struct _MM_WRITE_CLUSTER {
    ULONG Count;
    ULONG StartIndex;
    ULONG Cluster[2 * (MM_MAXIMUM_DISK_IO_SIZE / PAGE_SIZE) + 1];
} MM_WRITE_CLUSTER, *PMM_WRITE_CLUSTER;

LONG MmWriteAllModifiedPages;
LOGICAL MiFirstPageFileCreatedAndReady = FALSE;

LOGICAL MiDrainingMappedWrites = FALSE;

ULONG MmNumberOfMappedMdls;
#if DBG
ULONG MmNumberOfMappedMdlsInUse;
ULONG MmNumberOfMappedMdlsInUsePeak;

typedef struct _MM_MODWRITE_ERRORS {
    NTSTATUS Status;
    ULONG Count;
} MM_MODWRITE_ERRORS, *PMM_MODWRITE_ERRORS;

#define MM_MAX_MODWRITE_ERRORS  8
MM_MODWRITE_ERRORS MiModwriteErrors[MM_MAX_MODWRITE_ERRORS];
#endif

LONG MiClusterWritesDisabled;

#define MI_SLOW_CLUSTER_WRITES   10

#define ONEMB_IN_PAGES  ((1024 * 1024) / PAGE_SIZE)

NTSTATUS MiLastModifiedWriteError;
NTSTATUS MiLastMappedWriteError;

//
// Keep separate counters for the mapped and modified writer threads.  This
// way they can both be read and updated without locks.
//

#define MI_MAXIMUM_PRIORITY_BURST   32

ULONG MiMappedWriteBurstCount;
ULONG MiModifiedWriteBurstCount;

VOID
MiClusterWritePages (
    IN PMMPFN Pfn1,
    IN PFN_NUMBER PageFrameIndex,
    IN PMM_WRITE_CLUSTER WriteCluster,
    IN ULONG Size
    );

VOID
MiExtendPagingFileMaximum (
    IN ULONG PageFileNumber,
    IN PRTL_BITMAP NewBitmap
    );

SIZE_T
MiAttemptPageFileExtension (
    IN ULONG PageFileNumber,
    IN SIZE_T SizeNeeded,
    IN LOGICAL Maximum
    );

NTSTATUS
MiZeroPageFileFirstPage (
    IN PFILE_OBJECT File
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,NtCreatePagingFile)
#pragma alloc_text(PAGE,MmGetPageFileInformation)
#pragma alloc_text(PAGE,MmGetSystemPageFile)
#pragma alloc_text(PAGE,MiLdwPopupWorker)
#pragma alloc_text(PAGE,MiAttemptPageFileExtension)
#pragma alloc_text(PAGE,MiExtendPagingFiles)
#pragma alloc_text(PAGE,MiZeroPageFileFirstPage)
#pragma alloc_text(PAGELK,MiModifiedPageWriter)
#endif


extern POBJECT_TYPE IoFileObjectType;

extern SIZE_T MmSystemCommitReserve;

LIST_ENTRY MmMappedPageWriterList;

KEVENT MmMappedPageWriterEvent;

KEVENT MmMappedFileIoComplete;

ULONG MmSystemShutdown;

BOOLEAN MmSystemPageFileLocated;

NTSTATUS
MiCheckPageFileMapping (
    IN PFILE_OBJECT File
    );

VOID
MiInsertPageFileInList (
    VOID
    );

VOID
MiGatherMappedPages (
    IN KIRQL OldIrql
    );

VOID
MiGatherPagefilePages (
    IN PMMMOD_WRITER_MDL_ENTRY ModWriterEntry,
    IN PRTL_BITMAP Bitmap
    );

VOID
MiPageFileFull (
    VOID
    );

#if DBG
ULONG_PTR MmPagingFileDebug[8192];
#endif

#define MINIMUM_PAGE_FILE_SIZE ((ULONG)(256*PAGE_SIZE))

//
// Log pagefile writes so that scheduling, filesystem and storage stack
// problems can be tracked down.
//

#define MI_TRACK_PAGEFILE_WRITES 0x100

typedef struct _MI_PAGEFILE_TRACES {

    NTSTATUS Status;
    UCHAR Priority;
    UCHAR IrpPriority;
    LARGE_INTEGER CurrentTime;

    PFN_NUMBER AvailablePages;
    PFN_NUMBER ModifiedPagesTotal;
    PFN_NUMBER ModifiedPagefilePages;
    PFN_NUMBER ModifiedNoWritePages;

    PFN_NUMBER MdlHack[(sizeof(MDL)/sizeof(PFN_NUMBER)) + 1];

} MI_PAGEFILE_TRACES, *PMI_PAGEFILE_TRACES;

LONG MiPageFileTraceIndex;

MI_PAGEFILE_TRACES MiPageFileTraces[MI_TRACK_PAGEFILE_WRITES];

VOID
FORCEINLINE
MiSnapPagefileWrite (
    IN PMMMOD_WRITER_MDL_ENTRY ModWriterEntry,
    IN PLARGE_INTEGER CurrentTime,
    IN IO_PAGING_PRIORITY IrpPriority,
    IN NTSTATUS Status
    )
{
    PMI_PAGEFILE_TRACES Information;
    ULONG Index;

    Index = InterlockedIncrement (&MiPageFileTraceIndex);
    Index &= (MI_TRACK_PAGEFILE_WRITES - 1);
    Information = &MiPageFileTraces[Index];

    Information->Status = Status;
    Information->Priority = (UCHAR) KeGetCurrentThread()->Priority;
    Information->IrpPriority = (UCHAR) IrpPriority;
    Information->CurrentTime = *CurrentTime;

    Information->AvailablePages = MmAvailablePages;
    Information->ModifiedPagesTotal = MmModifiedPageListHead.Total;
    Information->ModifiedPagefilePages = MmTotalPagesForPagingFile;
    Information->ModifiedNoWritePages = MmModifiedNoWritePageListHead.Total;

    RtlCopyMemory (Information->MdlHack,
                   &ModWriterEntry->Mdl,
                   sizeof (Information->MdlHack));
}

#if MI_TRACK_PAGEFILE_WRITES
#define MI_PAGEFILE_WRITE(ModWriterEntry,CurrentTime,IrpPriority,Status) \
            MiSnapPagefileWrite(ModWriterEntry,CurrentTime,IrpPriority,Status)
#else
#define MI_PAGEFILE_WRITE(ModWriterEntry,CurrentTime,IrpPriority,Status)
#endif

VOID
MiModifiedPageWriterWorker (
    VOID
    );


VOID
MiReleaseModifiedWriter (
    VOID
    )

/*++

Routine Description:

    Non-pageable wrapper to signal the modified writer when the first pagefile
    creation has completely finished.

--*/

{
    KIRQL OldIrql;

    LOCK_PFN (OldIrql);
    MiFirstPageFileCreatedAndReady = TRUE;
    UNLOCK_PFN (OldIrql);
}

NTSTATUS
MiZeroPageFileFirstPage (
    IN PFILE_OBJECT File
    )

/*++

Routine Description:

    This routine zeroes the first page of the newly created paging file
    to ensure no stale crashdump signatures get to live on.

Arguments:

    File - Supplies a pointer to the file object for the paging file.

Return Value:

    NTSTATUS.

--*/

{
    PMDL Mdl;
    LARGE_INTEGER Offset = {0};
    PULONG Block;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    PPFN_NUMBER Page;
    PFN_NUMBER MdlHack[(sizeof(MDL)/sizeof(PFN_NUMBER)) + 1];
    KEVENT Event;

    Mdl = (PMDL)&MdlHack[0];

    MmCreateMdl (Mdl, NULL, PAGE_SIZE);

    Mdl->MdlFlags |= MDL_PAGES_LOCKED;

    Page = (PPFN_NUMBER)(Mdl + 1);

    *Page = MiGetPageForHeader (FALSE);

    Block = MmGetSystemAddressForMdlSafe (Mdl, HighPagePriority);

    if (Block != NULL) {
        KeZeroSinglePage (Block);
    }
    else {
        MiZeroPhysicalPage (*Page);
    }

    KeInitializeEvent (&Event, NotificationEvent, FALSE);

    Status = IoSynchronousPageWrite (File,
                                     Mdl,
                                     &Offset,
                                     &Event,
                                     &IoStatus);

    if (NT_SUCCESS (Status)) {

        KeWaitForSingleObject (&Event,
                               WrVirtualMemory,
                               KernelMode,
                               FALSE,
                               NULL);

        Status = IoStatus.Status;
    }

    if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) {
        MmUnmapLockedPages (Mdl->MappedSystemVa, Mdl);
    }

    MiRemoveImageHeaderPage (*Page);

    return Status;
}


NTSTATUS
NtCreatePagingFile (
    __in PUNICODE_STRING PageFileName,
    __in PLARGE_INTEGER MinimumSize,
    __in PLARGE_INTEGER MaximumSize,
    __in ULONG Priority
    )

/*++

Routine Description:

    This routine opens the specified file, attempts to write a page
    to the specified file, and creates the necessary structures to
    use the file as a paging file.

    If this file is the first paging file, the modified page writer
    is started.

    This system service requires the caller to have SeCreatePagefilePrivilege.

Arguments:

    PageFileName - Supplies the fully qualified file name.

    MinimumSize - Supplies the starting size of the paging file.
                  This value is rounded up to the host page size.

    MaximumSize - Supplies the maximum number of bytes to write to the file.
                  This value is rounded up to the host page size.

    Priority - Supplies the relative priority of this paging file.

--*/

{
    ULONG i;
    PFILE_OBJECT File;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES PagingFileAttributes;
    HANDLE FileHandle;
    IO_STATUS_BLOCK IoStatus;
    UNICODE_STRING CapturedName;
    PWSTR CapturedBuffer;
    LARGE_INTEGER CapturedMaximumSize;
    LARGE_INTEGER CapturedMinimumSize;
    FILE_END_OF_FILE_INFORMATION EndOfFileInformation;
    KPROCESSOR_MODE PreviousMode;
    FILE_FS_DEVICE_INFORMATION FileDeviceInfo;
    ULONG ReturnedLength;
    ULONG PageFileNumber;
    ULONG NewMaxSizeInPages;
    ULONG NewMinSizeInPages;
    PMMPAGING_FILE FoundExisting;
    PMMPAGING_FILE NewPagingFile;
    PRTL_BITMAP NewBitmap;
    PDEVICE_OBJECT deviceObject;
    MMPAGE_FILE_EXPANSION PageExtend;
    SECURITY_DESCRIPTOR SecurityDescriptor;
    ULONG DaclLength;
    PACL Dacl;


    DBG_UNREFERENCED_PARAMETER (Priority);

    PAGED_CODE();

    CapturedBuffer = NULL;
    Dacl = NULL;

    if (MmNumberOfPagingFiles == MAX_PAGE_FILES) {

        //
        // The maximum number of paging files is already in use.
        //

        return STATUS_TOO_MANY_PAGING_FILES;
    }

    PreviousMode = KeGetPreviousMode();

    if (PreviousMode != KernelMode) {

        //
        // Make sure the caller has the proper privilege for this.
        //

        if (!SeSinglePrivilegeCheck (SeCreatePagefilePrivilege, PreviousMode)) {
            return STATUS_PRIVILEGE_NOT_HELD;
        }

        //
        // Probe arguments.
        //

        try {

#if !defined (_WIN64)

            //
            // Note we only probe for byte alignment because early releases
            // of NT did and we don't want to break user apps
            // that had bad alignment if they worked before.
            //

            ProbeForReadSmallStructure (PageFileName,
                                        sizeof(*PageFileName),
                                        sizeof(UCHAR));
#else
            ProbeForReadSmallStructure (PageFileName,
                                        sizeof(*PageFileName),
                                        PROBE_ALIGNMENT (UNICODE_STRING));
#endif

            ProbeForReadSmallStructure (MaximumSize,
                                        sizeof(LARGE_INTEGER),
                                        PROBE_ALIGNMENT (LARGE_INTEGER));

            ProbeForReadSmallStructure (MinimumSize,
                                        sizeof(LARGE_INTEGER),
                                        PROBE_ALIGNMENT (LARGE_INTEGER));

            //
            // Capture arguments.
            //

            CapturedMinimumSize = *MinimumSize;

        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            // If an exception occurs during the probe or capture
            // of the initial values, then handle the exception and
            // return the exception code as the status value.
            //

            return GetExceptionCode();
        }
    }
    else {

        //
        // Capture arguments.
        //

        CapturedMinimumSize = *MinimumSize;
    }

    if ((CapturedMinimumSize.QuadPart > MI_MAXIMUM_PAGEFILE_SIZE) ||
        (CapturedMinimumSize.LowPart < MINIMUM_PAGE_FILE_SIZE)) {
        return STATUS_INVALID_PARAMETER_2;
    }

    if (PreviousMode != KernelMode) {

        try {
            CapturedMaximumSize = *MaximumSize;
        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            // If an exception occurs during the probe or capture
            // of the initial values, then handle the exception and
            // return the exception code as the status value.
            //

            return GetExceptionCode();
        }
    }
    else {
        CapturedMaximumSize = *MaximumSize;
    }

    if (CapturedMaximumSize.QuadPart > MI_MAXIMUM_PAGEFILE_SIZE) {
        return STATUS_INVALID_PARAMETER_3;
    }

    if (CapturedMinimumSize.QuadPart > CapturedMaximumSize.QuadPart) {
        return STATUS_INVALID_PARAMETER_3;
    }

    if (PreviousMode != KernelMode) {
        try {
            CapturedName = *PageFileName;
        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            // If an exception occurs during the probe or capture
            // of the initial values, then handle the exception and
            // return the exception code as the status value.
            //

            return GetExceptionCode();
        }
    }
    else {
        CapturedName = *PageFileName;
    }

    CapturedName.MaximumLength = CapturedName.Length;

    if ((CapturedName.Length == 0) ||
        (CapturedName.Length > MAXIMUM_FILENAME_LENGTH )) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    CapturedBuffer = ExAllocatePoolWithTag (PagedPool,
                                            (ULONG)CapturedName.Length,
                                            '  mM');

    if (CapturedBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (PreviousMode != KernelMode) {
        try {

            ProbeForRead (CapturedName.Buffer,
                          CapturedName.Length,
                          sizeof (UCHAR));

            //
            // Copy the string to the allocated buffer.
            //

            RtlCopyMemory (CapturedBuffer,
                           CapturedName.Buffer,
                           CapturedName.Length);

        } except (EXCEPTION_EXECUTE_HANDLER) {

            //
            // If an exception occurs during the probe or capture
            // of the initial values, then handle the exception and
            // return the exception code as the status value.
            //

            ExFreePool (CapturedBuffer);

            return GetExceptionCode();
        }
    }
    else {

        //
        // Copy the string to the allocated buffer.
        //

        RtlCopyMemory (CapturedBuffer,
                       CapturedName.Buffer,
                       CapturedName.Length);
    }

    //
    // Point the buffer to the string that was just copied.
    //

    CapturedName.Buffer = CapturedBuffer;

    //
    // Create a security descriptor to protect the pagefile
    //
    Status = RtlCreateSecurityDescriptor (&SecurityDescriptor,
                                          SECURITY_DESCRIPTOR_REVISION);

    if (!NT_SUCCESS (Status)) {
        goto ErrorReturn1;
    }
    DaclLength = sizeof (ACL) + sizeof (ACCESS_ALLOWED_ACE) * 2 +
                 RtlLengthSid (SeLocalSystemSid) +
                 RtlLengthSid (SeAliasAdminsSid);

    Dacl = ExAllocatePoolWithTag (PagedPool, DaclLength, 'lcaD');

    if (Dacl == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ErrorReturn1;
    }

    Status = RtlCreateAcl (Dacl, DaclLength, ACL_REVISION);

    if (!NT_SUCCESS (Status)) {
        goto ErrorReturn1;
    }

    Status = RtlAddAccessAllowedAce (Dacl,
                                     ACL_REVISION,
                                     FILE_ALL_ACCESS,
                                     SeAliasAdminsSid);

    if (!NT_SUCCESS (Status)) {
        goto ErrorReturn1;
    }

    Status = RtlAddAccessAllowedAce (Dacl,
                                     ACL_REVISION,
                                     FILE_ALL_ACCESS,
                                     SeLocalSystemSid);

    if (!NT_SUCCESS (Status)) {
        goto ErrorReturn1;
    }
  
    Status = RtlSetDaclSecurityDescriptor (&SecurityDescriptor,
                                           TRUE,
                                           Dacl,
                                           FALSE);

    if (!NT_SUCCESS (Status)) {
        goto ErrorReturn1;
    }
  

    //
    // Open a paging file and get the size.
    //

    InitializeObjectAttributes (&PagingFileAttributes,
                                &CapturedName,
                                (OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE),
                                NULL,
                                &SecurityDescriptor);

//
// Note this macro cannot use ULONG_PTR as it must also work on PAE.
//

#define ROUND64_TO_PAGES(Size)  (((ULONG64)(Size) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

    EndOfFileInformation.EndOfFile.QuadPart =
                                ROUND64_TO_PAGES (CapturedMinimumSize.QuadPart);

    Status = IoCreateFile (&FileHandle,
                           FILE_READ_DATA | FILE_WRITE_DATA | WRITE_DAC | SYNCHRONIZE,
                           &PagingFileAttributes,
                           &IoStatus,
                           &CapturedMinimumSize,
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM,
                           FILE_SHARE_WRITE,
                           FILE_SUPERSEDE,
                           FILE_NO_INTERMEDIATE_BUFFERING | FILE_NO_COMPRESSION | FILE_DELETE_ON_CLOSE,
                           NULL,
                           0L,
                           CreateFileTypeNone,
                           NULL,
                           IO_OPEN_PAGING_FILE | IO_NO_PARAMETER_CHECKING);

    if (NT_SUCCESS(Status)) {

        //
        // Update the DACL in case there was a pre-existing regular file named
        // pagefile.sys (even supersede above does not do this).
        //

        if (NT_SUCCESS(IoStatus.Status)) {

            Status = ZwSetSecurityObject (FileHandle,
                                          DACL_SECURITY_INFORMATION,
                                          &SecurityDescriptor);

            if (!NT_SUCCESS(Status)) {
                goto ErrorReturn2;
            }
        }
    }
    else {

        //
        // Treat this as an extension of an existing pagefile maximum -
        // and try to open rather than create the paging file specified.
        //

        Status = IoCreateFile (&FileHandle,
                           FILE_WRITE_DATA | SYNCHRONIZE,
                           &PagingFileAttributes,
                           &IoStatus,
                           &CapturedMinimumSize,
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           FILE_OPEN,
                           FILE_NO_INTERMEDIATE_BUFFERING | FILE_NO_COMPRESSION,
                           (PVOID) NULL,
                           0L,
                           CreateFileTypeNone,
                           (PVOID) NULL,
                           IO_OPEN_PAGING_FILE | IO_NO_PARAMETER_CHECKING);

        if (!NT_SUCCESS(Status)) {

#if DBG
            if (Status != STATUS_DISK_FULL) {
                DbgPrintEx (DPFLTR_MM_ID, DPFLTR_INFO_LEVEL, 
                    "MM MODWRITE: unable to open paging file %wZ - status = %X \n", &CapturedName, Status);
            }
#endif

            goto ErrorReturn1;
        }

        Status = ObReferenceObjectByHandle (FileHandle,
                                            FILE_READ_DATA | FILE_WRITE_DATA,
                                            IoFileObjectType,
                                            KernelMode,
                                            (PVOID *)&File,
                                            NULL);

        if (!NT_SUCCESS(Status)) {
            goto ErrorReturn2;
        }

        FoundExisting = NULL;

        KeAcquireGuardedMutex (&MmPageFileCreationLock);

        for (PageFileNumber = 0; PageFileNumber < MmNumberOfPagingFiles; PageFileNumber += 1) {
            if (MmPagingFile[PageFileNumber]->File->SectionObjectPointer == File->SectionObjectPointer) {
                FoundExisting = MmPagingFile[PageFileNumber];
                break;
            }
        }

        if (FoundExisting == NULL) {
            Status = STATUS_NOT_FOUND;
            goto ErrorReturn4;
        }

        //
        // Check for increases in the minimum or the maximum paging file sizes.
        // Decreasing either paging file size on the fly is not allowed.
        //

        NewMaxSizeInPages = (ULONG)(CapturedMaximumSize.QuadPart >> PAGE_SHIFT);
        NewMinSizeInPages = (ULONG)(CapturedMinimumSize.QuadPart >> PAGE_SHIFT);

        if (FoundExisting->MinimumSize > NewMinSizeInPages) {
            Status = STATUS_INVALID_PARAMETER_2;
            goto ErrorReturn4;
        }

        if (FoundExisting->MaximumSize > NewMaxSizeInPages) {
            Status = STATUS_INVALID_PARAMETER_3;
            goto ErrorReturn4;
        }

        if (NewMaxSizeInPages > FoundExisting->MaximumSize) {

            //
            // Make sure that the pagefile increase doesn't cause the commit
            // limit (in pages) to wrap.  Currently this can only happen on
            // PAE systems where 16 pagefiles of 16TB (==256TB) is greater
            // than the 32-bit commit variable (max is 16TB).
            //

            if (MmTotalCommitLimitMaximum + (NewMaxSizeInPages - FoundExisting->MaximumSize) <= MmTotalCommitLimitMaximum) {
                Status = STATUS_INVALID_PARAMETER_3;
                goto ErrorReturn4;
            }

            //
            // Handle the increase to the maximum paging file size.
            //

            MiCreateBitMap (&NewBitmap, NewMaxSizeInPages, NonPagedPool);

            if (NewBitmap == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto ErrorReturn4;
            }

            MiExtendPagingFileMaximum (PageFileNumber, NewBitmap);

            //
            // We may be low on commitment and/or may have put a temporary
            // stopgate on things.  Clear up the logjam now by forcing an
            // extension and immediately returning it.
            //

            if (MmTotalCommittedPages + 100 > MmTotalCommitLimit) {
                if (MiChargeCommitment (200, NULL) == TRUE) {
                    MiReturnCommitment (200);
                }
            }
        }

        if (NewMinSizeInPages > FoundExisting->MinimumSize) {

            //
            // Handle the increase to the minimum paging file size.
            //

            if (NewMinSizeInPages > FoundExisting->Size) {

                //
                // Queue a message to the segment dereferencing / pagefile
                // extending thread to see if the page file can be extended.
                //

                PageExtend.InProgress = 1;
                PageExtend.ActualExpansion = 0;
                PageExtend.RequestedExpansionSize = NewMinSizeInPages - FoundExisting->Size;
                PageExtend.Segment = NULL;
                PageExtend.PageFileNumber = PageFileNumber;
                KeInitializeEvent (&PageExtend.Event, NotificationEvent, FALSE);

                MiIssuePageExtendRequest (&PageExtend);
            }

            //
            // The current size is now greater than the new desired minimum.
            // Ensure subsequent contractions obey this new minimum.
            //

            if (FoundExisting->Size >= NewMinSizeInPages) {
                ASSERT (FoundExisting->Size >= FoundExisting->MinimumSize);
                ASSERT (NewMinSizeInPages >= FoundExisting->MinimumSize);
                FoundExisting->MinimumSize = NewMinSizeInPages;
            }
            else {

                //
                // The pagefile could not be expanded to handle the new minimum.
                // No easy way to undo any maximum raising that may have been
                // done as the space may have already been used, so just set
                // Status so our caller knows it didn't all go perfectly.
                //

                Status = STATUS_INSUFFICIENT_RESOURCES;
            }
        }

        goto ErrorReturn4;
    }

    //
    // Free the DACL as it's no longer needed.
    //

    ExFreePool (Dacl);
    Dacl = NULL;

    if (!NT_SUCCESS(IoStatus.Status)) {
        KdPrint(("MM MODWRITE: unable to open paging file %wZ - iosb %lx\n", &CapturedName, IoStatus.Status));
        Status = IoStatus.Status;
        goto ErrorReturn1;
    }

    //
    // Make sure that the pagefile increase doesn't cause the commit
    // limit (in pages) to wrap.  Currently this can only happen on
    // PAE systems where 16 pagefiles of 16TB (==256TB) is greater
    // than the 32-bit commit variable (max is 16TB).
    //

    if (MmTotalCommitLimitMaximum + (CapturedMaximumSize.QuadPart >> PAGE_SHIFT)
        <= MmTotalCommitLimitMaximum) {
        Status = STATUS_INVALID_PARAMETER_3;
        goto ErrorReturn2;
    }

    Status = ZwSetInformationFile (FileHandle,
                                   &IoStatus,
                                   &EndOfFileInformation,
                                   sizeof(EndOfFileInformation),
                                   FileEndOfFileInformation);

    if (!NT_SUCCESS(Status)) {
        KdPrint(("MM MODWRITE: unable to set length of paging file %wZ status = %X \n",
                 &CapturedName, Status));
        goto ErrorReturn2;
    }

    if (!NT_SUCCESS(IoStatus.Status)) {
        KdPrint(("MM MODWRITE: unable to set length of paging file %wZ - iosb %lx\n",
                &CapturedName, IoStatus.Status));
        Status = IoStatus.Status;
        goto ErrorReturn2;
    }

    Status = ObReferenceObjectByHandle ( FileHandle,
                                         FILE_READ_DATA | FILE_WRITE_DATA,
                                         IoFileObjectType,
                                         KernelMode,
                                         (PVOID *)&File,
                                         NULL );

    if (!NT_SUCCESS(Status)) {
        KdPrint(("MM MODWRITE: Unable to reference paging file - %wZ\n",
                 &CapturedName));
        goto ErrorReturn2;
    }

    //
    // Get the address of the target device object and ensure
    // the specified file is of a suitable type.
    //

    deviceObject = IoGetRelatedDeviceObject (File);

    if ((deviceObject->DeviceType != FILE_DEVICE_DISK_FILE_SYSTEM) &&
        (deviceObject->DeviceType != FILE_DEVICE_NETWORK_FILE_SYSTEM) &&
        (deviceObject->DeviceType != FILE_DEVICE_DFS_VOLUME) &&
        (deviceObject->DeviceType != FILE_DEVICE_DFS_FILE_SYSTEM)) {
            KdPrint(("MM MODWRITE: Invalid paging file type - %x\n",
                     deviceObject->DeviceType));
            Status = STATUS_UNRECOGNIZED_VOLUME;
            goto ErrorReturn3;
    }

    //
    // Make sure the specified file is not currently being used
    // as a mapped data file.
    //

    Status = MiCheckPageFileMapping (File);
    if (!NT_SUCCESS(Status)) {
        goto ErrorReturn3;
    }

    //
    // Make sure the volume is not a floppy disk.
    //

    Status = IoQueryVolumeInformation ( File,
                                        FileFsDeviceInformation,
                                        sizeof(FILE_FS_DEVICE_INFORMATION),
                                        &FileDeviceInfo,
                                        &ReturnedLength
                                      );

    if (FILE_FLOPPY_DISKETTE & FileDeviceInfo.Characteristics) {
        Status = STATUS_FLOPPY_VOLUME;
        goto ErrorReturn3;
    }

    //
    // Check with all of the drivers along the path to the file to ensure
    // that they are willing to follow the rules required of them and to
    // give them a chance to lock down code and data that needs to be locked.
    // If any of the drivers along the path refuses to participate, fail the
    // pagefile creation.
    //

    Status = PpPagePathAssign (File);

    if (!NT_SUCCESS(Status)) {
        KdPrint(( "PpPagePathAssign(%wZ) FAILED: %x\n", &CapturedName, Status ));
        //
        // Fail the pagefile creation if the storage stack tells us to.
        //

        goto ErrorReturn3;
    }

    NewPagingFile = ExAllocatePoolWithTag (NonPagedPool,
                                           sizeof(MMPAGING_FILE),
                                           '  mM');

    if (NewPagingFile == NULL) {

        //
        // Allocate pool failed.
        //

        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ErrorReturn3;
    }

    RtlZeroMemory (NewPagingFile, sizeof(MMPAGING_FILE));

    NewPagingFile->File = File;
    NewPagingFile->FileHandle = FileHandle;
    NewPagingFile->Size = (PFN_NUMBER)(CapturedMinimumSize.QuadPart >> PAGE_SHIFT);
    NewPagingFile->MinimumSize = NewPagingFile->Size;
    NewPagingFile->FreeSpace = NewPagingFile->Size - 1;

    NewPagingFile->MaximumSize = (PFN_NUMBER)(CapturedMaximumSize.QuadPart >>
                                                PAGE_SHIFT);

    for (i = 0; i < MM_PAGING_FILE_MDLS; i += 1) {

        NewPagingFile->Entry[i] = ExAllocatePoolWithTag (NonPagedPool,
                                            sizeof(MMMOD_WRITER_MDL_ENTRY) +
                                            MmModifiedWriteClusterSize *
                                            sizeof(PFN_NUMBER),
                                            '  mM');

        if (NewPagingFile->Entry[i] == NULL) {

            //
            // Allocate pool failed.
            //

            while (i != 0) {
                i -= 1;
                ExFreePool (NewPagingFile->Entry[i]);
            }

            ExFreePool (NewPagingFile);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto ErrorReturn3;
        }

        RtlZeroMemory (NewPagingFile->Entry[i], sizeof(MMMOD_WRITER_MDL_ENTRY));

        NewPagingFile->Entry[i]->PagingListHead = &MmPagingFileHeader;

        NewPagingFile->Entry[i]->PagingFile = NewPagingFile;
    }

    NewPagingFile->PageFileName = CapturedName;

    MiCreateBitMap (&NewPagingFile->Bitmap,
                    NewPagingFile->MaximumSize,
                    NonPagedPool);

    if (NewPagingFile->Bitmap == NULL) {

        //
        // Allocate pool failed.
        //

        ExFreePool (NewPagingFile->Entry[0]);
        ExFreePool (NewPagingFile->Entry[1]);
        ExFreePool (NewPagingFile);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ErrorReturn3;
    }

    Status = MiZeroPageFileFirstPage (File);

    if (!NT_SUCCESS (Status)) {

        //
        // The storage stack could not zero the first page of the file.
        // This means an old crashdump signature could still be around so
        // fail the create.
        //

        for (i = 0; i < MM_PAGING_FILE_MDLS; i += 1) {
            ExFreePool (NewPagingFile->Entry[i]);
        }
        ExFreePool (NewPagingFile);
        MiRemoveBitMap (&NewPagingFile->Bitmap);
        goto ErrorReturn3;
    }

    RtlSetAllBits (NewPagingFile->Bitmap);

    //
    // Set the first bit as 0 is an invalid page location, clear the
    // following bits.
    //

    RtlClearBits (NewPagingFile->Bitmap,
                  1,
                  (ULONG)(NewPagingFile->Size - 1));

    //
    // See if this pagefile is on the boot partition, and if so, mark it
    // so we can find it later if someone enables crashdump.
    //

    if (File->DeviceObject->Flags & DO_SYSTEM_BOOT_PARTITION) {
        NewPagingFile->BootPartition = 1;
    }
    else {
        NewPagingFile->BootPartition = 0;
    }

    //
    // Acquire the global page file creation mutex.
    //

    KeAcquireGuardedMutex (&MmPageFileCreationLock);

    PageFileNumber = MmNumberOfPagingFiles;

    MmPagingFile[PageFileNumber] = NewPagingFile;

    NewPagingFile->PageFileNumber = PageFileNumber;

    MiInsertPageFileInList ();

    if (PageFileNumber == 0) {

        //
        // The first paging file has been created and reservation of any
        // crashdump pages has completed, signal the modified
        // page writer.
        //

        MiReleaseModifiedWriter ();
    }

    KeReleaseGuardedMutex (&MmPageFileCreationLock);

    //
    // Note that the file handle (a kernel handle) is not closed during the
    // create path (it IS duped and closed in the pagefile size extending path)
    // to prevent the paging file from being deleted or opened again.  It is
    // also kept open so that extensions of existing pagefiles can be detected
    // because successive IoCreateFile calls will fail.
    //

    if ((!MmSystemPageFileLocated) &&
        (File->DeviceObject->Flags & DO_SYSTEM_BOOT_PARTITION)) {
        MmSystemPageFileLocated = IoInitializeCrashDump (FileHandle);
    }

    return STATUS_SUCCESS;

    //
    // Error returns:
    //

ErrorReturn4:
    KeReleaseGuardedMutex (&MmPageFileCreationLock);

ErrorReturn3:
    ObDereferenceObject (File);

ErrorReturn2:
    ZwClose (FileHandle);

ErrorReturn1:
    if (Dacl != NULL) {
        ExFreePool (Dacl);
    }

    ExFreePool (CapturedBuffer);

    return Status;
}


HANDLE
MmGetSystemPageFile (
    VOID
    )
/*++

Routine Description:

    Returns a filehandle to the paging file on the system boot partition.
    This is used by crashdump to enable crashdump after the system has
    already booted.

Arguments:

    None.

Return Value:

    A file handle to the paging file on the system boot partition,
    NULL if no such pagefile exists.

--*/

{
    HANDLE FileHandle;
    ULONG PageFileNumber;

    PAGED_CODE();

    FileHandle = NULL;

    KeAcquireGuardedMutex (&MmPageFileCreationLock);
    for (PageFileNumber = 0; PageFileNumber < MmNumberOfPagingFiles; PageFileNumber += 1) {
        if (MmPagingFile[PageFileNumber]->BootPartition) {
            FileHandle = MmPagingFile[PageFileNumber]->FileHandle;
        }
    }
    KeReleaseGuardedMutex (&MmPageFileCreationLock);

    return FileHandle;
}


LOGICAL
MmIsFileObjectAPagingFile (
    IN PFILE_OBJECT FileObject
    )
/*++

Routine Description:

    Returns TRUE if the file object refers to a paging file, FALSE if not.

Arguments:

    FileObject - Supplies the file object in question.

Return Value:

    Returns TRUE if the file object refers to a paging file, FALSE if not.

    Note this routine is called both at DISPATCH_LEVEL by drivers in their
    completion routines and it is called in the path to satisfy pagefile reads,
    so it cannot be made pageable.

--*/

{
    PMMPAGING_FILE PageFile;
    PMMPAGING_FILE *PagingFile;
    PMMPAGING_FILE *PagingFileEnd;

    //
    // It's ok to check without synchronization.
    //

    PagingFile = MmPagingFile;
    PagingFileEnd = PagingFile + MmNumberOfPagingFiles;

    while (PagingFile < PagingFileEnd) {
        PageFile = *PagingFile;
        if (PageFile->File == FileObject) {
            return TRUE;
        }
        PagingFile += 1;
    }

    return FALSE;
}


VOID
MiExtendPagingFileMaximum (
    IN ULONG PageFileNumber,
    IN PRTL_BITMAP NewBitmap
    )

/*++

Routine Description:

    This routine switches from the old bitmap to the new (larger) bitmap.

Arguments:

    PageFileNumber - Supplies the paging file number to be extended.

    NewBitmap - Supplies the new bitmap to use.

Return Value:

    Returns a non-NULL value for the caller to free to pool

Environment:

    Kernel mode, APC_LEVEL, MmPageFileCreationLock held.

--*/

{
    KIRQL OldIrql;
    PRTL_BITMAP OldBitmap;
    SIZE_T Delta;

    OldBitmap = MmPagingFile[PageFileNumber]->Bitmap;

    RtlSetAllBits (NewBitmap);

    LOCK_PFN (OldIrql);

    //
    // Copy the bits from the existing map.
    //

    RtlCopyMemory (NewBitmap->Buffer,
                   OldBitmap->Buffer,
                   ((OldBitmap->SizeOfBitMap + 31) / 32) * sizeof (ULONG));

    Delta = NewBitmap->SizeOfBitMap - OldBitmap->SizeOfBitMap;

    InterlockedExchangeAddSizeT (&MmTotalCommitLimitMaximum, Delta);

    MmPagingFile[PageFileNumber]->MaximumSize = NewBitmap->SizeOfBitMap;

    MmPagingFile[PageFileNumber]->Bitmap = NewBitmap;

    //
    // If any MDLs are waiting for space, get them up now.
    //

    if (!IsListEmpty (&MmFreePagingSpaceLow)) {
        MiUpdateModifiedWriterMdls (PageFileNumber);
    }

    //
    // The modified writer may be scanning the old map without holding any
    // locks - if so, he will have reference counted the old bitmap, so only
    // free it if this isn't in progress.  If it is in progress, the modified
    // writer will free the old bitmap.
    //

    if (MmPagingFile[PageFileNumber]->ReferenceCount != 0) {
        ASSERT (MmPagingFile[PageFileNumber]->ReferenceCount == 1);
        OldBitmap = NULL;
    }

    UNLOCK_PFN (OldIrql);

    if (OldBitmap != NULL) {
        MiRemoveBitMap (&OldBitmap);
    }
}


VOID
MiFinishPageFileExtension (
    IN ULONG PageFileNumber,
    IN PFN_NUMBER AdditionalAllocation
    )

/*++

Routine Description:

    This routine finishes the specified page file extension.

Arguments:

    PageFileNumber - Supplies the page file number to attempt to extend.

    SizeNeeded - Supplies the number of pages to extend the file by.

    Maximum - Supplies TRUE if the page file should be extended
              by the maximum size possible, but not to exceed
              SizeNeeded.

Return Value:

    None.

--*/

{
    KIRQL OldIrql;
    PMMPAGING_FILE PagingFile;

    //
    // Clear bits within the paging file bitmap to allow the extension
    // to take effect.
    //

    PagingFile = MmPagingFile[PageFileNumber];

    LOCK_PFN (OldIrql);

    ASSERT (RtlCheckBit (PagingFile->Bitmap, (ULONG) PagingFile->Size) == 1);

    RtlClearBits (PagingFile->Bitmap,
                  (ULONG)PagingFile->Size,
                  (ULONG)AdditionalAllocation);

    PagingFile->Size += AdditionalAllocation;
    PagingFile->FreeSpace += AdditionalAllocation;

    MiUpdateModifiedWriterMdls (PageFileNumber);

    UNLOCK_PFN (OldIrql);

    return;
}


SIZE_T
MiAttemptPageFileExtension (
    IN ULONG PageFileNumber,
    IN SIZE_T SizeNeeded,
    IN LOGICAL Maximum
    )

/*++

Routine Description:

    This routine attempts to extend the specified page file by SizeNeeded.

Arguments:

    PageFileNumber - Supplies the page file number to attempt to extend.

    SizeNeeded - Supplies the number of pages to extend the file by.

    Maximum - Supplies TRUE if the page file should be extended
              by the maximum size possible, but not to exceed
              SizeNeeded.

Return Value:

    Returns the size of the extension.  Zero if the page file cannot
    be extended.

--*/

{

    NTSTATUS status;
    FILE_FS_SIZE_INFORMATION FileInfo;
    FILE_END_OF_FILE_INFORMATION EndOfFileInformation;
    ULONG AllocSize;
    ULONG ReturnedLength;
    PFN_NUMBER PagesAvailable;
    SIZE_T SizeToExtend;
    SIZE_T MinimumExtension;
    LARGE_INTEGER BytesAvailable;

    //
    // Check to see if this page file is at the maximum.
    //

    if (MmPagingFile[PageFileNumber]->Size ==
                                    MmPagingFile[PageFileNumber]->MaximumSize) {
        return 0;
    }

    //
    // Find out how much free space is on this volume.
    //

    status = IoQueryVolumeInformation (MmPagingFile[PageFileNumber]->File,
                                       FileFsSizeInformation,
                                       sizeof(FileInfo),
                                       &FileInfo,
                                       &ReturnedLength);

    if (!NT_SUCCESS (status)) {

        //
        // The volume query did not succeed - return 0 indicating
        // the paging file was not extended.
        //

        return 0;
    }

    //
    // Attempt to extend by at least 16 megabytes, if that fails then attempt
    // for at least a megabyte.
    //

    MinimumExtension = MmPageFileExtension << 4;

retry:

    SizeToExtend = SizeNeeded;

    if (SizeNeeded < MinimumExtension) {
        SizeToExtend = MinimumExtension;
    }
    else {
        MinimumExtension = MmPageFileExtension;
    }

    //
    // Don't go over the maximum size for the paging file.
    //

    ASSERT (MmPagingFile[PageFileNumber]->MaximumSize >= MmPagingFile[PageFileNumber]->Size);

    PagesAvailable = MmPagingFile[PageFileNumber]->MaximumSize -
                     MmPagingFile[PageFileNumber]->Size;

    if (SizeToExtend > PagesAvailable) {
        SizeToExtend = PagesAvailable;

        if ((SizeToExtend < SizeNeeded) && (Maximum == FALSE)) {

            //
            // Can't meet the requested (mandatory) requirement.
            //

            return 0;
        }
    }

    //
    // See if there is enough space on the volume for the extension.
    //

    AllocSize = FileInfo.SectorsPerAllocationUnit * FileInfo.BytesPerSector;

    BytesAvailable = RtlExtendedIntegerMultiply (
                        FileInfo.AvailableAllocationUnits,
                        AllocSize);

    if ((UINT64)BytesAvailable.QuadPart > (UINT64)MmMinimumFreeDiskSpace) {

        BytesAvailable.QuadPart = BytesAvailable.QuadPart -
                                    (LONGLONG)MmMinimumFreeDiskSpace;

        if ((UINT64)BytesAvailable.QuadPart > (((UINT64)SizeToExtend) << PAGE_SHIFT)) {
            BytesAvailable.QuadPart = (((LONGLONG)SizeToExtend) << PAGE_SHIFT);
        }

        PagesAvailable = (PFN_NUMBER)(BytesAvailable.QuadPart >> PAGE_SHIFT);

        if ((Maximum == FALSE) && (PagesAvailable < SizeNeeded)) {

            //
            // Can't meet the requested (mandatory) requirement.
            //

            return 0;
        }

    }
    else {

        //
        // Not enough space is available period.
        //

        return 0;
    }

#if defined (_WIN64) || defined (_X86PAE_)
    EndOfFileInformation.EndOfFile.QuadPart =
              ((ULONG64)MmPagingFile[PageFileNumber]->Size + PagesAvailable) * PAGE_SIZE;
#else
    EndOfFileInformation.EndOfFile.LowPart =
              (MmPagingFile[PageFileNumber]->Size + PagesAvailable) * PAGE_SIZE;

    //
    // Set high part to zero as paging files are limited to 4GB.
    //

    EndOfFileInformation.EndOfFile.HighPart = 0;
#endif

    //
    // Attempt to extend the file by setting the end-of-file position.
    //

    ASSERT (KeGetCurrentIrql() < DISPATCH_LEVEL);

    status = IoSetInformation (MmPagingFile[PageFileNumber]->File,
                               FileEndOfFileInformation,
                               sizeof(FILE_END_OF_FILE_INFORMATION),
                               &EndOfFileInformation);

    if (status != STATUS_SUCCESS) {
        KdPrint(("MM MODWRITE: page file extension failed %p %lx\n",PagesAvailable,status));

        if (MinimumExtension != MmPageFileExtension) {
            MinimumExtension = MmPageFileExtension;
            goto retry;
        }

        return 0;
    }

    MiFinishPageFileExtension (PageFileNumber, PagesAvailable);

    return PagesAvailable;
}

SIZE_T
MiExtendPagingFiles (
    IN PMMPAGE_FILE_EXPANSION PageExpand
    )

/*++

Routine Description:

    This routine attempts to extend the paging files to provide
    SizeNeeded bytes.

    Note - Page file expansion and page file reduction are synchronized
           because a single thread is responsible for performing the
           operation.  Hence, while expansion is occurring, a reduction
           request will be queued to the thread.

Arguments:

    PageFileNumber - Supplies the page file number to extend.
                     MI_EXTEND_ANY_PAGFILE indicates to extend any page file.

Return Value:

    Returns the size of the extension.  Zero if the page file(s) cannot
    be extended.

--*/

{
    SIZE_T DesiredQuota;
    ULONG PageFileNumber;
    SIZE_T ExtendedSize;
    SIZE_T SizeNeeded;
    ULONG i;
    SIZE_T CommitLimit;
    SIZE_T CommittedPages;

    DesiredQuota = PageExpand->RequestedExpansionSize;
    PageFileNumber = PageExpand->PageFileNumber;

    ASSERT (PageExpand->ActualExpansion == 0);

    ASSERT (PageFileNumber < MmNumberOfPagingFiles || PageFileNumber == MI_EXTEND_ANY_PAGEFILE);

    if (MmNumberOfPagingFiles == 0) {
        InterlockedExchange ((PLONG)&PageExpand->InProgress, 0);
        return 0;
    }

    if (PageFileNumber < MmNumberOfPagingFiles) {
        i = PageFileNumber;
        ExtendedSize = MmPagingFile[i]->MaximumSize - MmPagingFile[i]->Size;
        if (ExtendedSize < DesiredQuota) {
            InterlockedExchange ((PLONG)&PageExpand->InProgress, 0);
            return 0;
        }

        ExtendedSize = MiAttemptPageFileExtension (i, DesiredQuota, FALSE);
        goto alldone;
    }

    //
    // Snap the globals into locals so calculations will be consistent from
    // step to step.  It is ok to snap the globals unsynchronized with respect
    // to each other as even when pagefile expansion occurs, the expansion
    // space is not reserved for the caller - any process could consume
    // the expansion prior to this routine returning.
    //

    CommittedPages = MmTotalCommittedPages;
    CommitLimit = MmTotalCommitLimit;

    SizeNeeded = CommittedPages + DesiredQuota + MmSystemCommitReserve;

    //
    // Check to make sure the request does not wrap.
    //

    if (SizeNeeded < CommittedPages) {
        InterlockedExchange ((PLONG)&PageExpand->InProgress, 0);
        return 0;
    }

    //
    // Check to see if ample space already exists.
    //

    if (SizeNeeded <= CommitLimit) {
        PageExpand->ActualExpansion = 1;
        InterlockedExchange ((PLONG)&PageExpand->InProgress, 0);
        return 1;
    }

    //
    // Calculate the additional pages needed.
    //

    SizeNeeded -= CommitLimit;
    if (SizeNeeded > MmSystemCommitReserve) {
        SizeNeeded -= MmSystemCommitReserve;
    }

    //
    // Make sure ample space exists within the paging files.
    //

    i = 0;
    ExtendedSize = 0;

    do {
        ExtendedSize += MmPagingFile[i]->MaximumSize - MmPagingFile[i]->Size;
        i += 1;
    } while (i < MmNumberOfPagingFiles);

    if (ExtendedSize < SizeNeeded) {
        InterlockedExchange ((PLONG)&PageExpand->InProgress, 0);
        return 0;
    }

    //
    // Attempt to extend only one of the paging files.
    //

    i = 0;
    do {
        ExtendedSize = MiAttemptPageFileExtension (i, SizeNeeded, FALSE);
        if (ExtendedSize != 0) {
            goto alldone;
        }
        i += 1;
    } while (i < MmNumberOfPagingFiles);

    ASSERT (ExtendedSize == 0);

    if (MmNumberOfPagingFiles == 1) {

        //
        // If the attempt didn't succeed for one (not enough disk space free) -
        // don't try to set it to the maximum size.
        //

        InterlockedExchange ((PLONG)&PageExpand->InProgress, 0);
        return 0;
    }

    //
    // Attempt to extend all paging files.
    //

    i = 0;
    do {
        ASSERT (SizeNeeded > ExtendedSize);
        ExtendedSize += MiAttemptPageFileExtension (i,
                                                    SizeNeeded - ExtendedSize,
                                                    TRUE);
        if (ExtendedSize >= SizeNeeded) {
            goto alldone;
        }
        i += 1;
    } while (i < MmNumberOfPagingFiles);

    //
    // Not enough space is available.
    //

    InterlockedExchange ((PLONG)&PageExpand->InProgress, 0);
    return 0;

alldone:

    ASSERT (ExtendedSize != 0);

    PageExpand->ActualExpansion = ExtendedSize;

    //
    // Increase the systemwide commit limit.
    //

    InterlockedExchangeAddSizeT (&MmTotalCommitLimit, ExtendedSize);

    //
    // Clear the in progress flag - if this is the global cantexpand structure
    // it is possible for it to be immediately reused.
    //

    InterlockedExchange ((PLONG)&PageExpand->InProgress, 0);

    return ExtendedSize;
}

MMPAGE_FILE_EXPANSION MiPageFileContract;


VOID
MiContractPagingFiles (
    VOID
    )

/*++

Routine Description:

    This routine checks to see if ample space is no longer committed
    and if so, does enough free space exist in any paging file.

    IFF the answer to both these is affirmative, a reduction in the
    paging file size(s) is attempted.

Arguments:

    None.

Return Value:

    None.

--*/

{
    ULONG i;
    KIRQL OldIrql;
    PMMPAGE_FILE_EXPANSION PageReduce;

    //
    // This is an unsynchronized check but that's ok.  The real check is
    // made when the packet below is processed by the dereference thread.
    //

    if (MmTotalCommittedPages >= ((MmTotalCommitLimit/10)*8)) {
        return;
    }

    if ((MmTotalCommitLimit - MmMinimumPageFileReduction) <=
                                                       MmTotalCommittedPages) {
        return;
    }

    for (i = 0; i < MmNumberOfPagingFiles; i += 1) {
        if (MmPagingFile[i]->Size != MmPagingFile[i]->MinimumSize) {
            if (MmPagingFile[i]->FreeSpace > MmMinimumPageFileReduction) {
                break;
            }
        }
    }

    if (i == MmNumberOfPagingFiles) {
        return;
    }

    PageReduce = &MiPageFileContract;

    //
    // See if the page file contraction item is already queued up, if so then
    // nothing more needs to be done.
    //

    ExAcquireSpinLock (&MmDereferenceSegmentHeader.Lock, &OldIrql);

    if (PageReduce->RequestedExpansionSize == MI_CONTRACT_PAGEFILES) {

        ExReleaseSpinLock (&MmDereferenceSegmentHeader.Lock, OldIrql);

        return;
    }

    PageReduce->Segment = NULL;
    PageReduce->RequestedExpansionSize = MI_CONTRACT_PAGEFILES;

    InsertTailList (&MmDereferenceSegmentHeader.ListHead,
                    &PageReduce->DereferenceList);

    ExReleaseSpinLock (&MmDereferenceSegmentHeader.Lock, OldIrql);

    KeReleaseSemaphore (&MmDereferenceSegmentHeader.Semaphore, 0L, 1L, FALSE);
    return;
}

VOID
MiAttemptPageFileReduction (
    VOID
    )

/*++

Routine Description:

    This routine attempts to reduce the size of the paging files to
    their minimum levels.

    Note - Page file expansion and page file reduction are synchronized
           because a single thread is responsible for performing the
           operation.  Hence, while expansion is occurring, a reduction
           request will be queued to the thread.

Arguments:

    None.

Return Value:

    None.

--*/

{
    SIZE_T CommitLimit;
    SIZE_T CommittedPages;
    SIZE_T SafetyMargin;
    KIRQL OldIrql;
    ULONG i;
    ULONG j;
    PFN_NUMBER StartReduction;
    PFN_NUMBER ReductionSize;
    PFN_NUMBER TryBit;
    PFN_NUMBER TryReduction;
    PMMPAGING_FILE PagingFile;
    SIZE_T MaxReduce;
    FILE_ALLOCATION_INFORMATION FileAllocationInfo;
    NTSTATUS status;

    //
    // Mark the global pagefile contraction item as being processed so other
    // threads will know to reset it if another contraction is desired.
    //

    ExAcquireSpinLock (&MmDereferenceSegmentHeader.Lock, &OldIrql);

    ASSERT (MiPageFileContract.RequestedExpansionSize == MI_CONTRACT_PAGEFILES);

    MiPageFileContract.RequestedExpansionSize = ~MI_CONTRACT_PAGEFILES;

    ExReleaseSpinLock (&MmDereferenceSegmentHeader.Lock, OldIrql);

    //
    // Snap the globals into locals so calculations will be consistent from
    // step to step.  It is ok to snap the globals unsynchronized with respect
    // to each other.
    //

    CommittedPages = MmTotalCommittedPages;
    CommitLimit = MmTotalCommitLimit;

    //
    // Make sure the commit limit is significantly greater than the number
    // of committed pages to avoid thrashing.
    //

    SafetyMargin = 2 * MmMinimumPageFileReduction;

    if (CommittedPages + SafetyMargin >= ((CommitLimit/10)*8)) {
        return;
    }

    MaxReduce = ((CommitLimit/10)*8) - (CommittedPages + SafetyMargin);

    ASSERT ((SSIZE_T)MaxReduce > 0);
    ASSERT ((LONG_PTR)MaxReduce >= 0);

    for (i = 0; i < MmNumberOfPagingFiles; i += 1) {

        if (MaxReduce < MmMinimumPageFileReduction) {

            //
            // Don't reduce any more paging files.
            //

            break;
        }

        PagingFile = MmPagingFile[i];

        if (PagingFile->Size == PagingFile->MinimumSize) {
            continue;
        }

        //
        // This unsynchronized check is ok because a synchronized check is
        // made later.
        //

        if (PagingFile->FreeSpace < MmMinimumPageFileReduction) {
            continue;
        }

        //
        // Lock the PFN database and check to see if ample pages
        // are free at the end of the paging file.
        //

        TryBit = PagingFile->Size - MmMinimumPageFileReduction;
        TryReduction = MmMinimumPageFileReduction;

        if (TryBit <= PagingFile->MinimumSize) {
            TryBit = PagingFile->MinimumSize;
            TryReduction = PagingFile->Size - PagingFile->MinimumSize;
        }

        StartReduction = 0;
        ReductionSize = 0;

        LOCK_PFN (OldIrql);

        do {

            //
            // Try to reduce.
            //

            if ((ReductionSize + TryReduction) > MaxReduce) {

                //
                // The reduction attempt would remove more
                // than MaxReduce pages.
                //

                break;
            }

            if (RtlAreBitsClear (PagingFile->Bitmap,
                                 (ULONG)TryBit,
                                 (ULONG)TryReduction)) {

                //
                // Can reduce it by TryReduction, see if it can
                // be made smaller.
                //

                StartReduction = TryBit;
                ReductionSize += TryReduction;

                if (StartReduction == PagingFile->MinimumSize) {
                    break;
                }

                TryBit = StartReduction - MmMinimumPageFileReduction;

                if (TryBit <= PagingFile->MinimumSize) {
                    TryReduction -= PagingFile->MinimumSize - TryBit;
                    TryBit = PagingFile->MinimumSize;
                }
                else {
                    TryReduction = MmMinimumPageFileReduction;
                }
            }
            else {

                //
                // Reduction has failed.
                //

                break;
            }

        } while (TRUE);

        //
        // Make sure there are no outstanding writes to
        // pages within the start reduction range.
        //

        if (StartReduction != 0) {

            //
            // There is an outstanding write past where the
            // new end of the paging file should be.  This
            // is a very rare condition, so just punt shrinking
            // the file.
            //

            for (j = 0; j < MM_PAGING_FILE_MDLS; j += 1) {
                if (PagingFile->Entry[j]->LastPageToWrite > StartReduction) {
                    StartReduction = 0;
                    break;
                }
            }
        }

        //
        // If there are no pages to remove, march on to the next pagefile.
        //

        if (StartReduction == 0) {
            UNLOCK_PFN (OldIrql);
            continue;
        }

        //
        // Reduce the paging file's size and free space.
        //

        ASSERT (ReductionSize == (PagingFile->Size - StartReduction));

        PagingFile->Size = StartReduction;
        PagingFile->FreeSpace -= ReductionSize;

        RtlSetBits (PagingFile->Bitmap,
                    (ULONG)StartReduction,
                    (ULONG)ReductionSize );

        //
        // Release the PFN lock now that the size info
        // has been updated.
        //

        UNLOCK_PFN (OldIrql);

        MaxReduce -= ReductionSize;
        ASSERT ((LONG)MaxReduce >= 0);

        //
        // Change the commit limit to reflect the returned page file space.
        // First try to charge the reduction amount to confirm that the
        // reduction is still a sensible thing to do.
        //

        if (MiChargeTemporaryCommitmentForReduction (ReductionSize + SafetyMargin) == FALSE) {

            LOCK_PFN (OldIrql);

            PagingFile->Size = StartReduction + ReductionSize;
            PagingFile->FreeSpace += ReductionSize;

            RtlClearBits (PagingFile->Bitmap,
                          (ULONG)StartReduction,
                          (ULONG)ReductionSize );

            UNLOCK_PFN (OldIrql);

            ASSERT ((LONG)(MaxReduce + ReductionSize) >= 0);

            break;
        }

        //
        // Reduce the systemwide commit limit - note this is carefully done
        // *PRIOR* to returning this commitment so no one else (including a DPC
        // in this very thread) can consume past the limit.
        //

        InterlockedExchangeAddSizeT (&MmTotalCommitLimit, 0 - ReductionSize);

        //
        // Now that the systemwide commit limit has been lowered, the amount
        // we have removed can be safely returned.
        //

        MiReturnCommitment (ReductionSize + SafetyMargin);

#if defined (_WIN64) || defined (_X86PAE_)
        FileAllocationInfo.AllocationSize.QuadPart =
                                       ((ULONG64)StartReduction << PAGE_SHIFT);

#else
        FileAllocationInfo.AllocationSize.LowPart = StartReduction * PAGE_SIZE;

        //
        // Set high part to zero, paging files are limited to 4gb.
        //

        FileAllocationInfo.AllocationSize.HighPart = 0;
#endif

        //
        // Reduce the allocated size of the paging file
        // thereby actually freeing the space and
        // setting a new end of file.
        //

        ASSERT (KeGetCurrentIrql() < DISPATCH_LEVEL);

        status = IoSetInformation (PagingFile->File,
                                   FileAllocationInformation,
                                   sizeof(FILE_ALLOCATION_INFORMATION),
                                   &FileAllocationInfo);
#if DBG
        //
        // Ignore errors on truncating the paging file
        // as we can always have less space in the bitmap
        // than the pagefile holds.
        //

        if (status != STATUS_SUCCESS) {
            DbgPrintEx (DPFLTR_MM_ID, DPFLTR_INFO_LEVEL, 
                "MM: pagefile truncate status %lx\n", status);
        }
#endif
    }

    return;
}


VOID
MiLdwPopupWorker (
    IN PVOID Context
    )

/*++

Routine Description:

    This routine is the worker routine to send a lost delayed write data popup
    for a given control area/file.

Arguments:

    Context - Supplies a pointer to the MM_LDW_WORK_CONTEXT for the failed I/O.

Return Value:

    None.

Environment:

    Kernel mode, PASSIVE_LEVEL.

--*/

{
    NTSTATUS Status;
    PFILE_OBJECT FileObject;
    PMM_LDW_WORK_CONTEXT LdwContext;
    POBJECT_NAME_INFORMATION FileNameInfo;

    PAGED_CODE();

    LdwContext = (PMM_LDW_WORK_CONTEXT) Context;
    FileObject = LdwContext->FileObject;
    FileNameInfo = NULL;

    if (MiDereferenceLastChanceLdw (LdwContext) == FALSE) {
        ExFreePool (LdwContext);
    }

    //
    // Throw the popup with the user-friendly form, if possible.
    // If everything fails, the user probably couldn't have figured
    // out what failed either.
    //

    Status = IoQueryFileDosDeviceName (FileObject, &FileNameInfo);

    if (Status == STATUS_SUCCESS) {

        IoRaiseInformationalHardError (STATUS_LOST_WRITEBEHIND_DATA,
                                       &FileNameInfo->Name,
                                       NULL);

    }
    else {
        if ((FileObject->FileName.Length) &&
            (FileObject->FileName.MaximumLength) &&
            (FileObject->FileName.Buffer)) {

            IoRaiseInformationalHardError (STATUS_LOST_WRITEBEHIND_DATA,
                                           &FileObject->FileName,
                                           NULL);
        }
    }

    //
    // Now drop the reference to the file object and clean up.
    //

    ObDereferenceObject (FileObject);

    if (FileNameInfo != NULL) {
        ExFreePool (FileNameInfo);
    }
}


VOID
MiWriteComplete (
    IN PVOID Context,
    IN PIO_STATUS_BLOCK IoStatus,
    IN ULONG Reserved
    )

/*++

Routine Description:

    This routine is the APC write completion procedure.  It is invoked
    at APC_LEVEL when a page write operation is completed.

Arguments:

    Context - Supplies a pointer to the MOD_WRITER_MDL_ENTRY which was
              used for this I/O.

    IoStatus - Supplies a pointer to the IO_STATUS_BLOCK which was used
               for this I/O.

Return Value:

    None.

Environment:

    Kernel mode, APC_LEVEL.

--*/

{
    PKEVENT Event;
    LONG ClusterWritesDisabled;
    LONG OldClusterWritesDisabled;
    PMM_LDW_WORK_CONTEXT LdwContext;
    PMMMOD_WRITER_MDL_ENTRY WriterEntry;
    PMMMOD_WRITER_MDL_ENTRY NextWriterEntry;
    PPFN_NUMBER Page;
    PMMPFN Pfn1;
    KIRQL OldIrql;
    LONG ByteCount;
    NTSTATUS status;
    PCONTROL_AREA ControlArea;
    LOGICAL FailAllIo;
    LOGICAL MarkDirty;
    PFILE_OBJECT FileObject;
    PERESOURCE FileResource;

    UNREFERENCED_PARAMETER (Reserved);

    ASSERT (KeAreAllApcsDisabled () == TRUE);

    Event = NULL;
    FailAllIo = FALSE;
    MarkDirty = FALSE;

    //
    // A page write has completed, at this time the pages are not
    // on any lists, write-in-progress is set in the PFN database,
    // and the reference count was incremented.
    //

    WriterEntry = (PMMMOD_WRITER_MDL_ENTRY)Context;
    ByteCount = (LONG) WriterEntry->Mdl.ByteCount;
    Page = &WriterEntry->Page[0];

    if (WriterEntry->Mdl.MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) {
        MmUnmapLockedPages (WriterEntry->Mdl.MappedSystemVa,
                            &WriterEntry->Mdl);
    }

    status = IoStatus->Status;
    ControlArea = WriterEntry->ControlArea;

    //
    // Do as much work as possible before acquiring the PFN lock.
    //

    while (ByteCount > 0) {

        Pfn1 = MI_PFN_ELEMENT (*Page);
        *Page = (PFN_NUMBER) Pfn1;
        Page += 1;
        ByteCount -= (LONG)PAGE_SIZE;
    }

    ByteCount = (LONG) WriterEntry->Mdl.ByteCount;
    Page = &WriterEntry->Page[0];

    FileResource = WriterEntry->FileResource;

    if (FileResource != NULL) {
        FileObject = WriterEntry->File;
        FsRtlReleaseFileForModWrite (FileObject, FileResource);
    }
    else if (ControlArea == NULL) {
        LARGE_INTEGER CurrentTime;
        KeQuerySystemTime (&CurrentTime);
        MI_PAGEFILE_WRITE (WriterEntry, &CurrentTime, 0, status);
    }

    if (!NT_SUCCESS (status)) {

        //
        // If the file object is over the network, assume that this
        // I/O operation can never complete and mark the pages as
        // clean and indicate in the control area all I/O should fail.
        // Note that the modified bit in the PFN database is not set.
        //
        // If the user changes the protection on the volume containing the
        // file to readonly, this puts us in a problematic situation.  We
        // cannot just keep retrying the writes because if there are no
        // other pages that can be written, not writing these can cause the
        // system to run out of pages, ie: bugcheck 4D.  So throw away
        // these pages just as if they were on a network that has
        // disappeared.
        //

        if (((status != STATUS_FILE_LOCK_CONFLICT) &&
            (ControlArea != NULL) &&
            (ControlArea->u.Flags.Networked == 1))
                        ||
            (status == STATUS_FILE_INVALID)
                        ||
            ((status == STATUS_MEDIA_WRITE_PROTECTED) &&
             (ControlArea != NULL))) {

            if (ControlArea->u.Flags.FailAllIo == 0) {
                ControlArea->u.Flags.FailAllIo = 1;
                FailAllIo = TRUE;

                KdPrint(("MM MODWRITE: failing all io, controlarea %p status %lx\n",
                      ControlArea, status));
            }
        }
        else {

            //
            // The modified write operation failed, SET the modified bit
            // for each page which was written and free the page file
            // space.
            //

#if DBG
            ULONG i;

            //
            // Save error status information to ease debugging.  Since this
            // doesn't need to be exact, don't bother lock-synchronizing.
            //

            for (i = 0; i < MM_MAX_MODWRITE_ERRORS - 1; i += 1) {

                if (MiModwriteErrors[i].Status == 0) {
                    MiModwriteErrors[i].Status = status;
                    MiModwriteErrors[i].Count += 1;
                    break;
                }
                else if (MiModwriteErrors[i].Status == status) {
                    MiModwriteErrors[i].Count += 1;
                    break;
                }
            }

            if (i == MM_MAX_MODWRITE_ERRORS - 1) {
                MiModwriteErrors[i].Count += 1;
            }
#endif

            MarkDirty = TRUE;
        }

        //
        // Save the last error for debugging purposes.
        //

        if (ControlArea == NULL) {
            MiLastModifiedWriteError = status;
        }
        else {
            MiLastMappedWriteError = status;
        }
    }

    //
    // Get the PFN lock so the PFN database can be manipulated.
    //

    LOCK_PFN (OldIrql);

    //
    // Indicate that the write is complete.
    //

    WriterEntry->LastPageToWrite = 0;

    while (ByteCount > 0) {

        Pfn1 = (PMMPFN) *Page;
        ASSERT (Pfn1->u3.e1.WriteInProgress == 1);
#if DBG
#if !defined (_WIN64)
        if (Pfn1->OriginalPte.u.Soft.Prototype == 0) {

            ULONG Offset;

            Offset = GET_PAGING_FILE_OFFSET(Pfn1->OriginalPte);

            if ((Offset < 8192) &&
                (GET_PAGING_FILE_NUMBER(Pfn1->OriginalPte) == 0)) {

                ASSERT ((MmPagingFileDebug[Offset] & 1) != 0);

                if ((!MI_IS_PFN_DELETED (Pfn1)) &&
                    ((MmPagingFileDebug[Offset] & ~0x1f) !=
                                   ((ULONG_PTR)Pfn1->PteAddress << 3)) &&
                    (Pfn1->PteAddress != MiGetPteAddress(PDE_BASE))) {

                    //
                    // Make sure this isn't a PTE that was forked
                    // during the I/O.
                    //

                    if ((Pfn1->PteAddress < (PMMPTE)PTE_TOP) ||
                        ((Pfn1->OriginalPte.u.Soft.Protection &
                                MM_COPY_ON_WRITE_MASK) ==
                                    MM_PROTECTION_WRITE_MASK)) {
                        DbgPrintEx (DPFLTR_MM_ID, DPFLTR_ERROR_LEVEL, 
                            "MMWRITE: Mismatch Pfn1 %p Offset %lx info %p\n",
                                 Pfn1,
                                 Offset,
                                 MmPagingFileDebug[Offset]);

                        DbgBreakPoint ();
                    }
                    else {
                        MmPagingFileDebug[Offset] &= 0x1f;
                        MmPagingFileDebug[Offset] |=
                            ((ULONG_PTR)Pfn1->PteAddress << 3);
                    }
                }
            }
        }
#endif
#endif //DBG

        Pfn1->u3.e1.WriteInProgress = 0;

        if (MarkDirty == TRUE) {

            //
            // The modified write operation failed, SET the modified bit
            // for each page which was written and free the page file
            // space.
            //

            MI_SET_MODIFIED (Pfn1, 1, 0x9);
        }

        if ((Pfn1->u3.e1.Modified == 1) &&
            (Pfn1->OriginalPte.u.Soft.Prototype == 0)) {

            //
            // This page was modified since the write was done,
            // release the page file space.
            //

            MiReleasePageFileSpace (Pfn1->OriginalPte);
            Pfn1->OriginalPte.u.Soft.PageFileHigh = 0;
        }

        MI_REMOVE_LOCKED_PAGE_CHARGE_AND_DECREF (Pfn1);

#if DBG
        *Page = 0xF0FFFFFF;
#endif

        Page += 1;
        ByteCount -= (LONG)PAGE_SIZE;
    }

    //
    // Choose which list to insert this entry into depending on
    // the amount of free space left in the paging file.
    //

    if ((WriterEntry->PagingFile != NULL) &&
        (WriterEntry->PagingFile->FreeSpace < MM_USABLE_PAGES_FREE)) {

        InsertTailList (&MmFreePagingSpaceLow, &WriterEntry->Links);
        WriterEntry->CurrentList = &MmFreePagingSpaceLow;
        MmNumberOfActiveMdlEntries -= 1;

        if (MmNumberOfActiveMdlEntries == 0) {

            //
            // If we leave this entry on the list, there will be
            // no more paging.  Locate all entries which are non
            // zero and pull them from the list.
            //

            WriterEntry = (PMMMOD_WRITER_MDL_ENTRY)MmFreePagingSpaceLow.Flink;

            while ((PLIST_ENTRY)WriterEntry != &MmFreePagingSpaceLow) {

                NextWriterEntry =
                            (PMMMOD_WRITER_MDL_ENTRY)WriterEntry->Links.Flink;

                if (WriterEntry->PagingFile->FreeSpace != 0) {

                    RemoveEntryList (&WriterEntry->Links);

                    //
                    // Insert this into the active list.
                    //

                    if (IsListEmpty (&WriterEntry->PagingListHead->ListHead)) {
                        KeSetEvent (&WriterEntry->PagingListHead->Event,
                                    0,
                                    FALSE);
                    }

                    InsertTailList (&WriterEntry->PagingListHead->ListHead,
                                    &WriterEntry->Links);
                    WriterEntry->CurrentList = &MmPagingFileHeader.ListHead;
                    MmNumberOfActiveMdlEntries += 1;
                }

                WriterEntry = NextWriterEntry;
            }

        }
    }
    else {

#if DBG
        if (WriterEntry->PagingFile == NULL) {
            MmNumberOfMappedMdlsInUse -= 1;
        }
#endif
        //
        // Ample space exists, put this on the active list.
        //

        if (IsListEmpty (&WriterEntry->PagingListHead->ListHead)) {
            Event = &WriterEntry->PagingListHead->Event;
        }

        InsertTailList (&WriterEntry->PagingListHead->ListHead,
                        &WriterEntry->Links);
    }

    ASSERT (((ULONG_PTR)WriterEntry->Links.Flink & 1) == 0);

    if (Event != NULL) {
        KeSetEvent (Event, 0, FALSE);
    }

    if (ControlArea != NULL) {

        if (FailAllIo == TRUE) {
    
            //
            // Reference our fileobject and queue the popup.  The DOS
            // name translation must occur at PASSIVE_LEVEL - we're at APC.
            //
    
            UNLOCK_PFN (OldIrql);

            LdwContext = ExAllocatePoolWithTag (NonPagedPool,
                                                sizeof(MM_LDW_WORK_CONTEXT),
                                                'pdmM');
    
            if (LdwContext != NULL) {
                LdwContext->FileObject = ControlArea->FilePointer;
                ObReferenceObject (LdwContext->FileObject);
    
                ExInitializeWorkItem (&LdwContext->WorkItem,
                                      MiLdwPopupWorker,
                                      (PVOID)LdwContext);
    
                ExQueueWorkItem (&LdwContext->WorkItem, DelayedWorkQueue);
            }

            LOCK_PFN (OldIrql);
        }
    
        //
        // A write to a mapped file just completed, check to see if
        // there are any waiters on the completion of this i/o.
        //

        ControlArea->ModifiedWriteCount -= 1;
        ASSERT ((SHORT)ControlArea->ModifiedWriteCount >= 0);

        if (ControlArea->u.Flags.SetMappedFileIoComplete != 0) {
            KePulseEvent (&MmMappedFileIoComplete, 0, FALSE);
        }

        if (MiDrainingMappedWrites == TRUE) {
            if (MmModifiedPageListHead.Flink != MM_EMPTY_LIST) {

                //
                // Note the TimerPending flag and the event must be set while
                // holding the PFN lock - otherwise the other thread (modified
                // or mapped writer) can clear TimerPending before we set
                // the event here.
                //

                MiTimerPending = TRUE;
                KeSetEvent (&MiMappedPagesTooOldEvent, 0, FALSE);
            }
            else {
                MiDrainingMappedWrites = FALSE;
            }
        }

        ControlArea->NumberOfPfnReferences -= 1;

        if (ControlArea->NumberOfPfnReferences == 0) {

            //
            // This routine returns with the PFN lock released!
            //

            MiCheckControlArea (ControlArea, OldIrql);
        }
        else {
            UNLOCK_PFN (OldIrql);
        }
    }
    else {
        UNLOCK_PFN (OldIrql);
    }

    if (!NT_SUCCESS(status)) {

        //
        // Wait for a short time so other processing can continue.
        //

        KeDelayExecutionThread (KernelMode,
                                FALSE,
                                (PLARGE_INTEGER)&Mm30Milliseconds);

        if (MmIsRetryIoStatus (status)) {

            //
            // Low resource scenarios are a chicken and egg problem.  The
            // mapped and modified writers must make forward progress to
            // alleviate low memory situations.  If these threads are
            // unable to write data due to resource problems in the driver
            // stack then temporarily fall back to single page I/Os as
            // the stack guarantees forward progress with those.  This
            // causes the low memory situation to persist slightly longer
            // but ensures that it won't become terminal either.
            //

            do {

                OldClusterWritesDisabled = ReadForWriteAccess (&MiClusterWritesDisabled);

                ClusterWritesDisabled = InterlockedCompareExchange (
                                                    &MiClusterWritesDisabled,
                                                    MI_SLOW_CLUSTER_WRITES,
                                                    OldClusterWritesDisabled);

            } while (OldClusterWritesDisabled != ClusterWritesDisabled);
        }
    }
    else {

        do {

            OldClusterWritesDisabled = MiClusterWritesDisabled;

            if (OldClusterWritesDisabled == 0) {
                break;
            }

            ClusterWritesDisabled = InterlockedCompareExchange (
                                                &MiClusterWritesDisabled,
                                                OldClusterWritesDisabled - 1,
                                                OldClusterWritesDisabled);

        } while (OldClusterWritesDisabled != ClusterWritesDisabled);
    }

    return;
}

LOGICAL
MiCancelWriteOfMappedPfn (
    IN PFN_NUMBER PageToStop,
    IN KIRQL OldIrql
    )

/*++

Routine Description:

    This routine attempts to stop a pending mapped page writer write for the
    specified PFN.  Note that if the write can be stopped, any other pages
    that may be clustered with the write are also stopped.

Arguments:

    PageToStop - Supplies the frame number that the caller wants to stop.

    OldIrql - Supplies the IRQL the caller acquired the PFN lock at.

Return Value:

    TRUE if the write was stopped, FALSE if not.

Environment:

    Kernel mode, PFN lock held.  The PFN lock is released and reacquired if
    the write was stopped.

    N.B.  No other locks may be held as IRQL is lowered to APC_LEVEL here.

--*/

{
    ULONG i;
    ULONG PageCount;
    PPFN_NUMBER Page;
    PLIST_ENTRY NextEntry;
    PMDL MemoryDescriptorList;
    PMMMOD_WRITER_MDL_ENTRY ModWriterEntry;

    //
    // Walk the MmMappedPageWriterList looking for an MDL which contains
    // the argument page.  If found, remove it and cancel the write.
    //

    NextEntry = MmMappedPageWriterList.Flink;
    while (NextEntry != &MmMappedPageWriterList) {

        ModWriterEntry = CONTAINING_RECORD(NextEntry,
                                           MMMOD_WRITER_MDL_ENTRY,
                                           Links);

        MemoryDescriptorList = &ModWriterEntry->Mdl;
        PageCount = (MemoryDescriptorList->ByteCount >> PAGE_SHIFT);
        Page = (PPFN_NUMBER)(MemoryDescriptorList + 1);

        for (i = 0; i < PageCount; i += 1) {
            if (*Page == PageToStop) {
                RemoveEntryList (NextEntry);
                goto CancelWrite;
            }
            Page += 1;
        }

        NextEntry = NextEntry->Flink;
    }

    return FALSE;

CancelWrite:

    UNLOCK_PFN (OldIrql);

    //
    // File lock conflict to indicate an error has occurred,
    // but that future I/Os should be allowed.  Keep APCs disabled and
    // call the write completion routine.
    //

    ModWriterEntry->u.IoStatus.Status = STATUS_FILE_LOCK_CONFLICT;
    ModWriterEntry->u.IoStatus.Information = 0;

    KeRaiseIrql (APC_LEVEL, &OldIrql);
    MiWriteComplete ((PVOID)ModWriterEntry,
                     &ModWriterEntry->u.IoStatus,
                     0);
    KeLowerIrql (OldIrql);

    LOCK_PFN (OldIrql);

    return TRUE;
}

VOID
MiModifiedPageWriter (
    IN PVOID StartContext
    )

/*++

Routine Description:

    Implements the NT modified page writer thread.  When the modified
    page threshold is reached, or memory becomes overcommitted the
    modified page writer event is set, and this thread becomes active.

Arguments:

    StartContext - not used.

Return Value:

    None.

Environment:

    Kernel mode.

--*/

{
    ULONG i;
    HANDLE ThreadHandle;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PMMMOD_WRITER_MDL_ENTRY ModWriteEntry;

    PAGED_CODE();

    UNREFERENCED_PARAMETER (StartContext);

    //
    // Initialize listheads as empty.
    //

    MmSystemShutdown = 0;
    KeInitializeEvent (&MmPagingFileHeader.Event, NotificationEvent, FALSE);
    KeInitializeEvent (&MmMappedFileHeader.Event, NotificationEvent, FALSE);

    InitializeListHead(&MmPagingFileHeader.ListHead);
    InitializeListHead(&MmMappedFileHeader.ListHead);
    InitializeListHead(&MmFreePagingSpaceLow);

    //
    // Allocate enough MDLs such that 2% of system memory can be pending
    // at any point in time in mapped writes.  Even smaller memory systems
    // get 20 MDLs as the minimum.
    //

    MmNumberOfMappedMdls = MmNumberOfPhysicalPages / (32 * 1024);

    if (MmNumberOfMappedMdls < 20) {
        MmNumberOfMappedMdls = 20;
    }

    for (i = 0; i < MmNumberOfMappedMdls; i += 1) {
        ModWriteEntry = ExAllocatePoolWithTag (NonPagedPool,
                                             sizeof(MMMOD_WRITER_MDL_ENTRY) +
                                                MmModifiedWriteClusterSize *
                                                    sizeof(PFN_NUMBER),
                                                'eWmM');

        if (ModWriteEntry == NULL) {
            break;
        }

        ModWriteEntry->PagingFile = NULL;
        ModWriteEntry->PagingListHead = &MmMappedFileHeader;

        InsertTailList (&MmMappedFileHeader.ListHead, &ModWriteEntry->Links);
    }

    MmNumberOfMappedMdls = i;

    //
    // Make this a real time thread.
    //

    KeSetPriorityThread (KeGetCurrentThread (), LOW_REALTIME_PRIORITY + 1);

    //
    // Start a secondary thread for writing mapped file pages.  This
    // is required as the writing of mapped file pages could cause
    // page faults resulting in requests for free pages.  But there
    // could be no free pages - hence a dead lock.  Rather than deadlock
    // the whole system waiting on the modified page writer, creating
    // a secondary thread allows that thread to block without affecting
    // on going page file writes.
    //

    KeInitializeEvent (&MmMappedPageWriterEvent, NotificationEvent, FALSE);
    InitializeListHead (&MmMappedPageWriterList);
    InitializeObjectAttributes (&ObjectAttributes, NULL, 0, NULL, NULL);

    Status = PsCreateSystemThread (&ThreadHandle,
                                   THREAD_ALL_ACCESS,
                                   &ObjectAttributes,
                                   0L,
                                   NULL,
                                   MiMappedPageWriter,
                                   NULL);

    if (!NT_SUCCESS(Status)) {
        KeBugCheckEx (MEMORY_MANAGEMENT,
                      0x41288,
                      Status,
                      0,
                      0);
    }

    ZwClose (ThreadHandle);

    MiModifiedPageWriterWorker ();

    //
    // Shutdown in progress, wait forever.
    //

    {
        LARGE_INTEGER Forever;

        //
        // System has shutdown, go into LONG wait.
        //

        Forever.LowPart = 0;
        Forever.HighPart = 0xF000000;
        KeDelayExecutionThread (KernelMode, FALSE, &Forever);
    }

    return;
}


VOID
MiModifiedPageWriterTimerDispatch (
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2
    )

/*++

Routine Description:

    This routine is executed whenever modified mapped pages are waiting to
    be written.  Its job is to signal the Modified Page Writer to write
    these out.

Arguments:

    Dpc - Supplies a pointer to a control object of type DPC.

    DeferredContext - Optional deferred context;  not used.

    SystemArgument1 - Optional argument 1;  not used.

    SystemArgument2 - Optional argument 2;  not used.

Return Value:

    None.

--*/

{
    UNREFERENCED_PARAMETER (Dpc);
    UNREFERENCED_PARAMETER (DeferredContext);
    UNREFERENCED_PARAMETER (SystemArgument1);
    UNREFERENCED_PARAMETER (SystemArgument2);

    LOCK_PFN_AT_DPC ();

    MiTimerPending = TRUE;
    KeSetEvent (&MiMappedPagesTooOldEvent, 0, FALSE);

    UNLOCK_PFN_FROM_DPC ();
}


VOID
MiModifiedPageWriterWorker (
    VOID
    )

/*++

Routine Description:

    Implements the NT modified page writer thread.  When the modified
    page threshold is reached, or memory becomes overcommitted the
    modified page writer event is set, and this thread becomes active.

Arguments:

    None.

Return Value:

    None.

Environment:

    Kernel mode.

--*/

{
    PRTL_BITMAP Bitmap;
    KIRQL OldIrql;
    static KWAIT_BLOCK WaitBlockArray[ModifiedWriterMaximumObject];
    PVOID WaitObjects[ModifiedWriterMaximumObject];
    NTSTATUS WakeupStatus;
    LOGICAL MappedPage;
    PMMMOD_WRITER_MDL_ENTRY ModWriterEntry;

    PsGetCurrentThread()->MemoryMaker = 1;

    //
    // Wait for the modified page writer event or the mapped pages event.
    //

    WaitObjects[NormalCase] = (PVOID)&MmModifiedPageWriterEvent;
    WaitObjects[MappedPagesNeedWriting] = (PVOID)&MiMappedPagesTooOldEvent;

    for (;;) {

        WakeupStatus = KeWaitForMultipleObjects (ModifiedWriterMaximumObject,
                                                 &WaitObjects[0],
                                                 WaitAny,
                                                 WrFreePage,
                                                 KernelMode,
                                                 FALSE,
                                                 NULL,
                                                 &WaitBlockArray[0]);

        LOCK_PFN (OldIrql);

        for (;;) {

            //
            // Modified page writer was signaled.
            //

            if (MmModifiedPageListHead.Total == 0) {

                //
                // No more pages, clear the event(s) and wait again...
                // Note we can clear both events regardless of why we woke up
                // since no modified pages of any type exist.
                //

                MiTimerPending = FALSE;
                KeClearEvent (&MiMappedPagesTooOldEvent);

                UNLOCK_PFN (OldIrql);

                KeClearEvent (&MmModifiedPageWriterEvent);

                break;
            }

            //
            // If we didn't wake up explicitly to deal with mapped pages,
            // then determine which type of pages are the most popular:
            // page file backed pages, or mapped file backed pages.
            //

            if ((WakeupStatus == MappedPagesNeedWriting) ||
                (MmTotalPagesForPagingFile <
                MmModifiedPageListHead.Total - MmTotalPagesForPagingFile)) {

                //
                // More pages are destined for mapped files.
                //

                if (MmModifiedPageListHead.Flink != MM_EMPTY_LIST) {

                    if (WakeupStatus == MappedPagesNeedWriting) {

                        //
                        // Our mapped pages DPC went off, only deal with
                        // those pages.  Write all the mapped pages (ONLY),
                        // then clear the flag and come back to the top.
                        //

                        MiDrainingMappedWrites = TRUE;
                    }

                    MappedPage = TRUE;

                    if (IsListEmpty (&MmMappedFileHeader.ListHead)) {

                        //
                        // Make sure page is destined for paging file as there
                        // are no MDLs for mapped writes free.
                        //

                        if (WakeupStatus != MappedPagesNeedWriting) {

                            //
                            // No MDLs are available for writing mapped pages,
                            // try for a page destined for a pagefile.
                            //

                            if (MmTotalPagesForPagingFile != 0) {
                                MappedPage = FALSE;
                            }
                        }
                    }
                }
                else {

                    //
                    // No more modified mapped pages (there may still be
                    // modified pagefile-destined pages), so clear only the
                    // mapped pages event and check for directions at the top
                    // again.
                    //

                    if (WakeupStatus == MappedPagesNeedWriting) {

                        MiTimerPending = FALSE;
                        KeClearEvent (&MiMappedPagesTooOldEvent);
                        UNLOCK_PFN (OldIrql);

                        break;
                    }
                    MappedPage = FALSE;
                }
            }
            else {

                //
                // More pages are destined for the paging file.
                //

                MappedPage = FALSE;

                if (((IsListEmpty(&MmPagingFileHeader.ListHead)) ||
                    (MiFirstPageFileCreatedAndReady == FALSE))) {

                    //
                    // Try for a dirty section-backed page as no paging
                    // file MDLs are available.
                    //

                    if (MmModifiedPageListHead.Flink != MM_EMPTY_LIST) {
                        MappedPage = TRUE;
                    }
                    else {
                        ASSERT (MmTotalPagesForPagingFile == MmModifiedPageListHead.Total);
                        if ((MiFirstPageFileCreatedAndReady == FALSE) &&
                            (MmNumberOfPagingFiles != 0)) {

                            //
                            // The first paging has been created but the
                            // reservation checking for crashdumps has not
                            // finished yet.  Delay a bit as this will finish
                            // shortly and then restart.
                            //

                            UNLOCK_PFN (OldIrql);
                            KeDelayExecutionThread (KernelMode, FALSE, (PLARGE_INTEGER)&MmShortTime);
                            LOCK_PFN (OldIrql);
                            continue;
                        }
                    }
                }
            }

            if (MappedPage == TRUE) {

                if (IsListEmpty(&MmMappedFileHeader.ListHead)) {

                    if (WakeupStatus == MappedPagesNeedWriting) {

                        //
                        // Since we woke up only to take care of mapped pages,
                        // don't wait for an MDL below because drivers may take
                        // an inordinate amount of time processing the
                        // outstanding ones.  We might have to wait too long,
                        // resulting in the system running out of pages.
                        //

                        if (MiTimerPending == TRUE) {

                            //
                            // This should be normal case - the reason we must
                            // first check timer pending above is for the rare
                            // case - when this thread first ran for normal
                            // modified page processing and took
                            // care of all the pages including the mapped ones.
                            // Then this thread woke up again for the mapped
                            // reason and here we are.
                            //

                            MiTimerPending = FALSE;
                            KeClearEvent (&MiMappedPagesTooOldEvent);
                        }

                        MiTimerPending = TRUE;

                        KeSetTimerEx (&MiModifiedPageWriterTimer,
                                      MiModifiedPageLife,
                                      0,
                                      &MiModifiedPageWriterTimerDpc);

                        UNLOCK_PFN (OldIrql);
                        break;
                    }

                    //
                    // Reset the event indicating no mapped files in
                    // the list, drop the PFN lock and wait for an
                    // I/O operation to complete with a one second
                    // timeout.
                    //

                    KeClearEvent (&MmMappedFileHeader.Event);

                    UNLOCK_PFN (OldIrql);

                    KeWaitForSingleObject (&MmMappedFileHeader.Event,
                                           WrPageOut,
                                           KernelMode,
                                           FALSE,
                                           (PLARGE_INTEGER)&Mm30Milliseconds);

                    //
                    // Don't go on as the old PageFrameIndex at the
                    // top of the ModifiedList may have changed states.
                    //

                    LOCK_PFN (OldIrql);
                }
                else {

                    //
                    // This routine returns with the PFN lock released !
                    //

                    MiGatherMappedPages (OldIrql);

                    //
                    // Nothing magic here, just give other processors a turn at
                    // the PFN lock (and allow DPCs a chance to execute).
                    //

                    LOCK_PFN (OldIrql);
                }
            }
            else {

                if (IsListEmpty(&MmPagingFileHeader.ListHead)) {

                    //
                    // Reset the event indicating no paging files MDLs in
                    // the list, drop the PFN lock and wait for an
                    // I/O operation to complete.
                    //

                    KeClearEvent (&MmPagingFileHeader.Event);
                    UNLOCK_PFN (OldIrql);
                    KeWaitForSingleObject (&MmPagingFileHeader.Event,
                                           WrPageOut,
                                           KernelMode,
                                           FALSE,
                                           (PLARGE_INTEGER)&Mm30Milliseconds);

                    //
                    // Don't go on as the old PageFrameIndex at the
                    // top of the ModifiedList may have changed states.
                    //
                }
                else {

                    ModWriterEntry = (PMMMOD_WRITER_MDL_ENTRY)RemoveHeadList (
                                            &MmPagingFileHeader.ListHead);

#if DBG
                    ModWriterEntry->Links.Flink = MM_IO_IN_PROGRESS;
#endif

                    //
                    // Increment the reference count under PFN lock protection.
                    //

                    ASSERT (ModWriterEntry->PagingFile->ReferenceCount == 0);
                    ModWriterEntry->PagingFile->ReferenceCount += 1;

                    Bitmap = ModWriterEntry->PagingFile->Bitmap;

                    UNLOCK_PFN (OldIrql);

                    MiGatherPagefilePages (ModWriterEntry, Bitmap);
                }

                LOCK_PFN (OldIrql);
            }

            if (MmSystemShutdown) {

                //
                // Shutdown has returned.  Stop the modified page writer.
                //

                UNLOCK_PFN (OldIrql);
                return;
            }

            //
            // If this is a mapped page timer, then keep on writing till there's
            // nothing left.
            //

            if (WakeupStatus == MappedPagesNeedWriting) {
                continue;
            }

            //
            // If this is a request to write all modified pages, then keep on
            // writing.
            //

            if (MmWriteAllModifiedPages) {
                continue;
            }

            if (MmModifiedPageListHead.Total < MmModifiedWriteClusterSize) {

                //
                // There are ample pages, clear the event and wait again...
                //

                UNLOCK_PFN (OldIrql);

                KeClearEvent (&MmModifiedPageWriterEvent);
                break;
            }

        } // end for

    } // end for
}

VOID
MiGatherMappedPages (
    IN KIRQL OldIrql
    )

/*++

Routine Description:

    This routine processes the specified modified page by examining
    the prototype PTE for that page and the adjacent prototype PTEs
    building a cluster of modified pages destined for a mapped file.
    Once the cluster is built, it is sent to the mapped writer thread
    to be processed.

Arguments:

    OldIrql - Supplies the IRQL the caller acquired the PFN lock at.

Return Value:

    The number of pages in the attempted write.  The PFN lock is released
    prior to returning.

Environment:

    PFN lock held.

--*/

{
    PMMPFN Pfn2;
    PFN_NUMBER PagesWritten;
    PMMMOD_WRITER_MDL_ENTRY ModWriterEntry;
    PSUBSECTION Subsection;
    PCONTROL_AREA ControlArea;
    PPFN_NUMBER Page;
    PMMPTE LastPte;
    PMMPTE BasePte;
    PMMPTE NextPte;
    PMMPTE PointerPte;
    PMMPTE StartingPte;
    MMPTE PteContents;
    PVOID HyperMapped;
    PEPROCESS Process;
    PMMPFN Pfn1;
    PFN_NUMBER PageFrameIndex;
    PKPRCB Prcb;

    PageFrameIndex = MmModifiedPageListHead.Flink;
    ASSERT (PageFrameIndex != MM_EMPTY_LIST);
    Pfn1 = MI_PFN_ELEMENT (PageFrameIndex);

    //
    // This page is destined for a mapped file, check to see if
    // there are any physically adjacent pages are also in the
    // modified page list and write them out at the same time.
    //

    Subsection = MiGetSubsectionAddress (&Pfn1->OriginalPte);
    ControlArea = Subsection->ControlArea;

    if (ControlArea->u.Flags.NoModifiedWriting) {

        //
        // This page should not be written out, add it to the
        // tail of the modified NO WRITE list and get the next page.
        //

        MiUnlinkPageFromList (Pfn1);
        MiInsertPageInList (&MmModifiedNoWritePageListHead,
                            PageFrameIndex);
        UNLOCK_PFN (OldIrql);
        return;
    }

    if (ControlArea->u.Flags.Image) {

        //
        // This is an image section, writes are not
        // allowed to an image section.
        //

        //
        // Change page contents to look like it's a demand zero
        // page and put it back into the modified list.
        //

        //
        // Decrement the count for PfnReferences to the
        // segment as paging file pages are not counted as
        // "image" references.
        //

        ControlArea->NumberOfPfnReferences -= 1;
        ASSERT ((LONG)ControlArea->NumberOfPfnReferences >= 0);
        MiUnlinkPageFromList (Pfn1);

        Pfn1->OriginalPte.u.Soft.PageFileHigh = 0;
        Pfn1->OriginalPte.u.Soft.Prototype = 0;
        Pfn1->OriginalPte.u.Soft.Transition = 0;

        //
        // Insert the page at the tail of the list and get
        // color update performed.
        //

        MiInsertPageInList (&MmModifiedPageListHead, PageFrameIndex);
        UNLOCK_PFN (OldIrql);
        return;
    }

    //
    // Look at backwards at previous prototype PTEs to see if
    // this can be clustered into a larger write operation.
    //

    PointerPte = Pfn1->PteAddress;
    NextPte = PointerPte - (MmModifiedWriteClusterSize - 1);

    //
    // Make sure NextPte is in the same page.
    //

    if (NextPte < (PMMPTE)PAGE_ALIGN (PointerPte)) {
        NextPte = (PMMPTE)PAGE_ALIGN (PointerPte);
    }

    //
    // Make sure NextPte is within the subsection.
    //

    if (NextPte < Subsection->SubsectionBase) {
        NextPte = Subsection->SubsectionBase;
    }

    //
    // If the prototype PTEs are not currently mapped,
    // map them via hyperspace.  BasePte refers to the
    // prototype PTEs for nonfaulting references.
    //

    if (MiIsProtoAddressValid (PointerPte)) {
        Process = NULL;
        HyperMapped = NULL;
        BasePte = PointerPte;
    }
    else {
        Process = PsGetCurrentProcess ();
        HyperMapped = MiMapPageInHyperSpaceAtDpc (Process, Pfn1->u4.PteFrame);
        BasePte = (PMMPTE)((PCHAR)HyperMapped + BYTE_OFFSET (PointerPte));
    }

    ASSERT (MI_GET_PAGE_FRAME_FROM_TRANSITION_PTE (BasePte) == PageFrameIndex);

    PointerPte -= 1;
    BasePte -= 1;

    if (MiClusterWritesDisabled != 0) {
        NextPte = PointerPte + 1;
    }

    //
    // Don't go before the start of the subsection nor cross
    // a page boundary.
    //

    while (PointerPte >= NextPte) {

        PteContents = *BasePte;

        //
        // If the page is not in transition, exit loop.
        //

        if ((PteContents.u.Hard.Valid == 1) ||
            (PteContents.u.Soft.Transition == 0) ||
            (PteContents.u.Soft.Prototype == 1)) {

            break;
        }

        Pfn2 = MI_PFN_ELEMENT (PteContents.u.Trans.PageFrameNumber);

        //
        // Make sure page is modified and on the modified list.
        //

        if ((Pfn2->u3.e1.Modified == 0 ) ||
            (Pfn2->u3.e2.ReferenceCount != 0)) {
            break;
        }
        PageFrameIndex = MI_GET_PAGE_FRAME_FROM_TRANSITION_PTE (&PteContents);
        PointerPte -= 1;
        BasePte -= 1;
    }

    StartingPte = PointerPte + 1;
    BasePte = BasePte + 1;

    Pfn1 = MI_PFN_ELEMENT (PageFrameIndex);
    ASSERT (StartingPte == Pfn1->PteAddress);
    MiUnlinkPageFromList (Pfn1);

    //
    // Get an entry from the list and fill it in.
    //

    ModWriterEntry = (PMMMOD_WRITER_MDL_ENTRY)RemoveHeadList (
                                    &MmMappedFileHeader.ListHead);

#if DBG
    MmNumberOfMappedMdlsInUse += 1;
    if (MmNumberOfMappedMdlsInUse > MmNumberOfMappedMdlsInUsePeak) {
        MmNumberOfMappedMdlsInUsePeak = MmNumberOfMappedMdlsInUse;
    }
#endif

    ModWriterEntry->File = ControlArea->FilePointer;
    ModWriterEntry->ControlArea = ControlArea;

    //
    // Calculate the offset to read into the file.
    //  offset = base + ((thispte - basepte) << PAGE_SHIFT)
    //

    ModWriterEntry->WriteOffset.QuadPart = MiStartingOffset (Subsection,
                                                             Pfn1->PteAddress);

    MmInitializeMdl(&ModWriterEntry->Mdl, NULL, PAGE_SIZE);

    ModWriterEntry->Mdl.MdlFlags |= MDL_PAGES_LOCKED;

    ModWriterEntry->Mdl.Size = (CSHORT)(sizeof(MDL) +
                      (sizeof(PFN_NUMBER) * MmModifiedWriteClusterSize));

    Page = &ModWriterEntry->Page[0];

    //
    // Up the reference count for the physical page as there
    // is I/O in progress.
    //

    MI_ADD_LOCKED_PAGE_CHARGE_FOR_MODIFIED_PAGE (Pfn1);

    //
    // Clear the modified bit for the page and set the write
    // in progress bit.
    //

    MI_SET_MODIFIED (Pfn1, 0, 0x23);

    Pfn1->u3.e1.WriteInProgress = 1;

    //
    // Put this physical page into the MDL.
    //

    *Page = PageFrameIndex;

    //
    // See if any adjacent pages are also modified and in
    // the transition state and if so, write them out at
    // the same time.
    //

    LastPte = StartingPte + MmModifiedWriteClusterSize;

    //
    // Look at the last PTE, ensuring a page boundary is not crossed.
    //
    // If LastPte is not in the same page as the StartingPte,
    // set LastPte so the cluster will not cross.
    //

    if (StartingPte < (PMMPTE)PAGE_ALIGN(LastPte)) {
        LastPte = (PMMPTE)PAGE_ALIGN(LastPte);
    }

    //
    // Make sure LastPte is within the subsection.
    //

    if (LastPte > &Subsection->SubsectionBase[Subsection->PtesInSubsection]) {
        LastPte = &Subsection->SubsectionBase[Subsection->PtesInSubsection];
    }

    //
    // Look forwards.
    //

    NextPte = BasePte + 1;
    PointerPte = StartingPte + 1;

    if (MiClusterWritesDisabled != 0) {
        LastPte = PointerPte;
    }

    //
    // Loop until an MDL is filled, the end of a subsection
    // is reached, or a page boundary is reached.
    // Note, PointerPte points to the PTE. NextPte points
    // to where it is mapped in hyperspace (if required).
    //

    while (PointerPte < LastPte) {

        PteContents = *NextPte;

        //
        // If the page is not in transition, exit loop.
        //

        if ((PteContents.u.Hard.Valid == 1) ||
            (PteContents.u.Soft.Transition == 0) ||
            (PteContents.u.Soft.Prototype == 1)) {

            break;
        }

        Pfn2 = MI_PFN_ELEMENT (PteContents.u.Trans.PageFrameNumber);

        if ((Pfn2->u3.e1.Modified == 0 ) ||
            (Pfn2->u3.e2.ReferenceCount != 0)) {

            //
            // Page is not dirty or not on the modified list,
            // end clustering operation.
            //

            break;
        }
        Page += 1;

        //
        // Add physical page to MDL.
        //

        *Page = MI_GET_PAGE_FRAME_FROM_TRANSITION_PTE (&PteContents);
        ASSERT (PointerPte == Pfn2->PteAddress);
        MiUnlinkPageFromList (Pfn2);

        //
        // Up the reference count for the physical page as there
        // is I/O in progress.
        //

        MI_ADD_LOCKED_PAGE_CHARGE_FOR_MODIFIED_PAGE (Pfn2);

        //
        // Clear the modified bit for the page and set the
        // write in progress bit.
        //

        MI_SET_MODIFIED (Pfn2, 0, 0x24);

        Pfn2->u3.e1.WriteInProgress = 1;

        ModWriterEntry->Mdl.ByteCount += PAGE_SIZE;

        NextPte += 1;
        PointerPte += 1;
    }

    if (HyperMapped != NULL) {
        MiUnmapPageInHyperSpaceFromDpc (Process, HyperMapped);
    }

    ASSERT (BYTES_TO_PAGES (ModWriterEntry->Mdl.ByteCount) <= MmModifiedWriteClusterSize);

    ModWriterEntry->u.LastByte.QuadPart = ModWriterEntry->WriteOffset.QuadPart +
                        ModWriterEntry->Mdl.ByteCount;

    ASSERT (Subsection->ControlArea->u.Flags.Image == 0);

#if DBG
    if ((ULONG)ModWriterEntry->Mdl.ByteCount >
                                ((1+MmModifiedWriteClusterSize)*PAGE_SIZE)) {
        DbgPrintEx (DPFLTR_MM_ID, DPFLTR_ERROR_LEVEL, 
            "Mdl %p, MDL End Offset %lx %lx Subsection %p\n",
                    ModWriterEntry->Mdl,
                    ModWriterEntry->u.LastByte.LowPart,
                    ModWriterEntry->u.LastByte.HighPart,
                    Subsection);
        DbgBreakPoint ();
    }
#endif

    PagesWritten = (ModWriterEntry->Mdl.ByteCount >> PAGE_SHIFT);

    Prcb = KeGetCurrentPrcb ();
    Prcb->MmMappedWriteIoCount += 1;
    Prcb->MmMappedPagesWriteCount += (ULONG)PagesWritten;

    //
    // Increment the count of modified page writes outstanding
    // in the control area.
    //

    ControlArea->ModifiedWriteCount += 1;

    //
    // Increment the number of PFN references.  This allows the file
    // system to purge (i.e. call MmPurgeSection) modified writes.
    //

    ControlArea->NumberOfPfnReferences += 1;

    ModWriterEntry->FileResource = NULL;

    if (ControlArea->u.Flags.BeingPurged == 1) {
        UNLOCK_PFN (OldIrql);
        ModWriterEntry->u.IoStatus.Status = STATUS_FILE_LOCK_CONFLICT;
        ModWriterEntry->u.IoStatus.Information = 0;
        KeRaiseIrql (APC_LEVEL, &OldIrql);
        MiWriteComplete ((PVOID)ModWriterEntry,
                         &ModWriterEntry->u.IoStatus,
                         0);
        KeLowerIrql (OldIrql);
        return;
    }

    //
    // Send the entry to the MappedPageWriter.
    //

    InsertTailList (&MmMappedPageWriterList, &ModWriterEntry->Links);

    KeSetEvent (&MmMappedPageWriterEvent, 0, FALSE);

    UNLOCK_PFN (OldIrql);

    return;
}

VOID
MiGatherPagefilePages (
    IN PMMMOD_WRITER_MDL_ENTRY ModWriterEntry,
    IN PRTL_BITMAP Bitmap
    )

/*++

Routine Description:

    This routine processes the specified modified page by getting
    that page and gather any other pages on the modified list destined
    for the paging file in a large write cluster.  This cluster is
    then written to the paging file.

Arguments:

    ModWriterEntry - Supplies the modified writer entry to use for the write.

    Bitmap - Supplies the captured bitmap to scan for free pagefile blocks.
             This address was captured under the PFN lock by the caller - this
             routine must free the bitmap pool if it detects that a new bitmap
             (ie: the pagefile has been extended) while the lock free scan was
             underway.

Return Value:

    None.

Environment:

    No locks held.

--*/

{
    PFILE_OBJECT File;
    IO_PAGING_PRIORITY IrpPriority;
    PMMPAGING_FILE CurrentPagingFile;
    NTSTATUS Status;
    PPFN_NUMBER Page;
    ULONG StartBit;
    LARGE_INTEGER StartingOffset;
    PFN_NUMBER ClusterSize;
    PFN_NUMBER ThisCluster;
    MMPTE LongPte;
    KIRQL OldIrql;
    ULONG NextColor;
    LOGICAL PageFileFull;
    PMMPFN Pfn1;
    PFN_NUMBER PageFrameIndex;
    PKPRCB Prcb;

    //
    // Page is destined for the paging file.
    //

    OldIrql = PASSIVE_LEVEL;
    CurrentPagingFile = ModWriterEntry->PagingFile;

    File = CurrentPagingFile->File;

    if (MiClusterWritesDisabled == 0) {
        ThisCluster = MmModifiedWriteClusterSize;
    }
    else {
        ThisCluster = 1;
    }

    PageFileFull = FALSE;

    MmInitializeMdl (&ModWriterEntry->Mdl, NULL, PAGE_SIZE);

    ModWriterEntry->Mdl.MdlFlags |= MDL_PAGES_LOCKED;

    ModWriterEntry->Mdl.Size = (CSHORT)(sizeof(MDL) +
                    sizeof(PFN_NUMBER) * MmModifiedWriteClusterSize);

    Page = &ModWriterEntry->Page[0];

    ClusterSize = 0;

    //
    // As scan durations may be long, first scan for a possible hit without
    // holding the PFN lock.
    //

    do {

        //
        // Attempt to cluster MmModifiedWriteClusterSize pages
        // together.  Reduce by one page until we succeed or
        // can't find a single page free in the paging file.
        //

        StartBit = RtlFindClearBits (Bitmap,
                                     (ULONG)ThisCluster,
                                     0);

        if (StartBit != NO_BITS_FOUND) {

            LOCK_PFN (OldIrql);

            //
            // Note that the current bitmap may be different from the one
            // that was passed in.
            //

            StartBit = RtlFindClearBitsAndSet (CurrentPagingFile->Bitmap,
                                               (ULONG)ThisCluster,
                                               StartBit);

            if (StartBit != NO_BITS_FOUND) {

                //
                // The bits have been set and the PFN lock is still held.
                //

                CurrentPagingFile->ReferenceCount -= 1;
                ASSERT (CurrentPagingFile->ReferenceCount == 0);

                if (Bitmap == CurrentPagingFile->Bitmap) {

                    //
                    // The paging file has not been extended so don't free the
                    // existing bitmap.
                    //

                    Bitmap = NULL;
                }

                break;
            }

            UNLOCK_PFN (OldIrql);
        }
        else {
            ThisCluster -= 1;
            PageFileFull = TRUE;
        }

    } while (ThisCluster != 0);

    if (StartBit == NO_BITS_FOUND) {

        if (MiClusterWritesDisabled == 0) {
            ThisCluster = MmModifiedWriteClusterSize;
        }
        else {
            ThisCluster = 1;
        }

        LOCK_PFN (OldIrql);

        CurrentPagingFile->ReferenceCount -= 1;
        ASSERT (CurrentPagingFile->ReferenceCount == 0);

        do {

            //
            // Attempt to cluster MmModifiedWriteClusterSize pages
            // together.  Since we hold the PFN lock, reduce by one
            // half (instead of one page) until we succeed or
            // can't find a single page free in the paging file.
            //

            StartBit = RtlFindClearBitsAndSet (CurrentPagingFile->Bitmap,
                                               (ULONG)ThisCluster,
                                               0);

            if (StartBit != NO_BITS_FOUND) {
                break;
            }

            ThisCluster = ThisCluster >> 1;
            PageFileFull = TRUE;

        } while (ThisCluster != 0);

        if (StartBit == NO_BITS_FOUND) {

            //
            // Paging file must be full.
            //

            KdPrint(("MM MODWRITE: page file full\n"));
            ASSERT(CurrentPagingFile->FreeSpace == 0);

            //
            // Move this entry to the not enough space list,
            // and try again.
            //

            InsertTailList (&MmFreePagingSpaceLow, &ModWriterEntry->Links);
            ModWriterEntry->CurrentList = &MmFreePagingSpaceLow;
            MmNumberOfActiveMdlEntries -= 1;

            if (Bitmap == CurrentPagingFile->Bitmap) {

                //
                // The paging file has not been extended so don't free the
                // existing bitmap.
                //

                Bitmap = NULL;
            }

            UNLOCK_PFN (OldIrql);

            if (Bitmap != NULL) {
                MiRemoveBitMap (&Bitmap);
            }

            MiPageFileFull ();
            return;
        }
    }

    CurrentPagingFile->FreeSpace -= ThisCluster;
    CurrentPagingFile->CurrentUsage += ThisCluster;

    if (CurrentPagingFile->FreeSpace < 32) {
        PageFileFull = TRUE;
    }

    StartingOffset.QuadPart = (UINT64)StartBit << PAGE_SHIFT;

    if (MmTotalPagesForPagingFile == 0) {

        //
        // No modified pages left - other threads may have put them back into
        // working sets, flushed them or deleted them.
        //

        ASSERT (ThisCluster != 0);
        ClusterSize = 0;
        goto bail;
    }

#if MM_MAXIMUM_NUMBER_OF_COLORS > 1
    NextColor will need to be selected round-robin.
#else
    NextColor = 0;
#endif

    MI_GET_MODIFIED_PAGE_ANY_COLOR (PageFrameIndex, NextColor);

    ASSERT (PageFrameIndex != MM_EMPTY_LIST);

    Pfn1 = MI_PFN_ELEMENT (PageFrameIndex);

    //
    // Search through the modified page list looking for other
    // pages destined for the paging file and build a cluster.
    //

    while (ClusterSize != ThisCluster) {

        //
        // Is this page destined for a paging file?
        //

        if (Pfn1->OriginalPte.u.Soft.Prototype == 0) {

            *Page = PageFrameIndex;

            //
            // Remove the page from the modified list. Note that
            // write-in-progress marks the state.
            //
            // Unlink the page so the same page won't be found
            // on the modified page list by color.
            //

            MiUnlinkPageFromList (Pfn1);
            NextColor = MI_GET_NEXT_COLOR(NextColor);

            MI_GET_MODIFIED_PAGE_BY_COLOR (PageFrameIndex,
                                           NextColor);

            //
            // Up the reference count for the physical page as there
            // is I/O in progress.
            //

            MI_ADD_LOCKED_PAGE_CHARGE_FOR_MODIFIED_PAGE (Pfn1);

            //
            // Clear the modified bit for the page and set the
            // write in progress bit.
            //

            MI_SET_MODIFIED (Pfn1, 0, 0x25);

            Pfn1->u3.e1.WriteInProgress = 1;
            ASSERT (Pfn1->OriginalPte.u.Soft.PageFileHigh == 0);

            MI_SET_PAGING_FILE_INFO (LongPte,
                                     Pfn1->OriginalPte,
                                     CurrentPagingFile->PageFileNumber,
                                     StartBit);

#if DBG
            if ((StartBit < 8192) &&
                (CurrentPagingFile->PageFileNumber == 0)) {
                ASSERT ((MmPagingFileDebug[StartBit] & 1) == 0);
                MmPagingFileDebug[StartBit] =
                    (((ULONG_PTR)Pfn1->PteAddress << 3) |
                        ((ClusterSize & 0xf) << 1) | 1);
            }
#endif

            //
            // Change the original PTE contents to refer to
            // the paging file offset where this was written.
            //

            Pfn1->OriginalPte = LongPte;

            ClusterSize += 1;
            Page += 1;
            StartBit += 1;
        }
        else {

            //
            // This page was not destined for a paging file,
            // get another page.
            //
            // Get a page of the same color as the one which
            // was not usable.
            //

            MI_GET_MODIFIED_PAGE_BY_COLOR (PageFrameIndex,
                                           NextColor);
        }

        if (PageFrameIndex == MM_EMPTY_LIST) {
            break;
        }

        Pfn1 = MI_PFN_ELEMENT (PageFrameIndex);

    } //end while

    if (ClusterSize != ThisCluster) {

bail:

        //
        // A complete cluster could not be located, free the
        // excess page file space that was reserved and adjust
        // the size of the packet.
        //

        RtlClearBits (CurrentPagingFile->Bitmap,
                      StartBit,
                      (ULONG)(ThisCluster - ClusterSize));

        CurrentPagingFile->FreeSpace += ThisCluster - ClusterSize;
        CurrentPagingFile->CurrentUsage -= ThisCluster - ClusterSize;

        //
        // If there are no pages to write, don't issue a write
        // request and restart the scan loop.
        //

        if (ClusterSize == 0) {

            //
            // No pages to write.  Insert the entry back in the list.
            //

            if (IsListEmpty (&ModWriterEntry->PagingListHead->ListHead)) {
                KeSetEvent (&ModWriterEntry->PagingListHead->Event,
                            0,
                            FALSE);
            }

            InsertTailList (&ModWriterEntry->PagingListHead->ListHead,
                            &ModWriterEntry->Links);

            if (Bitmap == CurrentPagingFile->Bitmap) {

                //
                // The paging file has not been extended so don't free the
                // existing bitmap.
                //

                Bitmap = NULL;
            }

            UNLOCK_PFN (OldIrql);

            if (Bitmap != NULL) {
                MiRemoveBitMap (&Bitmap);
            }

            if (PageFileFull == TRUE) {
                MiPageFileFull ();
            }
            return;
        }
    }

    if (CurrentPagingFile->PeakUsage <
                                CurrentPagingFile->CurrentUsage) {
        CurrentPagingFile->PeakUsage =
                                CurrentPagingFile->CurrentUsage;
    }

    ModWriterEntry->LastPageToWrite = StartBit - 1;

    //
    // Release the PFN lock and wait for the write to complete.
    //

    UNLOCK_PFN (OldIrql);

    Prcb = KeGetCurrentPrcb ();
    InterlockedIncrement (&Prcb->MmDirtyWriteIoCount);

    InterlockedExchangeAdd (&Prcb->MmDirtyPagesWriteCount,
                            (LONG) ClusterSize);

    ModWriterEntry->Mdl.ByteCount = (ULONG)(ClusterSize * PAGE_SIZE);

    KeQuerySystemTime (&ModWriterEntry->IssueTime);

    IrpPriority = IoPagingPriorityNormal;

    if (MiModifiedWriteBurstCount != 0) {
        if (MmAvailablePages < MM_PLENTY_FREE_LIMIT) {
            MiModifiedWriteBurstCount -= 1;
            IrpPriority = IoPagingPriorityHigh;
        }
        else {
            MiModifiedWriteBurstCount = 0;
        }
    }
    else if (MmAvailablePages < MM_HIGH_LIMIT) {
        MiModifiedWriteBurstCount = MI_MAXIMUM_PRIORITY_BURST;
        IrpPriority = IoPagingPriorityHigh;
    }
    else if (MmAvailablePages < MM_TIGHT_LIMIT) {
        MiModifiedWriteBurstCount = MI_MAXIMUM_PRIORITY_BURST / 4;
        IrpPriority = IoPagingPriorityHigh;
    }

    MI_PAGEFILE_WRITE (ModWriterEntry,
                       &ModWriterEntry->IssueTime,
                       IrpPriority,
                       (NTSTATUS)-1);

    //
    // Issue the write request.
    //

    Status = IoAsynchronousPageWrite (File,
                                      &ModWriterEntry->Mdl,
                                      &StartingOffset,
                                      MiWriteComplete,
                                      (PVOID)ModWriterEntry,
                                      IrpPriority,
                                      &ModWriterEntry->u.IoStatus,
                                      &ModWriterEntry->Irp);

    if (NT_ERROR (Status)) {
        KdPrint(("MM MODWRITE: modified page write failed %lx\n", Status));

        //
        // An error has occurred, disable APCs and
        // call the write completion routine.
        //

        ModWriterEntry->u.IoStatus.Status = Status;
        ModWriterEntry->u.IoStatus.Information = 0;
        KeRaiseIrql (APC_LEVEL, &OldIrql);
        MiWriteComplete ((PVOID)ModWriterEntry,
                         &ModWriterEntry->u.IoStatus,
                         0);
        KeLowerIrql (OldIrql);
    }

    if ((Bitmap != NULL) && (Bitmap != CurrentPagingFile->Bitmap)) {

        //
        // The paging file has been extended so free the entry bitmap.
        //

        MiRemoveBitMap (&Bitmap);
    }

    if (PageFileFull == TRUE) {
        MiPageFileFull ();
    }

    return;
}

VOID
MiMappedPageWriter (
    IN PVOID StartContext
    )

/*++

Routine Description:

    Implements the NT secondary modified page writer thread.
    Requests for writes to mapped files are sent to this thread.
    This is required as the writing of mapped file pages could cause
    page faults resulting in requests for free pages.  But there
    could be no free pages - hence a deadlock.  Rather than deadlock
    the whole system waiting on the modified page writer, creating
    a secondary thread allows that thread to block without affecting
    ongoing page file writes.

Arguments:

    StartContext - not used.

Return Value:

    None.

Environment:

    Kernel mode.

--*/

{
    KIRQL OldIrql;
    NTSTATUS Status;
    KEVENT TempEvent;
    PETHREAD CurrentThread;
    IO_PAGING_PRIORITY IrpPriority;
    PMMMOD_WRITER_MDL_ENTRY ModWriterEntry;

    UNREFERENCED_PARAMETER (StartContext);

    //
    // Make this a real time thread.
    //

    CurrentThread = PsGetCurrentThread ();

    KeSetPriorityThread (&CurrentThread->Tcb, LOW_REALTIME_PRIORITY + 1);

    CurrentThread->MemoryMaker = 1;

    //
    // Let the file system know that we are getting resources.
    //

    FsRtlSetTopLevelIrpForModWriter ();

    KeInitializeEvent (&TempEvent, NotificationEvent, FALSE);

    while (TRUE) {

        KeWaitForSingleObject (&MmMappedPageWriterEvent,
                               WrVirtualMemory,
                               KernelMode,
                               FALSE,
                               NULL);

        LOCK_PFN (OldIrql);

        if (IsListEmpty (&MmMappedPageWriterList)) {
            KeClearEvent (&MmMappedPageWriterEvent);
            UNLOCK_PFN (OldIrql);
        }
        else {

            ModWriterEntry = (PMMMOD_WRITER_MDL_ENTRY)RemoveHeadList (
                                                &MmMappedPageWriterList);

            UNLOCK_PFN (OldIrql);

            if (ModWriterEntry->ControlArea->u.Flags.FailAllIo == 1) {
                Status = STATUS_UNSUCCESSFUL;
            }
            else {
                Status = FsRtlAcquireFileForModWriteEx (ModWriterEntry->File,
                                                        &ModWriterEntry->u.LastByte,
                                                        &ModWriterEntry->FileResource);
                if (NT_SUCCESS(Status)) {

                    //
                    // Issue the write request.
                    //

                    IrpPriority = IoPagingPriorityNormal;

                    if (MiMappedWriteBurstCount != 0) {
                        if (MmAvailablePages < MM_PLENTY_FREE_LIMIT) {
                            MiMappedWriteBurstCount -= 1;
                            IrpPriority = IoPagingPriorityHigh;
                        }
                        else {
                            MiMappedWriteBurstCount = 0;
                        }
                    }
                    else if (MmAvailablePages < MM_HIGH_LIMIT) {
                        MiMappedWriteBurstCount = MI_MAXIMUM_PRIORITY_BURST;
                        IrpPriority = IoPagingPriorityHigh;
                    }
                    else if (MmAvailablePages < MM_TIGHT_LIMIT) {
                        MiMappedWriteBurstCount = MI_MAXIMUM_PRIORITY_BURST / 4;
                        IrpPriority = IoPagingPriorityHigh;
                    }

                    Status = IoAsynchronousPageWrite (ModWriterEntry->File,
                                                      &ModWriterEntry->Mdl,
                                                      &ModWriterEntry->WriteOffset,
                                                      MiWriteComplete,
                                                      (PVOID) ModWriterEntry,
                                                      IrpPriority,
                                                      &ModWriterEntry->u.IoStatus,
                                                      &ModWriterEntry->Irp);
                }
                else {

                    //
                    // Unable to get the file system resources, set error status
                    // to lock conflict (ignored by MiWriteComplete) so the APC
                    // routine is explicitly called.
                    //

                    Status = STATUS_FILE_LOCK_CONFLICT;
                }
            }

            if (NT_ERROR (Status)) {

                //
                // An error has occurred, disable APCs and
                // call the write completion routine.
                //

                ModWriterEntry->u.IoStatus.Status = Status;
                ModWriterEntry->u.IoStatus.Information = 0;
                KeRaiseIrql (APC_LEVEL, &OldIrql);
                MiWriteComplete ((PVOID)ModWriterEntry,
                                 &ModWriterEntry->u.IoStatus,
                                 0);
                KeLowerIrql (OldIrql);
            }
        }
    }
}


BOOLEAN
MmDisableModifiedWriteOfSection (
    __in PSECTION_OBJECT_POINTERS SectionObjectPointer
    )

/*++

Routine Description:

    This function disables page writing by the modified page writer for
    the section which is mapped by the specified file object pointer.

    This should only be used for files which CANNOT be mapped by user
    programs, e.g., volume files, directory files, etc.

Arguments:

    SectionObjectPointer - Supplies a pointer to the section objects


Return Value:

    Returns TRUE if the operation was a success, FALSE if either
    the there is no section or the section already has a view.

--*/

{
    KIRQL OldIrql;
    BOOLEAN state;
    PCONTROL_AREA ControlArea;

    state = TRUE;

    LOCK_PFN (OldIrql);

    ControlArea = ((PCONTROL_AREA)(SectionObjectPointer->DataSectionObject));

    if (ControlArea != NULL) {
        if (ControlArea->NumberOfMappedViews == 0) {

            //
            // There are no views to this section, indicate no modified
            // page writing is allowed.
            //

            ControlArea->u.Flags.NoModifiedWriting = 1;
        }
        else {

            //
            // Return the current modified page writing state.
            //

            state = (BOOLEAN)ControlArea->u.Flags.NoModifiedWriting;
        }
    }
    else {

        //
        // This file no longer has an associated segment.
        //

        state = 0;
    }

    UNLOCK_PFN (OldIrql);
    return state;
}


BOOLEAN
MmEnableModifiedWriteOfSection (
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer
    )

/*++

Routine Description:

    This function enables page writing by the modified page writer for
    the section which is mapped by the specified file object pointer.

    This should only be used for files which have previously been disabled.
    Normal sections are created allowing modified write.

Arguments:

    SectionObjectPointer - Supplies a pointer to the section objects


Return Value:

    Returns TRUE if the operation was a success, FALSE if not.

--*/

{
    KIRQL OldIrql;
    PCONTROL_AREA ControlArea;
    PMMPFN Pfn1;
    PSUBSECTION Subsection;
    PFN_NUMBER PageFrameIndex;
    PFN_NUMBER NextPageFrameIndex;

    LOCK_PFN2 (OldIrql);

    ControlArea = ((PCONTROL_AREA)(SectionObjectPointer->DataSectionObject));

    if ((ControlArea != NULL) && (ControlArea->u.Flags.NoModifiedWriting)) {

        //
        // Indicate modified page writing is now allowed.
        //

        ControlArea->u.Flags.NoModifiedWriting = 0;

        //
        // Move any straggling pages on the modnowrite list back onto the
        // modified list otherwise they would be orphaned and this could
        // cause us to run out of pages.
        //

        if (MmModifiedNoWritePageListHead.Total != 0) {

            PageFrameIndex = MmModifiedNoWritePageListHead.Flink;
    
            do {

                Pfn1 = MI_PFN_ELEMENT (PageFrameIndex);

                NextPageFrameIndex = Pfn1->u1.Flink;

                Subsection = MiGetSubsectionAddress (&Pfn1->OriginalPte);

                if (ControlArea == Subsection->ControlArea) {
    
                    //
                    // This page must be moved to the modified list.
                    //
    
                    MiUnlinkPageFromList (Pfn1);

                    MiInsertPageInList (&MmModifiedPageListHead,
                                        PageFrameIndex);
                }

                PageFrameIndex = NextPageFrameIndex;

            } while (PageFrameIndex != MM_EMPTY_LIST);
        }
    }

    UNLOCK_PFN2 (OldIrql);

    return TRUE;
}


#define ROUND_UP(VALUE,ROUND) ((ULONG)(((ULONG)VALUE + \
                               ((ULONG)ROUND - 1L)) & (~((ULONG)ROUND - 1L))))
NTSTATUS
MmGetPageFileInformation (
    OUT PVOID SystemInformation,
    IN ULONG SystemInformationLength,
    OUT PULONG Length
    )

/*++

Routine Description:

    This routine returns information about the currently active paging
    files.

Arguments:

    SystemInformation - Returns the paging file information.

    SystemInformationLength - Supplies the length of the SystemInformation
                              buffer.

    Length - Returns the length of the paging file information placed in the
             buffer.

Return Value:

    Returns the status of the operation.

--*/

{
    PSYSTEM_PAGEFILE_INFORMATION PageFileInfo;
    ULONG NextEntryOffset = 0;
    ULONG TotalSize = 0;
    ULONG i;
    UNICODE_STRING UserBufferPageFileName;

    PAGED_CODE();

    *Length = 0;
    PageFileInfo = (PSYSTEM_PAGEFILE_INFORMATION)SystemInformation;

    PageFileInfo->TotalSize = 0;

    for (i = 0; i < MmNumberOfPagingFiles; i += 1) {
        PageFileInfo = (PSYSTEM_PAGEFILE_INFORMATION)(
                                (PUCHAR)PageFileInfo + NextEntryOffset);
        NextEntryOffset = sizeof(SYSTEM_PAGEFILE_INFORMATION);
        TotalSize += sizeof(SYSTEM_PAGEFILE_INFORMATION);

        if (TotalSize > SystemInformationLength) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }

        PageFileInfo->TotalSize = (ULONG)MmPagingFile[i]->Size;
        PageFileInfo->TotalInUse = (ULONG)MmPagingFile[i]->CurrentUsage;
        PageFileInfo->PeakUsage = (ULONG)MmPagingFile[i]->PeakUsage;

        //
        // The PageFileName portion of the UserBuffer must be saved locally
        // to protect against a malicious thread changing the contents.  This
        // is because we will reference the contents ourselves when the actual
        // string is copied out carefully below.
        //

        UserBufferPageFileName.Length = MmPagingFile[i]->PageFileName.Length;
        UserBufferPageFileName.MaximumLength = (USHORT)(MmPagingFile[i]->PageFileName.Length + sizeof(WCHAR));
        UserBufferPageFileName.Buffer = (PWCHAR)(PageFileInfo + 1);

        PageFileInfo->PageFileName = UserBufferPageFileName;

        TotalSize += ROUND_UP (UserBufferPageFileName.MaximumLength,
                               sizeof(ULONG));
        NextEntryOffset += ROUND_UP (UserBufferPageFileName.MaximumLength,
                                     sizeof(ULONG));

        if (TotalSize > SystemInformationLength) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }

        //
        // Carefully reference the user buffer here.
        //

        RtlCopyMemory(UserBufferPageFileName.Buffer,
                      MmPagingFile[i]->PageFileName.Buffer,
                      MmPagingFile[i]->PageFileName.Length);
        UserBufferPageFileName.Buffer[
                    MmPagingFile[i]->PageFileName.Length/sizeof(WCHAR)] = UNICODE_NULL;
        PageFileInfo->NextEntryOffset = NextEntryOffset;
    }
    PageFileInfo->NextEntryOffset = 0;
    *Length = TotalSize;
    return STATUS_SUCCESS;
}


NTSTATUS
MiCheckPageFileMapping (
    IN PFILE_OBJECT File
    )

/*++

Routine Description:

    Non-pageable routine to check to see if a given file has
    no sections and therefore is eligible to become a paging file.

Arguments:

    File - Supplies a pointer to the file object.

Return Value:

    Returns STATUS_SUCCESS if the file can be used as a paging file.

--*/

{
    KIRQL OldIrql;

    LOCK_PFN (OldIrql);

    if (File->SectionObjectPointer == NULL) {
        UNLOCK_PFN (OldIrql);
        return STATUS_SUCCESS;
    }

    if ((File->SectionObjectPointer->DataSectionObject != NULL) ||
        (File->SectionObjectPointer->ImageSectionObject != NULL)) {

        UNLOCK_PFN (OldIrql);
        return STATUS_INCOMPATIBLE_FILE_MAP;
    }
    UNLOCK_PFN (OldIrql);
    return STATUS_SUCCESS;

}


VOID
MiInsertPageFileInList (
    VOID
    )

/*++

Routine Description:

    Non-pageable routine to add a page file into the list
    of system wide page files.

Arguments:

    None, implicitly found through page file structures.

Return Value:

    None.  Operation cannot fail.

--*/

{
    ULONG i;
    KIRQL OldIrql;
    SIZE_T FreeSpace;
    SIZE_T MaximumSize;

    LOCK_PFN (OldIrql);

    MmNumberOfPagingFiles += 1;

    if (IsListEmpty (&MmPagingFileHeader.ListHead)) {
        KeSetEvent (&MmPagingFileHeader.Event, 0, FALSE);
    }

    for (i = 0; i < MM_PAGING_FILE_MDLS; i += 1) {

        InsertTailList (&MmPagingFileHeader.ListHead,
                     &MmPagingFile[MmNumberOfPagingFiles - 1]->Entry[i]->Links);

        MmPagingFile[MmNumberOfPagingFiles - 1]->Entry[i]->CurrentList =
                                                &MmPagingFileHeader.ListHead;
    }

    FreeSpace = MmPagingFile[MmNumberOfPagingFiles - 1]->FreeSpace;
    MaximumSize = MmPagingFile[MmNumberOfPagingFiles - 1]->MaximumSize;

    MmPagingFile[MmNumberOfPagingFiles - 1]->ReferenceCount = 0;

    MmNumberOfActiveMdlEntries += 2;

    UNLOCK_PFN (OldIrql);

    //
    // Increase the systemwide commit limit maximum first.  Then increase
    // the current limit.
    //

    InterlockedExchangeAddSizeT (&MmTotalCommitLimitMaximum, MaximumSize);

    InterlockedExchangeAddSizeT (&MmTotalCommitLimit, FreeSpace);

    return;
}

VOID
MiPageFileFull (
    VOID
    )

/*++

Routine Description:

    This routine is called when no space can be found in a paging file.
    It looks through all the paging files to see if ample space is
    available and if not, tries to expand the paging files.

    If more than 90% of all the paging files are in use, the commitment limit
    is set to the total and then 100 pages are added.

Arguments:

    None.

Return Value:

    None.

--*/

{
    ULONG i;
    PFN_NUMBER Total;
    PFN_NUMBER Free;
    SIZE_T QuotaCharge;

    if (MmNumberOfPagingFiles == 0) {
        return;
    }

    Total = 0;
    Free = 0;

    for (i = 0; i < MmNumberOfPagingFiles; i += 1) {
        Total += MmPagingFile[i]->Size;
        Free += MmPagingFile[i]->FreeSpace;
    }

    //
    // Check to see if more than 90% of the total space has been used.
    //

    if (((Total >> 5) + (Total >> 4)) >= Free) {

        //
        // Try to expand the paging files.
        //
        // Check (unsynchronized is ok) commit limits of each pagefile.
        // If all the pagefiles are already at their maximums, then don't
        // make things worse by setting commit to the maximum - this gives
        // systems with lots of memory a longer lease on life when they have
        // small pagefiles.
        //

        i = 0;

        do {
            if (MmPagingFile[i]->MaximumSize > MmPagingFile[i]->Size) {
                break;
            }
            i += 1;
        } while (i < MmNumberOfPagingFiles);

        if (i == MmNumberOfPagingFiles) {

            //
            // No pagefiles can be extended,
            // display a popup if we haven't before.
            //

            MiCauseOverCommitPopup ();

            return;
        }

        QuotaCharge = MmTotalCommitLimit - MmTotalCommittedPages;

        //
        // IFF the total number of committed pages is less than the limit,
        // or in any event, no more than 50 pages past the limit,
        // then charge pages against the commitment to trigger pagefile
        // expansion.
        //
        // If the total commit is more than 50 past the limit, then don't
        // bother trying to extend the pagefile.
        //

        if ((SSIZE_T)QuotaCharge + 50 > 0) {

            if ((SSIZE_T)QuotaCharge < 50) {
                QuotaCharge = 50;
            }

            MiChargeCommitmentCantExpand (QuotaCharge, TRUE);

            MM_TRACK_COMMIT (MM_DBG_COMMIT_PAGEFILE_FULL, QuotaCharge);

            //
            // Display a popup if we haven't before.
            //

            MiCauseOverCommitPopup ();

            MiReturnCommitment (QuotaCharge);

            MM_TRACK_COMMIT (MM_DBG_COMMIT_RETURN_PAGEFILE_FULL, QuotaCharge);
        }
    }
    return;
}

VOID
MiFlushAllPages (
    VOID
    )

/*++

Routine Description:

    Forces a write of all modified pages.

Arguments:

    None.

Return Value:

    None.

Environment:

    Kernel mode.  No locks held.  APC_LEVEL or less.

--*/

{
    ULONG j;

    //
    // If there are no paging files, then no sense in waiting for
    // modified writes to complete.
    //

    if (MmNumberOfPagingFiles == 0) {
        return;
    }

    InterlockedIncrement (&MmWriteAllModifiedPages);
    KeSetEvent (&MmModifiedPageWriterEvent, 0, FALSE);

    j = 0xff;

    do {
        KeDelayExecutionThread (KernelMode, FALSE, (PLARGE_INTEGER)&Mm30Milliseconds);
        j -= 1;
    } while ((MmModifiedPageListHead.Total > 50) && (j > 0));

    InterlockedDecrement (&MmWriteAllModifiedPages);
    return;
}


LOGICAL
MiIssuePageExtendRequest (
    IN PMMPAGE_FILE_EXPANSION PageExtend
    )

/*++

Routine Description:

    Queue a message to the segment dereferencing / pagefile extending
    thread to see if the page file can be extended.  Extension is done
    in the context of a system thread due to mutexes which the current
    thread may be holding.

Arguments:

    PageExtend - Supplies a pointer to the page file extension request.

Return Value:

    TRUE indicates the request completed.  FALSE indicates the request timed
    out and was removed.

Environment:

    Kernel mode.  No locks held.  APC_LEVEL or below.

--*/

{
    ULONG i;
    KIRQL OldIrql;
    NTSTATUS status;
    PLIST_ENTRY NextEntry;
    PETHREAD Thread;

    Thread = PsGetCurrentThread ();

    //
    // The segment dereference thread cannot wait for itself !
    //

    if (Thread->StartAddress == (PVOID)(ULONG_PTR)MiDereferenceSegmentThread) {
        return FALSE;
    }

    //
    // Avoid repeatedly waking the segment dereference thread just for it to
    // perform a no-op by first checking whether the pagefile(s) are already
    // at their maximum (or don't exist) - if so, just return a failure.
    //

    for (i = 0; i < MmNumberOfPagingFiles; i += 1) {
        if (MmPagingFile[i]->Size < MmPagingFile[i]->MaximumSize) {
            break;
        }
    }

    if (i == MmNumberOfPagingFiles) {
        return FALSE;
    }

    ExAcquireSpinLock (&MmDereferenceSegmentHeader.Lock, &OldIrql);

    InsertHeadList (&MmDereferenceSegmentHeader.ListHead,
                    &PageExtend->DereferenceList);

    ExReleaseSpinLock (&MmDereferenceSegmentHeader.Lock, OldIrql);

    KeReleaseSemaphore (&MmDereferenceSegmentHeader.Semaphore,
                        0L,
                        1L,
                        TRUE);

    //
    // Wait for the thread to extend the paging file.
    //

    status = KeWaitForSingleObject (&PageExtend->Event,
                                    Executive,
                                    KernelMode,
                                    FALSE,
                                    (PageExtend->RequestedExpansionSize < 10) ?
                                        (PLARGE_INTEGER)&MmOneSecond : (PLARGE_INTEGER)&MmTwentySeconds);

    if (status == STATUS_TIMEOUT) {

        //
        // The wait has timed out, if this request has not
        // been processed, remove it from the list and check
        // to see if we should allow this request to succeed.
        // This prevents a deadlock between the file system
        // trying to allocate memory in the FSP and the
        // segment dereferencing thread trying to close a
        // file object, and waiting in the file system.
        //

        KdPrint(("MiIssuePageExtendRequest: wait timed out, page-extend= %p, quota = %lx\n",
                   PageExtend, PageExtend->RequestedExpansionSize));

        ExAcquireSpinLock (&MmDereferenceSegmentHeader.Lock, &OldIrql);

        NextEntry = MmDereferenceSegmentHeader.ListHead.Flink;

        while (NextEntry != &MmDereferenceSegmentHeader.ListHead) {

            //
            // Check to see if this is the entry we are waiting for.
            //

            if (NextEntry == &PageExtend->DereferenceList) {

                //
                // Remove this entry.
                //

                RemoveEntryList (&PageExtend->DereferenceList);
                ExReleaseSpinLock (&MmDereferenceSegmentHeader.Lock, OldIrql);
                return FALSE;
            }
            NextEntry = NextEntry->Flink;
        }

        ExReleaseSpinLock (&MmDereferenceSegmentHeader.Lock, OldIrql);

        //
        // Entry is being processed, wait for completion.
        //

        KdPrint (("MiIssuePageExtendRequest: rewaiting...\n"));

        KeWaitForSingleObject (&PageExtend->Event,
                               Executive,
                               KernelMode,
                               FALSE,
                               NULL);
    }

    return TRUE;
}


VOID
MiIssuePageExtendRequestNoWait (
    IN PFN_NUMBER SizeInPages
    )

/*++

Routine Description:

    Queue a message to the segment dereferencing / pagefile extending
    thread to see if the page file can be extended.  Extension is done
    in the context of a system thread due to mutexes which the current
    thread may be holding.

Arguments:

    SizeInPages - Supplies the size in pages to increase the page file(s) by.
                  This is rounded up to a 1MB multiple by this routine.

Return Value:

    TRUE indicates the request completed.  FALSE indicates the request timed
    out and was removed.

Environment:

    Kernel mode.  No locks held.  DISPATCH_LEVEL or less.

    Note this routine must be very careful to not use any paged
    pool as the only reason it is being called is because pool is depleted.

--*/

{
    KIRQL OldIrql;
    LONG OriginalInProgress;

    OriginalInProgress = InterlockedCompareExchange (
                            &MmAttemptForCantExtend.InProgress, 1, 0);

    if (OriginalInProgress != 0) {

        //
        // An expansion request is already in progress, assume
        // it will help enough (another can always be issued later) and
        // that it will succeed.
        //

        return;
    }

    ASSERT (MmAttemptForCantExtend.InProgress == 1);

    SizeInPages = (SizeInPages + ONEMB_IN_PAGES - 1) & ~(ONEMB_IN_PAGES - 1);

    MmAttemptForCantExtend.ActualExpansion = 0;
    MmAttemptForCantExtend.RequestedExpansionSize = SizeInPages;

    ExAcquireSpinLock (&MmDereferenceSegmentHeader.Lock, &OldIrql);

    InsertHeadList (&MmDereferenceSegmentHeader.ListHead,
                    &MmAttemptForCantExtend.DereferenceList);

    ExReleaseSpinLock (&MmDereferenceSegmentHeader.Lock, OldIrql);

    KeReleaseSemaphore (&MmDereferenceSegmentHeader.Semaphore,
                        0L,
                        1L,
                        FALSE);

    return;
}

