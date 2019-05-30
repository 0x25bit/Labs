/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    cmwrapr.c

Abstract:

    This module contains the source for wrapper routines called by the
    hive code, which in turn call the appropriate NT routines.

--*/

#include    "cmp.h"

VOID
CmpUnmapCmViewSurroundingOffset(
        IN  PCMHIVE             CmHive,
        IN  ULONG               FileOffset
        );

#ifdef ALLOC_DATA_PRAGMA
#pragma data_seg("PAGEDATA")
#endif
ULONG perftouchbuffer = 0;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,CmpAllocate)
#ifdef POOL_TAGGING
#pragma alloc_text(PAGE,CmpAllocateTag)
#endif
#pragma alloc_text(PAGE,CmpFree)
#pragma alloc_text(PAGE,CmpDoFileSetSize)
#pragma alloc_text(PAGE,CmpCreateEvent)
#pragma alloc_text(PAGE,CmpFileRead)
#pragma alloc_text(PAGE,CmpFileWrite)
#pragma alloc_text(PAGE,CmpFileFlush)
#pragma alloc_text(PAGE,CmpFileWriteThroughCache)
#endif

extern BOOLEAN CmpNoWrite;


//
// never read more than 64k, neither the filesystem nor some disk drivers
// like it much.
//
#define MAX_FILE_IO 0x10000

#define CmpIoFileRead       1
#define CmpIoFileWrite      2
#define CmpIoFileSetSize    3
#define CmpIoFileFlush      4

extern struct {
    ULONG       Action;
    HANDLE      Handle;
    NTSTATUS    Status;
} CmRegistryIODebug;

extern BOOLEAN CmpFlushOnLockRelease;

//
// Storage management
//

PVOID
CmpAllocate(
    ULONG   Size,
    BOOLEAN UseForIo,
    ULONG   Tag
    )
/*++

Routine Description:

    This routine makes more memory available to a hive.

    It is environment specific.

Arguments:

    Size - amount of space caller wants

    UseForIo - TRUE if object allocated will be target of I/O,
               FALSE if not.

Return Value:

    NULL if failure, address of allocated block if not.

--*/
{
    PVOID   result;
    ULONG   pooltype;

#if DBG
    PVOID   Caller;
    PVOID   CallerCaller;
    RtlGetCallersAddress(&Caller, &CallerCaller);
#endif

    if (CmpClaimGlobalQuota(Size) == FALSE) {
        return NULL;
    }

    pooltype = (UseForIo) ? PagedPoolCacheAligned : PagedPool;
    result = ExAllocatePoolWithTag(
                pooltype,
                Size,
                Tag
                );

    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_POOL,"**CmpAllocate: allocate:%08lx, ", Size));
    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_POOL,"type:%d, at:%08lx  ", PagedPool, result));
    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_POOL,"c:%p  cc:%p\n", Caller, CallerCaller));

    if (result == NULL) {
        CmpReleaseGlobalQuota(Size);
    }

    return result;
}

#ifdef POOL_TAGGING
PVOID
CmpAllocateTag(
    ULONG   Size,
    BOOLEAN UseForIo,
    ULONG   Tag
    )
/*++

Routine Description:

    This routine makes more memory available to a hive.

    It is environment specific.

Arguments:

    Size - amount of space caller wants

    UseForIo - TRUE if object allocated will be target of I/O,
               FALSE if not.

Return Value:

    NULL if failure, address of allocated block if not.

--*/
{
    PVOID   result;
    ULONG   pooltype;

#if DBG
    PVOID   Caller;
    PVOID   CallerCaller;
    RtlGetCallersAddress(&Caller, &CallerCaller);
#endif

    if (CmpClaimGlobalQuota(Size) == FALSE) {
        return NULL;
    }

    pooltype = (UseForIo) ? PagedPoolCacheAligned : PagedPool;
    result = ExAllocatePoolWithTag(
                pooltype,
                Size,
                Tag
                );

    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_POOL,"**CmpAllocate: allocate:%08lx, ", Size));
    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_POOL,"type:%d, at:%08lx  ", PagedPool, result));
    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_POOL,"c:%p  cc:%p\n", Caller, CallerCaller));

    if (result == NULL) {
        CmpReleaseGlobalQuota(Size);
    }
    return result;
}
#endif


VOID
CmpFree(
    PVOID   MemoryBlock,
    ULONG   GlobalQuotaSize
    )
/*++

Routine Description:

    This routine frees memory that has been allocated by the registry.

    It is environment specific


Arguments:

    MemoryBlock - supplies address of memory object to free

    GlobalQuotaSize - amount of global quota to release

Return Value:

    NONE

--*/
{
#if DBG
    PVOID   Caller;
    PVOID   CallerCaller;
    RtlGetCallersAddress(&Caller, &CallerCaller);
    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_POOL,"**FREEING:%08lx c,cc:%p,%p\n", MemoryBlock, Caller, CallerCaller));
#endif
    ASSERT(GlobalQuotaSize > 0);
    CmpReleaseGlobalQuota(GlobalQuotaSize);
    ExFreePool(MemoryBlock);
    return;
}


NTSTATUS
CmpDoFileSetSize(
    PHHIVE      Hive,
    ULONG       FileType,
    ULONG       FileSize,
    ULONG       OldFileSize
    )
/*++

Routine Description:

    This routine sets the size of a file.  It must not return until
    the size is guaranteed.

    It is environment specific.

    Must be running in the context of the cmp worker thread.

Arguments:

    Hive - Hive we are doing I/O for

    FileType - which supporting file to use

    FileSize - 32 bit value to set the file's size to

    OldFileSize - old file size, in order to determine if this is a shrink;
                - ignored if file type is not primary, or hive doesn't use 
                the mapped views technique

Return Value:

    FALSE if failure
    TRUE if success

--*/
{
    PCMHIVE                         CmHive;
    HANDLE                          FileHandle;
    NTSTATUS                        Status;
    FILE_END_OF_FILE_INFORMATION    FileInfo;
    IO_STATUS_BLOCK                 IoStatus;
    BOOLEAN                         oldFlag;
    LARGE_INTEGER                   FileOffset;         // where the mapping starts

    ASSERT(FIELD_OFFSET(CMHIVE, Hive) == 0);

    CmHive = (PCMHIVE)Hive;
    FileHandle = CmHive->FileHandles[FileType];
    if (FileHandle == NULL) {
        return TRUE;
    }

    //
    // disable hard error popups, to avoid self deadlock on bogus devices
    //
    oldFlag = IoSetThreadHardErrorMode(FALSE);

    FileInfo.EndOfFile.HighPart = 0L;
    if( FileType == HFILE_TYPE_PRIMARY ) {
        FileInfo.EndOfFile.LowPart  = ROUND_UP(FileSize, CM_FILE_GROW_INCREMENT);
    } else {
        FileInfo.EndOfFile.LowPart  = FileSize;
    }

    ASSERT_PASSIVE_LEVEL();

    Status = ZwSetInformationFile(
                FileHandle,
                &IoStatus,
                (PVOID)&FileInfo,
                sizeof(FILE_END_OF_FILE_INFORMATION),
                FileEndOfFileInformation
                );

    if (NT_SUCCESS(Status)) {
        ASSERT(IoStatus.Status == Status);
    } else {
        
        //
        // set debugging info
        //
        CmRegistryIODebug.Action = CmpIoFileSetSize;
        CmRegistryIODebug.Handle = FileHandle;
        CmRegistryIODebug.Status = Status;
#if DBG
        DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_TRACE_LEVEL,"CmpFileSetSize:\tHandle=%08lx  OldLength = %08lx NewLength=%08lx  \n", 
                                                        FileHandle, OldFileSize, FileSize);
#endif
        if( (Status == STATUS_DISK_FULL) && ExIsResourceAcquiredExclusiveLite(&CmpRegistryLock) ) {
            DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_TRACE_LEVEL,"Disk is full while attempting to grow file %lx; will flush upon lock release\n",FileHandle);
            CmpFlushOnLockRelease = TRUE;;
        }
    }

    //
    // restore hard error popups mode
    //
    IoSetThreadHardErrorMode(oldFlag);
    

    //
    // purge
    //
    if( HiveWritesThroughCache(Hive,FileType) && (OldFileSize > FileSize)) {
        //
        // first we have to unmap any possible mapped views in the last 256K window
        // to avoid deadlock on CcWaitOnActiveCount inside CcPurgeCacheSection call below
        //
        ULONG   Offset = FileSize & (~(_256K - 1));
        //
        // we are not allowed to shrink in shared mode.
        //
	    ASSERT_HIVE_WRITER_LOCK_OWNED((PCMHIVE)Hive);

        while( Offset < OldFileSize ) {
            CmpUnmapCmViewSurroundingOffset((PCMHIVE)Hive,Offset);
            Offset += CM_VIEW_SIZE;
        }

        //
        // we need to take extra precaution here and unmap the very last view too
        //
        
        FileOffset.HighPart = 0;
        FileOffset.LowPart = FileSize;
        //
        // This is a shrink; Inform cache manager of the change of the size
        //
        CcPurgeCacheSection( ((PCMHIVE)Hive)->FileObject->SectionObjectPointer, (PLARGE_INTEGER)(((ULONG_PTR)(&FileOffset)) + 1), 
                            OldFileSize - FileSize, FALSE );

        //
        // Flush out this view to clear out the Cc dirty hints
        //
        CcFlushCache( ((PCMHIVE)Hive)->FileObject->SectionObjectPointer, (PLARGE_INTEGER)(((ULONG_PTR)(&FileOffset)) + 1),/*we are private writers*/
                            OldFileSize - FileSize,NULL);

    }
    
    return Status;
}

NTSTATUS
CmpCreateEvent(
    IN EVENT_TYPE  eventType,
    OUT PHANDLE eventHandle,
    OUT PKEVENT *event
    )
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES obja;

    InitializeObjectAttributes( &obja, NULL, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL );
    status = ZwCreateEvent(
        eventHandle,
        EVENT_ALL_ACCESS,
        &obja,
        eventType,
        FALSE);
    
    if (!NT_SUCCESS(status)) {
        return status;
    }
    
    status = ObReferenceObjectByHandle(
        *eventHandle,
        EVENT_ALL_ACCESS,
        NULL,
        KernelMode,
        event,
        NULL);
    
    if (!NT_SUCCESS(status)) {
        ZwClose(*eventHandle);
        return status;
    }
    return status;
}

BOOLEAN
CmpFileRead (
    PHHIVE      Hive,
    ULONG       FileType,
    PULONG      FileOffset,
    PVOID       DataBuffer,
    ULONG       DataLength
    )
/*++

Routine Description:

    This routine reads in a buffer from a file.

    It is environment specific.

    NOTE:   We assume the handle is opened for asynchronous access,
            and that we, and not the IO system, are keeping the
            offset pointer.

    NOTE:   Only 32bit offsets are supported, even though the underlying
            IO system on NT supports 64 bit offsets.

Arguments:

    Hive - Hive we are doing I/O for

    FileType - which supporting file to use

    FileOffset - pointer to variable providing 32bit offset on input,
                 and receiving new 32bit offset on output.

    DataBuffer - pointer to buffer

    DataLength - length of buffer

Return Value:

    FALSE if failure
    TRUE if success

--*/
{
    NTSTATUS status;
    LARGE_INTEGER   Offset;
    IO_STATUS_BLOCK IoStatus;
    PCMHIVE CmHive;
    HANDLE  FileHandle;
    ULONG LengthToRead;
    HANDLE eventHandle = NULL;
    PKEVENT eventObject = NULL;

    ASSERT(FIELD_OFFSET(CMHIVE, Hive) == 0);
    CmHive = (PCMHIVE)Hive;
    FileHandle = CmHive->FileHandles[FileType];
    if (FileHandle == NULL) {
        return TRUE;
    }

    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_IO,"CmpFileRead:\n"));
    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_IO,"\tHandle=%08lx  Offset=%08lx  ", FileHandle, *FileOffset));
    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_IO,"Buffer=%p  Length=%08lx\n", DataBuffer, DataLength));

    //
    // Detect attempt to read off end of 2gig file (this should be irrelevant)
    //
    if ((0xffffffff - *FileOffset) < DataLength) {
        CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"CmpFileRead: runoff\n"));
        return FALSE;
    }

    status = CmpCreateEvent(
        SynchronizationEvent,
        &eventHandle,
        &eventObject);
    if (!NT_SUCCESS(status))
        return FALSE;

    //
    // We'd really like to just call the filesystems and have them do
    // the right thing.  But the filesystem will attempt to lock our
    // entire buffer into memory, and that may fail for large requests.
    // So we split our reads into 64k chunks and call the filesystem for
    // each one.
    //
    ASSERT_PASSIVE_LEVEL();
    while (DataLength > 0) {

        //
        // Convert ULONG to Large
        //
        Offset.LowPart = *FileOffset;
        Offset.HighPart = 0L;

        //
        // trim request down if necessary.
        //
        if (DataLength > MAX_FILE_IO) {
            LengthToRead = MAX_FILE_IO;
        } else {
            LengthToRead = DataLength;
        }

        status = ZwReadFile(
                    FileHandle,
                    eventHandle,
                    NULL,               // apcroutine
                    NULL,               // apccontext
                    &IoStatus,
                    DataBuffer,
                    LengthToRead,
                    &Offset,
                    NULL                // key
                    );

        if (STATUS_PENDING == status) {
            status = KeWaitForSingleObject(eventObject, Executive,
                                           KernelMode, FALSE, NULL);
            ASSERT(STATUS_SUCCESS == status);
            status = IoStatus.Status;
        }

        //
        // adjust offsets
        //
        *FileOffset = Offset.LowPart + LengthToRead;
        DataLength -= LengthToRead;
        DataBuffer = (PVOID)((PCHAR)DataBuffer + LengthToRead);

        if (NT_SUCCESS(status)) {
            ASSERT(IoStatus.Status == status);
            if (IoStatus.Information != LengthToRead) {
                CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"CmpFileRead:\n\t"));
                CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"Failure1: status = %08lx  ", status));
                CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"IoInformation = %08lx\n", IoStatus.Information));
                ObDereferenceObject(eventObject);
                ZwClose(eventHandle);
                CmRegistryIODebug.Action = CmpIoFileRead;
                CmRegistryIODebug.Handle = FileHandle;
#if defined(_WIN64)
                CmRegistryIODebug.Status = (ULONG)IoStatus.Information - LengthToRead;
#else
                CmRegistryIODebug.Status = (ULONG)&IoStatus;
#endif
                return FALSE;
            }
        } else {
            //
            // set debugging info
            //
            CmRegistryIODebug.Action = CmpIoFileRead;
            CmRegistryIODebug.Handle = FileHandle;
            CmRegistryIODebug.Status = status;
#if DBG
            DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_TRACE_LEVEL,"CmpFileRead:\tFailure2: status = %08lx  IoStatus = %08lx\n", status, IoStatus.Status);
#endif

            ObDereferenceObject(eventObject);
            ZwClose(eventHandle);
            return FALSE;
        }

    }
    ObDereferenceObject(eventObject);
    ZwClose(eventHandle);
    return TRUE;
}

BOOLEAN
CmpFileWriteThroughCache(
    PHHIVE              Hive,
    ULONG               FileType,
    PCMP_OFFSET_ARRAY   offsetArray,
    ULONG               offsetArrayCount
    )
/*++

Routine Description:

    This is routine writes dirty ranges of data using Cc mapped views.
    The benefit is that writes don't go through Cc Lazy Writer, so there 
    is no danger to be throttled or deferred.

    It also flushes the cache for the written ranges, guaranteeing that 
    the data was commited to the disk upon return.

Arguments:

    Hive - Hive we are doing I/O for

    FileType - which supporting file to use

    offsetArray - array of structures where each structure holds a 32bit offset
                  into the Hive file and pointer the a buffer written to that
                  file offset.

    offsetArrayCount - number of elements in the offsetArray.

Return Value:

    FALSE if failure
    TRUE if success

Note:

    This routine is intended to deal only with paged bins (i.e. bins crossing the 
    CM_VIEW_SIZE boundary or bins that were added after the last sync)

Assumption:

    We work on the assumption that the data to be written at one iteration never spans 
    over the CM_VIEW_SIZE boundary. HvpFindNextDirtyBlock takes care of that !!! 
--*/
{
    ULONG           i;
    PVOID           DataBuffer;
    ULONG           DataLength;
    ULONG           FileOffset;
    PCMHIVE         CmHive;
    PVOID           Bcb;
    PVOID           FileBuffer;
    LARGE_INTEGER   Offset;
    IO_STATUS_BLOCK IoStatus;

    ASSERT_PASSIVE_LEVEL();

#if !DBG
    UNREFERENCED_PARAMETER (FileType);
#endif

    CmHive = (PCMHIVE)CONTAINING_RECORD(Hive, CMHIVE, Hive);

    ASSERT( ((FileType == HFILE_TYPE_EXTERNAL) && (CmHive->FileObject != NULL)) || HiveWritesThroughCache(Hive,FileType) );

    Offset.HighPart = 0;
    //
    // iterate through the array of data
    //
    for(i=0;i<offsetArrayCount;i++) {
        DataBuffer =  offsetArray[i].DataBuffer;
        DataLength =  offsetArray[i].DataLength;
        FileOffset = offsetArray[i].FileOffset;
        //
        // data should never span over CM_VIEW_SIZE boundary
        //
        ASSERT( (FileOffset & (~(CM_VIEW_SIZE - 1))) == ((FileOffset + DataLength - 1) & (~(CM_VIEW_SIZE - 1))) );

        //
        // unmap any possible mapped view that could overlap with this range ; not needed !!!!
        //

        //
        // map and pin data
        //
        Offset.LowPart = FileOffset;
        try {
            if( !CcPinRead (CmHive->FileObject,&Offset,DataLength,PIN_WAIT,&Bcb,&FileBuffer) ) {
                CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"CmpFileWriteThroughCache - could not pin read view i= %lu\n",i));
#if DBG
                DbgBreakPoint();
#endif //DBG
                return FALSE;        
            }
            //
            // copy data to pinned view; we need to do it inside try except, to protect against devices/volumes
            // dismounting from under us.
            //
            RtlCopyMemory(FileBuffer,DataBuffer, DataLength);

        } except (EXCEPTION_EXECUTE_HANDLER) {
            //
            // in low-memory scenarios, CcPinRead throws a STATUS_INSUFFICIENT_RESOURCES
            // We want to catch this and treat as a  "not enough resources" problem, 
            // rather than letting it to surface the kernel call
            //
            CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"CmpFileWriteThroughCache : CcPinRead has raised :%08lx\n",GetExceptionCode()));
            return FALSE;
        }

        //
        // dirty, unpin and flush
        //
        CcSetDirtyPinnedData (Bcb,NULL);
        CcUnpinData( Bcb );
        CcFlushCache (CmHive->FileObject->SectionObjectPointer,(PLARGE_INTEGER)(((ULONG_PTR)(&Offset)) + 1)/*we are private writers*/,DataLength,&IoStatus);
        if(!NT_SUCCESS(IoStatus.Status) ) {
            return FALSE;
        }
    }

    return TRUE;
}

BOOLEAN
CmpFileWrite(
    PHHIVE              Hive,
    ULONG               FileType,
    PCMP_OFFSET_ARRAY   offsetArray,
    ULONG               offsetArrayCount,
    PULONG              FileOffset
    )
/*++

Routine Description:

    This routine writes an array of buffers out to a file.

    It is environment specific.

    NOTE:   We assume the handle is opened for asynchronous access,
            and that we, and not the IO system, are keeping the
            offset pointer.

    NOTE:   Only 32bit offsets are supported, even though the underlying
            IO system on NT supports 64 bit offsets.

Arguments:

    Hive - Hive we are doing I/O for

    FileType - which supporting file to use

    offsetArray - array of structures where each structure holds a 32bit offset
                  into the Hive file and pointer the a buffer written to that
                  file offset.

    offsetArrayCount - number of elements in the offsetArray.

    FileOffset - returns the file offset after the last write to the file.

Return Value:

    FALSE if failure
    TRUE if success

--*/
{
    NTSTATUS        status;
    LARGE_INTEGER   Offset;
    PCMHIVE         CmHive;
    HANDLE          FileHandle;
    ULONG           LengthToWrite;
    LONG            WaitBufferCount = 0;
    LONG            idx;
    ULONG           arrayCount = 0;
    PVOID           DataBuffer = NULL;      // W4 only
    ULONG           DataLength;
    BOOLEAN         ret_val = TRUE;
    PCM_WRITE_BLOCK WriteBlock = NULL;

    if (CmpNoWrite) {
        return TRUE;
    }

    ASSERT(FIELD_OFFSET(CMHIVE, Hive) == 0);
    CmHive = (PCMHIVE)Hive;
    FileHandle = CmHive->FileHandles[FileType];
    if (FileHandle == NULL) {
        return TRUE;
    }

    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_IO,"CmpFileWrite:\n"));
    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_IO,"\tHandle=%08lx  ", FileHandle));

    //
    // decide whether we wait for IOs to complete or just issue them and
    // rely on the CcFlushCache to do the job
    //
    
    // Bring pages being written into memory first to allow disk to write
    // buffer contiguously.
    try {
        for (idx = 0; (ULONG) idx < offsetArrayCount; idx++) {
            char * start = offsetArray[idx].DataBuffer;
            char * end = (char *) start + offsetArray[idx].DataLength;
            while (start < end) {
                // perftouchbuffer globally declared so that compiler won't try
                // to remove it and this loop (if its smart enough?).
                perftouchbuffer += (ULONG) *start;
                start += PAGE_SIZE;
            }
        }
    } except (EXCEPTION_EXECUTE_HANDLER) {
        //
        // we might get STATUS_IN_PAGE_ERROR 
        //
        CmKdPrintEx((DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"CmpFileWrite has raised :%08lx\n",GetExceptionCode()));
        return FALSE;
    }

    WriteBlock = (PCM_WRITE_BLOCK)ExAllocatePool(NonPagedPool,sizeof(CM_WRITE_BLOCK));
    if( WriteBlock == NULL ) {
        return FALSE;
    }

    for (idx = 0; idx < MAXIMUM_WAIT_OBJECTS; idx++) {
        WriteBlock->EventHandles[idx] = NULL;
#if DBG
        WriteBlock->EventObjects[idx] = NULL;
#endif
    }
    //
    // We'd really like to just call the filesystems and have them do
    // the right thing.  But the filesystem will attempt to lock our
    // entire buffer into memory, and that may fail for large requests.
    // So we split our reads into 64k chunks and call the filesystem for
    // each one.
    //
    ASSERT_PASSIVE_LEVEL();
    arrayCount = 0;
    DataLength = 0;
    // This outer loop is hit more than once if the MAXIMUM_WAIT_OBJECTS limit
    // is hit before the offset array is drained.
    while (arrayCount < offsetArrayCount) {
        WaitBufferCount = 0;

        // This loop fills the wait buffer.
        while ((arrayCount < offsetArrayCount) &&
               (WaitBufferCount < MAXIMUM_WAIT_OBJECTS)) {

            // If data length isn't zero than the wait buffer filled before the
            // buffer in the last offsetArray element was sent to write file.
            if (DataLength == 0) {
                *FileOffset = offsetArray[arrayCount].FileOffset;
                DataBuffer =  offsetArray[arrayCount].DataBuffer;
                DataLength =  offsetArray[arrayCount].DataLength;
                //
                // Detect attempt to read off end of 2gig file
                // (this should be irrelevant)
                //
                if ((0xffffffff - *FileOffset) < DataLength) {
                    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"CmpFileWrite: runoff\n"));
                    status = STATUS_INVALID_PARAMETER_5;
                    goto Error_Exit;
                }
            }
            // else still more to write out of last buffer.

            while ((DataLength > 0) && (WaitBufferCount < MAXIMUM_WAIT_OBJECTS)) {

                //
                // Convert ULONG to Large
                //
                Offset.LowPart = *FileOffset;
                Offset.HighPart = 0L;

                //
                // trim request down if necessary.
                //
                if (DataLength > MAX_FILE_IO) {
                    LengthToWrite = MAX_FILE_IO;
                } else {
                    LengthToWrite = DataLength;
                }

                // Previously created events are reused.
                if (WriteBlock->EventHandles[WaitBufferCount] == NULL) {
                    status = CmpCreateEvent(SynchronizationEvent,
                                            &(WriteBlock->EventHandles[WaitBufferCount]),
                                            &(WriteBlock->EventObjects[WaitBufferCount]));
                    if (!NT_SUCCESS(status)) {
                        // Make sure we don't try to clean this up.
                        WriteBlock->EventHandles[WaitBufferCount] = NULL;
                        goto Error_Exit;
                    }
                    CmpSetHandleProtection(WriteBlock->EventHandles[WaitBufferCount],TRUE);
                }
                
                status = ZwWriteFile(FileHandle,
                                     WriteBlock->EventHandles[WaitBufferCount],
                                     NULL,               // apcroutine
                                     NULL,               // apccontext
                                     &(WriteBlock->IoStatus[WaitBufferCount]),
                                     DataBuffer,
                                     LengthToWrite,
                                     &Offset,
                                     NULL);
                        
                if (!NT_SUCCESS(status)) {
                    goto Error_Exit;
                } 

                WaitBufferCount++;
                
                //
                // adjust offsets
                //
                *FileOffset = Offset.LowPart + LengthToWrite;
                DataLength -= LengthToWrite;
                DataBuffer = (PVOID)((PCHAR)DataBuffer + LengthToWrite);
            } // while (DataLength > 0 && WaitBufferCount < MAXIMUM_WAIT_OBJECTS)
            
            arrayCount++;
            
        } // while (arrayCount < offsetArrayCount && 
          //        WaitBufferCount < MAXIMUM_WAIT_OBJECTS)

        status = KeWaitForMultipleObjects(WaitBufferCount, 
                                          WriteBlock->EventObjects,
                                          WaitAll,
                                          Executive,
                                          KernelMode, 
                                          FALSE, 
                                          NULL,
                                          WriteBlock->WaitBlockArray);
    
        if (!NT_SUCCESS(status))
            goto Error_Exit;
    
        for (idx = 0; idx < WaitBufferCount; idx++) {
            if (!NT_SUCCESS(WriteBlock->IoStatus[idx].Status)) {
                status = WriteBlock->IoStatus[idx].Status;
                ret_val = FALSE;
                goto Done;
            }
        }
        
        // There may still be more to do if the last element held a big buffer
        // and the wait buffer filled before it was all sent to the file.
        if (DataLength > 0) {
            arrayCount--;
        }

    } // while (arrayCount < offsetArrayCount)

    ret_val = TRUE;

    goto Done;
Error_Exit:
    //
    // set debugging info
    //
    CmRegistryIODebug.Action = CmpIoFileWrite;
    CmRegistryIODebug.Handle = FileHandle;
    CmRegistryIODebug.Status = status;
#if DBG
    DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_TRACE_LEVEL,"CmpFileWrite: error exiting %d\n", status);
#endif
    //
    // if WaitBufferCount > 0 then we have successfully issued
    // some I/Os, but not all of them. This is an error, but we
    // cannot return from this routine until all the successfully
    // issued I/Os have completed.
    //
    if (WaitBufferCount > 0) {
        //
        // only if we decided that we want to wait for the write to complete 
        // (log files and hives not using the mapped views technique)
        //
        status = KeWaitForMultipleObjects(WaitBufferCount, 
                                          WriteBlock->EventObjects,
                                          WaitAll,
                                          Executive,
                                          KernelMode, 
                                          FALSE, 
                                          NULL,
                                          WriteBlock->WaitBlockArray);
    }


    ret_val = FALSE;
Done:
    idx = 0;
    // Clean up open event handles and objects.
    while ((idx < MAXIMUM_WAIT_OBJECTS) && (WriteBlock->EventHandles[idx] != NULL)) {
        ASSERT( WriteBlock->EventObjects[idx] );
        ObDereferenceObject(WriteBlock->EventObjects[idx]);
        CmCloseHandle(WriteBlock->EventHandles[idx]);
        idx++;
    }

    if( WriteBlock != NULL ) {
        ExFreePool(WriteBlock);
    }

    return ret_val;
}

BOOLEAN
CmpFileFlush (
    PHHIVE          Hive,
    ULONG           FileType,
    PLARGE_INTEGER  FileOffset,
    ULONG           Length
    )
/*++

Routine Description:

    This routine performs a flush on a file handle.

Arguments:

    Hive - Hive we are doing I/O for

    FileType - which supporting file to use

    FileOffset - If this parameter is supplied (not NULL), then only the
                 byte range specified by FileOffset and Length are flushed.

    Length - Defines the length of the byte range to flush, starting at
             FileOffset.  This parameter is ignored if FileOffset is
             specified as NULL.
    

Return Value:

    FALSE if failure
    TRUE if success

Note: 
    
    FileOffset and Length are only taken into account when FileType == HFILE_TYPE_PRIMARY
    and the hive uses the mapped-views method.
--*/
{
    NTSTATUS        status;
    IO_STATUS_BLOCK IoStatus;
    PCMHIVE         CmHive;
    HANDLE          FileHandle;

    ASSERT(FIELD_OFFSET(CMHIVE, Hive) == 0);
    CmHive = (PCMHIVE)Hive;
    FileHandle = CmHive->FileHandles[FileType];
    if (FileHandle == NULL) {
        return TRUE;
    }

    if (CmpNoWrite) {
        return TRUE;
    }

    CmKdPrintEx((DPFLTR_CONFIG_ID,CML_IO,"CmpFileFlush:\n\tHandle = %08lx\n", FileHandle));

    ASSERT_PASSIVE_LEVEL();


    if( HiveWritesThroughCache(Hive,FileType) == TRUE ) {       
        //
        // OK, we need to flush using CcFlushCache
        //
        CcFlushCache (CmHive->FileObject->SectionObjectPointer,(PLARGE_INTEGER)((ULONG_PTR)FileOffset + 1)/*we are private writers*/,Length,&IoStatus);
        status = IoStatus.Status;
	    if( !NT_SUCCESS(status) ) {
			goto Error;
		}
    } 
    //
    // we have to do that regardless, to make sure the disk cache makes it to the disk.
    //
    status = ZwFlushBuffersFile(
                FileHandle,
                &IoStatus
                );

    if (NT_SUCCESS(status)) {
        ASSERT(IoStatus.Status == status);
        return TRUE;
    } else {
Error:
        //
        // set debugging info
        //
        CmRegistryIODebug.Action = CmpIoFileFlush;
        CmRegistryIODebug.Handle = FileHandle;
        CmRegistryIODebug.Status = status;

#if DBG
        DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_TRACE_LEVEL,"CmpFileFlush:\tFailure1: status = %08lx  IoStatus = %08lx\n",status,IoStatus.Status);
#endif
        return FALSE;
    }
}

#ifdef ALLOC_DATA_PRAGMA
#pragma data_seg()
#endif

