/*++ BUILD Version: 0001    // Increment this if a change has global effects

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    FsRtl.h

Abstract:

    This module defines all of the general File System Rtl routines

--*/

#ifndef _FSRTL_
#define _FSRTL_

// begin_ntifs
//
//  The following are globally used definitions for an LBN and a VBN
//

typedef ULONG LBN;
typedef LBN *PLBN;

typedef ULONG VBN;
typedef VBN *PVBN;


// end_ntifs
//
//  The following routine is called during phase 1 initialization to allow
//  us to create the pool of file system threads and the associated
//  synchronization resources.
//

NTKERNELAPI
BOOLEAN
FsRtlInitSystem (
    );

// begin_ntifs
//
//  Every file system that uses the cache manager must have FsContext
//  of the file object point to a common fcb header structure.
// end_ntifs
//  Either the normal or advanced FsRtl Header.
// begin_ntifs
//

typedef enum _FAST_IO_POSSIBLE {
    FastIoIsNotPossible = 0,
    FastIoIsPossible,
    FastIoIsQuestionable
} FAST_IO_POSSIBLE;

// end_ntifs
//  Changes to this structure will affect FSRTL_ADVANCED_FCB_HEADER.
// begin_ntifs

typedef struct _FSRTL_COMMON_FCB_HEADER {

    CSHORT NodeTypeCode;
    CSHORT NodeByteSize;

    //
    //  General flags available to FsRtl.
    //

    UCHAR Flags;

    //
    //  Indicates if fast I/O is possible or if we should be calling
    //  the check for fast I/O routine which is found via the driver
    //  object.
    //

    UCHAR IsFastIoPossible; // really type FAST_IO_POSSIBLE

    //
    //  Second Flags Field
    //

    UCHAR Flags2;

    //
    //  The following reserved field should always be 0
    //

    UCHAR Reserved;

    PERESOURCE Resource;

    PERESOURCE PagingIoResource;

    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER FileSize;
    LARGE_INTEGER ValidDataLength;

} FSRTL_COMMON_FCB_HEADER;
typedef FSRTL_COMMON_FCB_HEADER *PFSRTL_COMMON_FCB_HEADER;

//
//  This Fcb header is used for files which support caching
//  of compressed data, and related new support.
//
//  We start out by prefixing this structure with the normal
//  FsRtl header from above, which we have to do two different
//  ways for c++ or c.
//

#ifdef __cplusplus
typedef struct _FSRTL_ADVANCED_FCB_HEADER:FSRTL_COMMON_FCB_HEADER {
#else   // __cplusplus

typedef struct _FSRTL_ADVANCED_FCB_HEADER {

    //
    //  Put in the standard FsRtl header fields
    //

    FSRTL_COMMON_FCB_HEADER ;

#endif  // __cplusplus

    //
    //  The following two fields are supported only if
    //  Flags2 contains FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS
    //

    //
    //  This is a pointer to a Fast Mutex which may be used to
    //  properly synchronize access to the FsRtl header.  The
    //  Fast Mutex must be nonpaged.
    //

    PFAST_MUTEX FastMutex;

    //
    // This is a pointer to a list of context structures belonging to
    // filesystem filter drivers that are linked above the filesystem.
    // Each structure is headed by FSRTL_FILTER_CONTEXT.
    //

    LIST_ENTRY FilterContexts;

} FSRTL_ADVANCED_FCB_HEADER;
typedef FSRTL_ADVANCED_FCB_HEADER *PFSRTL_ADVANCED_FCB_HEADER;

//
//  Define FsRtl common header flags
//

#define FSRTL_FLAG_FILE_MODIFIED        (0x01)
#define FSRTL_FLAG_FILE_LENGTH_CHANGED  (0x02)
#define FSRTL_FLAG_LIMIT_MODIFIED_PAGES (0x04)

//
//  Following flags determine how the modified page writer should
//  acquire the file.  These flags can't change while either resource
//  is acquired.  If neither of these flags is set then the
//  modified/mapped page writer will attempt to acquire the paging io
//  resource shared.
//

#define FSRTL_FLAG_ACQUIRE_MAIN_RSRC_EX (0x08)
#define FSRTL_FLAG_ACQUIRE_MAIN_RSRC_SH (0x10)

//
//  This flag will be set by the Cache Manager if a view is mapped
//  to a file.
//

#define FSRTL_FLAG_USER_MAPPED_FILE     (0x20)

//  This flag indicates that the file system is using the 
//  FSRTL_ADVANCED_FCB_HEADER structure instead of the FSRTL_COMMON_FCB_HEADER
//  structure.
//

#define FSRTL_FLAG_ADVANCED_HEADER      (0x40)

//  This flag determines whether there currently is an Eof advance
//  in progress.  All such advances must be serialized.
//

#define FSRTL_FLAG_EOF_ADVANCE_ACTIVE   (0x80)

//
//  Flag values for Flags2
//
//  All unused bits are reserved and should NOT be modified.
//

//
//  If this flag is set, the Cache Manager will allow modified writing
//  in spite of the value of FsContext2.
//

#define FSRTL_FLAG2_DO_MODIFIED_WRITE        (0x01)

//
//  If this flag is set, the additional fields FilterContexts and FastMutex
//  are supported in FSRTL_COMMON_HEADER, and can be used to associate
//  context for filesystem filters with streams.
//

#define FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS  (0x02)

//
//  If this flag is set, the cache manager will flush and purge the cache map when
//  a user first maps a file
//

#define FSRTL_FLAG2_PURGE_WHEN_MAPPED (0x04)

//
//  The following constants are used to block top level Irp processing when
//  (in either the fast io or cc case) file system resources have been
//  acquired above the file system, or we are in an Fsp thread.
//

#define FSRTL_FSP_TOP_LEVEL_IRP         0x01
#define FSRTL_CACHE_TOP_LEVEL_IRP       0x02
#define FSRTL_MOD_WRITE_TOP_LEVEL_IRP   0x03
#define FSRTL_FAST_IO_TOP_LEVEL_IRP     0x04
#define FSRTL_MAX_TOP_LEVEL_IRP_FLAG    0xFFFF

//
//  The following structure is used to synchronize Eof extends.
//

typedef struct _EOF_WAIT_BLOCK {

    LIST_ENTRY EofWaitLinks;
    KEVENT Event;

} EOF_WAIT_BLOCK;

typedef EOF_WAIT_BLOCK *PEOF_WAIT_BLOCK;

// begin_ntosp
//
//  Normal uncompressed Copy and Mdl Apis
//

NTKERNELAPI
BOOLEAN
FsRtlCopyRead (
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in ULONG Length,
    __in BOOLEAN Wait,
    __in ULONG LockKey,
    __out_bcount(Length) PVOID Buffer,
    __out PIO_STATUS_BLOCK IoStatus,
    __in PDEVICE_OBJECT DeviceObject
    );

NTKERNELAPI
BOOLEAN
FsRtlCopyWrite (
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in ULONG Length,
    __in BOOLEAN Wait,
    __in ULONG LockKey,
    __in_bcount(Length) PVOID Buffer,
    __out PIO_STATUS_BLOCK IoStatus,
    __in PDEVICE_OBJECT DeviceObject
    );

// end_ntifs

NTKERNELAPI
BOOLEAN
FsRtlMdlRead (
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in ULONG Length,
    __in ULONG LockKey,
    __out PMDL *MdlChain,
    __out PIO_STATUS_BLOCK IoStatus
    );

NTKERNELAPI
BOOLEAN
FsRtlMdlReadComplete (
    __in PFILE_OBJECT FileObject,
    __in PMDL MdlChain
    );

// end_ntosp

NTKERNELAPI
BOOLEAN
FsRtlPrepareMdlWrite (
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in ULONG Length,
    __in ULONG LockKey,
    __out PMDL *MdlChain,
    __out PIO_STATUS_BLOCK IoStatus
    );

BOOLEAN
FsRtlMdlWriteComplete (
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in PMDL MdlChain
    );

// begin_ntifs

NTKERNELAPI
BOOLEAN
FsRtlMdlReadDev (
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in ULONG Length,
    __in ULONG LockKey,
    __out PMDL *MdlChain,
    __out PIO_STATUS_BLOCK IoStatus,
    __in PDEVICE_OBJECT DeviceObject
    );

NTKERNELAPI
BOOLEAN
FsRtlMdlReadCompleteDev (
    __in PFILE_OBJECT FileObject,
    __in PMDL MdlChain,
    __in PDEVICE_OBJECT DeviceObject
    );

NTKERNELAPI
BOOLEAN
FsRtlPrepareMdlWriteDev (
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in ULONG Length,
    __in ULONG LockKey,
    __out PMDL *MdlChain,
    __out PIO_STATUS_BLOCK IoStatus,
    __in PDEVICE_OBJECT DeviceObject
    );

NTKERNELAPI
BOOLEAN
FsRtlMdlWriteCompleteDev (
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in PMDL MdlChain,
    __in PDEVICE_OBJECT DeviceObject
    );

//
//  In Irps, compressed reads and writes are  designated by the
//  subfunction IRP_MN_COMPRESSED must be set and the Compressed
//  Data Info buffer must be described by the following structure
//  pointed to by Irp->Tail.Overlay.AuxiliaryBuffer.
//

typedef struct _FSRTL_AUXILIARY_BUFFER {

    //
    //  Buffer description with length.
    //

    PVOID Buffer;
    ULONG Length;

    //
    //  Flags
    //

    ULONG Flags;

    //
    //  Pointer to optional Mdl mapping buffer for file system use
    //

    PMDL Mdl;

} FSRTL_AUXILIARY_BUFFER;
typedef FSRTL_AUXILIARY_BUFFER *PFSRTL_AUXILIARY_BUFFER;

//
//  If this flag is set, the auxiliary buffer structure is
//  deallocated on Irp completion.  The caller has the
//  option in this case of appending this structure to the
//  structure being described, causing it all to be
//  deallocated at once.  If this flag is clear, no deallocate
//  occurs.
//

#define FSRTL_AUXILIARY_FLAG_DEALLOCATE 0x00000001

// end_ntifs
//
//  The following routines are intended to be called by Mm to avoid deadlocks.
//  They prerequire file system resources before acquire Mm resources.
//

//
//  This macro is called once when the ModifiedPageWriter is started.
//

#define FsRtlSetTopLevelIrpForModWriter() {             \
    PIRP tempIrp = (PIRP)FSRTL_MOD_WRITE_TOP_LEVEL_IRP; \
    IoSetTopLevelIrp(tempIrp);                          \
}

NTKERNELAPI
BOOLEAN
FsRtlAcquireFileForModWrite (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER EndingOffset,
    OUT PERESOURCE *ResourceToRelease
    );

NTKERNELAPI
NTSTATUS
FsRtlAcquireFileForModWriteEx (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER EndingOffset,
    OUT PERESOURCE *ResourceToRelease
    );

NTKERNELAPI
VOID
FsRtlReleaseFileForModWrite (
    IN PFILE_OBJECT FileObject,
    IN PERESOURCE ResourceToRelease
    );

NTKERNELAPI
VOID
FsRtlAcquireFileForCcFlush (
    IN PFILE_OBJECT FileObject
    );

NTKERNELAPI
NTSTATUS
FsRtlAcquireFileForCcFlushEx (
    IN PFILE_OBJECT FileObject
    );

NTKERNELAPI
VOID
FsRtlReleaseFileForCcFlush (
    IN PFILE_OBJECT FileObject
    );

NTKERNELAPI
NTSTATUS
FsRtlAcquireToCreateMappedSection (
    IN PFILE_OBJECT FileObject,
    IN ULONG SectionPageProtection
    );
    
NTKERNELAPI
NTSTATUS
FsRtlAcquireFileExclusiveCommon (
    IN PFILE_OBJECT FileObject,
    IN FS_FILTER_SECTION_SYNC_TYPE SyncType,
    IN ULONG SectionPageProtection
    );

// begin_ntifs
//
//  The following two routines are called from NtCreateSection to avoid
//  deadlocks with the file systems.
//

NTKERNELAPI
VOID
FsRtlAcquireFileExclusive (
    __in PFILE_OBJECT FileObject
    );

NTKERNELAPI
VOID
FsRtlReleaseFile (
    __in PFILE_OBJECT FileObject
    );

//
//  These routines provide a simple interface for the common operations
//  of query/set file size.
//

NTSTATUS
FsRtlGetFileSize(
    __in PFILE_OBJECT FileObject,
    __inout PLARGE_INTEGER FileSize
    );

// end_ntifs

NTSTATUS
FsRtlSetFileSize(
    IN PFILE_OBJECT FileObject,
    IN OUT PLARGE_INTEGER FileSize
    );

// begin_ntddk begin_ntifs
//
// Determine if there is a complete device failure on an error.
//

NTKERNELAPI
BOOLEAN
FsRtlIsTotalDeviceFailure(
    __in NTSTATUS Status
    );

// end_ntddk

//
//  Byte range file lock routines, implemented in FileLock.c
//
//  The file lock info record is used to return enumerated information
//  about a file lock
//

typedef struct _FILE_LOCK_INFO {

    //
    //  A description of the current locked range, and if the lock
    //  is exclusive or shared
    //

    LARGE_INTEGER StartingByte;
    LARGE_INTEGER Length;
    BOOLEAN ExclusiveLock;

    //
    //  The following fields describe the owner of the lock.
    //

    ULONG Key;
    PFILE_OBJECT FileObject;
    PVOID ProcessId;

    //
    //  The following field is used internally by FsRtl
    //

    LARGE_INTEGER EndingByte;

} FILE_LOCK_INFO;
typedef FILE_LOCK_INFO *PFILE_LOCK_INFO;

//
//  The following two procedure prototypes are used by the caller of the
//  file lock package to supply an alternate routine to call when
//  completing an IRP and when unlocking a byte range.  Note that the only
//  utility to us this interface is currently the redirector, all other file
//  system will probably let the IRP complete normally with IoCompleteRequest.
//  The user supplied routine returns any value other than success then the
//  lock package will remove any lock that we just inserted.
//

typedef NTSTATUS (*PCOMPLETE_LOCK_IRP_ROUTINE) (
    __in PVOID Context,
    __in PIRP Irp
    );

typedef VOID (*PUNLOCK_ROUTINE) (
    __in PVOID Context,
    __in PFILE_LOCK_INFO FileLockInfo
    );

//
//  A FILE_LOCK is an opaque structure but we need to declare the size of
//  it here so that users can allocate space for one.
//

typedef struct _FILE_LOCK {

    //
    //  The optional procedure to call to complete a request
    //

    PCOMPLETE_LOCK_IRP_ROUTINE CompleteLockIrpRoutine;

    //
    //  The optional procedure to call when unlocking a byte range
    //

    PUNLOCK_ROUTINE UnlockRoutine;

    //
    //  FastIoIsQuestionable is set to true whenever the filesystem require
    //  additional checking about whether the fast path can be taken.  As an
    //  example Ntfs requires checking for disk space before the writes can
    //  occur.
    //

    BOOLEAN FastIoIsQuestionable;
    BOOLEAN SpareC[3];

    //
    //  FsRtl lock information
    //

    PVOID   LockInformation;

    //
    //  Contains continuation information for FsRtlGetNextFileLock
    //

    FILE_LOCK_INFO  LastReturnedLockInfo;
    PVOID           LastReturnedLock;

} FILE_LOCK;
typedef FILE_LOCK *PFILE_LOCK;

NTKERNELAPI
PFILE_LOCK
FsRtlAllocateFileLock (
    __in_opt PCOMPLETE_LOCK_IRP_ROUTINE CompleteLockIrpRoutine,
    __in_opt PUNLOCK_ROUTINE UnlockRoutine
    );

NTKERNELAPI
VOID
FsRtlFreeFileLock (
    __in PFILE_LOCK FileLock
    );

NTKERNELAPI
VOID
FsRtlInitializeFileLock (
    __in PFILE_LOCK FileLock,
    __in_opt PCOMPLETE_LOCK_IRP_ROUTINE CompleteLockIrpRoutine,
    __in_opt PUNLOCK_ROUTINE UnlockRoutine
    );

NTKERNELAPI
VOID
FsRtlUninitializeFileLock (
    __in PFILE_LOCK FileLock
    );

NTKERNELAPI
NTSTATUS
FsRtlProcessFileLock (
    __in PFILE_LOCK FileLock,
    __in PIRP Irp,
    __in_opt PVOID Context
    );

NTKERNELAPI
BOOLEAN
FsRtlCheckLockForReadAccess (
    __in PFILE_LOCK FileLock,
    __in PIRP Irp
    );

NTKERNELAPI
BOOLEAN
FsRtlCheckLockForWriteAccess (
    __in PFILE_LOCK FileLock,
    __in PIRP Irp
    );

NTKERNELAPI
BOOLEAN
FsRtlFastCheckLockForRead (
    __in PFILE_LOCK FileLock,
    __in PLARGE_INTEGER StartingByte,
    __in PLARGE_INTEGER Length,
    __in ULONG Key,
    __in PFILE_OBJECT FileObject,
    __in PVOID ProcessId
    );

NTKERNELAPI
BOOLEAN
FsRtlFastCheckLockForWrite (
    __in PFILE_LOCK FileLock,
    __in PLARGE_INTEGER StartingByte,
    __in PLARGE_INTEGER Length,
    __in ULONG Key,
    __in PVOID FileObject,
    __in PVOID ProcessId
    );

NTKERNELAPI
PFILE_LOCK_INFO
FsRtlGetNextFileLock (
    __in PFILE_LOCK FileLock,
    __in BOOLEAN Restart
    );

NTKERNELAPI
NTSTATUS
FsRtlFastUnlockSingle (
    __in PFILE_LOCK FileLock,
    __in PFILE_OBJECT FileObject,
    __in LARGE_INTEGER UNALIGNED *FileOffset,
    __in PLARGE_INTEGER Length,
    __in PEPROCESS ProcessId,
    __in ULONG Key,
    __in_opt PVOID Context,
    __in BOOLEAN AlreadySynchronized
    );

NTKERNELAPI
NTSTATUS
FsRtlFastUnlockAll (
    __in PFILE_LOCK FileLock,
    __in PFILE_OBJECT FileObject,
    __in PEPROCESS ProcessId,
    __in_opt PVOID Context
    );

NTKERNELAPI
NTSTATUS
FsRtlFastUnlockAllByKey (
    __in PFILE_LOCK FileLock,
    __in PFILE_OBJECT FileObject,
    __in PEPROCESS ProcessId,
    __in ULONG Key,
    __in_opt PVOID Context
    );

NTKERNELAPI
BOOLEAN
FsRtlPrivateLock (
    __in PFILE_LOCK FileLock,
    __in PFILE_OBJECT FileObject,
    __in PLARGE_INTEGER FileOffset,
    __in PLARGE_INTEGER Length,
    __in PEPROCESS ProcessId,
    __in ULONG Key,
    __in BOOLEAN FailImmediately,
    __in BOOLEAN ExclusiveLock,
    __out PIO_STATUS_BLOCK Iosb,
    __in_opt PIRP Irp,
    __in_opt PVOID Context,
    __in BOOLEAN AlreadySynchronized
    );

//
//  BOOLEAN
//  FsRtlFastLock (
//      IN PFILE_LOCK FileLock,
//      IN PFILE_OBJECT FileObject,
//      IN PLARGE_INTEGER FileOffset,
//      IN PLARGE_INTEGER Length,
//      IN PEPROCESS ProcessId,
//      IN ULONG Key,
//      IN BOOLEAN FailImmediately,
//      IN BOOLEAN ExclusiveLock,
//      OUT PIO_STATUS_BLOCK Iosb,
//      IN PVOID Context OPTIONAL,
//      IN BOOLEAN AlreadySynchronized
//      );
//

#define FsRtlFastLock(A1,A2,A3,A4,A5,A6,A7,A8,A9,A10,A11) ( \
    FsRtlPrivateLock( A1,   /* FileLock            */       \
                      A2,   /* FileObject          */       \
                      A3,   /* FileOffset          */       \
                      A4,   /* Length              */       \
                      A5,   /* ProcessId           */       \
                      A6,   /* Key                 */       \
                      A7,   /* FailImmediately     */       \
                      A8,   /* ExclusiveLock       */       \
                      A9,   /* Iosb                */       \
                      NULL, /* Irp                 */       \
                      A10,  /* Context             */       \
                      A11   /* AlreadySynchronized */ )     \
)

//
//  BOOLEAN
//  FsRtlAreThereCurrentFileLocks (
//      IN PFILE_LOCK FileLock
//      );
//

#define FsRtlAreThereCurrentFileLocks(FL) ( \
    ((FL)->FastIoIsQuestionable))



//
//  Filesystem property tunneling, implemented in tunnel.c
//

//
//  Tunnel cache structure
//

typedef struct {

    //
    //  Mutex for cache manipulation
    //

    FAST_MUTEX          Mutex;

    //
    //  Splay Tree of tunneled information keyed by
    //  DirKey ## Name
    //

    PRTL_SPLAY_LINKS    Cache;

    //
    //  Timer queue used to age entries out of the main cache
    //

    LIST_ENTRY          TimerQueue;

    //
    //  Keep track of the number of entries in the cache to prevent
    //  excessive use of memory
    //

    USHORT              NumEntries;

} TUNNEL, *PTUNNEL;

NTKERNELAPI
VOID
FsRtlInitializeTunnelCache (
    __in TUNNEL *Cache
    );

NTKERNELAPI
VOID
FsRtlAddToTunnelCache (
    __in TUNNEL *Cache,
    __in ULONGLONG DirectoryKey,
    __in UNICODE_STRING *ShortName,
    __in UNICODE_STRING *LongName,
    __in BOOLEAN KeyByShortName,
    __in ULONG DataLength,
    __in_bcount(DataLength) VOID *Data
    );

NTKERNELAPI
BOOLEAN
FsRtlFindInTunnelCache (
    __in TUNNEL *Cache,
    __in ULONGLONG DirectoryKey,
    __in UNICODE_STRING *Name,
    __out UNICODE_STRING *ShortName,
    __out UNICODE_STRING *LongName,
    __inout ULONG  *DataLength,
    __out_bcount_part(*DataLength, *DataLength) VOID *Data
    );


NTKERNELAPI
VOID
FsRtlDeleteKeyFromTunnelCache (
    __in TUNNEL *Cache,
    __in ULONGLONG DirectoryKey
    );


NTKERNELAPI
VOID
FsRtlDeleteTunnelCache (
    __in TUNNEL *Cache
    );


//
//  Dbcs name support routines, implemented in DbcsName.c
//

//
//  The following enumerated type is used to denote the result of name
//  comparisons
//

typedef enum _FSRTL_COMPARISON_RESULT {
    LessThan = -1,
    EqualTo = 0,
    GreaterThan = 1
} FSRTL_COMPARISON_RESULT;

#ifdef NLS_MB_CODE_PAGE_TAG
#undef NLS_MB_CODE_PAGE_TAG
#endif // NLS_MB_CODE_PAGE_TAG

// end_ntifs
#if defined(_NTIFS_) || defined(_NTDRIVER_)
// begin_ntifs

#define LEGAL_ANSI_CHARACTER_ARRAY        (*FsRtlLegalAnsiCharacterArray) // ntosp
#define NLS_MB_CODE_PAGE_TAG              (*NlsMbOemCodePageTag)
#define NLS_OEM_LEAD_BYTE_INFO            (*NlsOemLeadByteInfo) // ntosp

// end_ntifs
#else

#define LEGAL_ANSI_CHARACTER_ARRAY        FsRtlLegalAnsiCharacterArray
#define NLS_MB_CODE_PAGE_TAG              NlsMbOemCodePageTag
#define NLS_OEM_LEAD_BYTE_INFO            NlsOemLeadByteInfo

#endif
// begin_ntifs begin_ntosp

extern UCHAR const* const LEGAL_ANSI_CHARACTER_ARRAY;
extern PUSHORT NLS_OEM_LEAD_BYTE_INFO;  // Lead byte info. for ACP

//
//  These following bit values are set in the FsRtlLegalDbcsCharacterArray
//

#define FSRTL_FAT_LEGAL         0x01
#define FSRTL_HPFS_LEGAL        0x02
#define FSRTL_NTFS_LEGAL        0x04
#define FSRTL_WILD_CHARACTER    0x08
#define FSRTL_OLE_LEGAL         0x10
#define FSRTL_NTFS_STREAM_LEGAL (FSRTL_NTFS_LEGAL | FSRTL_OLE_LEGAL)

//
//  The following macro is used to determine if an Ansi character is wild.
//

#define FsRtlIsAnsiCharacterWild(C) (                               \
    FsRtlTestAnsiCharacter((C), FALSE, FALSE, FSRTL_WILD_CHARACTER) \
)

//
//  The following macro is used to determine if an Ansi character is Fat legal.
//

#define FsRtlIsAnsiCharacterLegalFat(C,WILD_OK) (                 \
    FsRtlTestAnsiCharacter((C), TRUE, (WILD_OK), FSRTL_FAT_LEGAL) \
)

//
//  The following macro is used to determine if an Ansi character is Hpfs legal.
//

#define FsRtlIsAnsiCharacterLegalHpfs(C,WILD_OK) (                 \
    FsRtlTestAnsiCharacter((C), TRUE, (WILD_OK), FSRTL_HPFS_LEGAL) \
)

//
//  The following macro is used to determine if an Ansi character is Ntfs legal.
//

#define FsRtlIsAnsiCharacterLegalNtfs(C,WILD_OK) (                 \
    FsRtlTestAnsiCharacter((C), TRUE, (WILD_OK), FSRTL_NTFS_LEGAL) \
)

//
//  The following macro is used to determine if an Ansi character is
//  legal in an Ntfs stream name
//

#define FsRtlIsAnsiCharacterLegalNtfsStream(C,WILD_OK) (                    \
    FsRtlTestAnsiCharacter((C), TRUE, (WILD_OK), FSRTL_NTFS_STREAM_LEGAL)   \
)

//
//  The following macro is used to determine if an Ansi character is legal,
//  according to the caller's specification.
//

#define FsRtlIsAnsiCharacterLegal(C,FLAGS) (          \
    FsRtlTestAnsiCharacter((C), TRUE, FALSE, (FLAGS)) \
)

//
//  The following macro is used to test attributes of an Ansi character,
//  according to the caller's specified flags.
//

#define FsRtlTestAnsiCharacter(C, DEFAULT_RET, WILD_OK, FLAGS) (            \
        ((SCHAR)(C) < 0) ? DEFAULT_RET :                                    \
                           FlagOn( LEGAL_ANSI_CHARACTER_ARRAY[(C)],         \
                                   (FLAGS) |                                \
                                   ((WILD_OK) ? FSRTL_WILD_CHARACTER : 0) ) \
)


//
//  The following two macros use global data defined in ntos\rtl\nlsdata.c
//
//  BOOLEAN
//  FsRtlIsLeadDbcsCharacter (
//      IN UCHAR DbcsCharacter
//      );
//
//  /*++
//
//  Routine Description:
//
//      This routine takes the first bytes of a Dbcs character and
//      returns whether it is a lead byte in the system code page.
//
//  Arguments:
//
//      DbcsCharacter - Supplies the input character being examined
//
//  Return Value:
//
//      BOOLEAN - TRUE if the input character is a dbcs lead and
//              FALSE otherwise
//
//  --*/
//
//

#define FsRtlIsLeadDbcsCharacter(DBCS_CHAR) (                      \
    (BOOLEAN)((UCHAR)(DBCS_CHAR) < 0x80 ? FALSE :                  \
              (NLS_MB_CODE_PAGE_TAG &&                             \
               (NLS_OEM_LEAD_BYTE_INFO[(UCHAR)(DBCS_CHAR)] != 0))) \
)

NTKERNELAPI
VOID
FsRtlDissectDbcs (
    __in ANSI_STRING Path,
    __out PANSI_STRING FirstName,
    __out PANSI_STRING RemainingName
    );

NTKERNELAPI
BOOLEAN
FsRtlDoesDbcsContainWildCards (
    __in PANSI_STRING Name
    );

NTKERNELAPI
BOOLEAN
FsRtlIsDbcsInExpression (
    __in PANSI_STRING Expression,
    __in PANSI_STRING Name
    );

NTKERNELAPI
BOOLEAN
FsRtlIsFatDbcsLegal (
    __in ANSI_STRING DbcsName,
    __in BOOLEAN WildCardsPermissible,
    __in BOOLEAN PathNamePermissible,
    __in BOOLEAN LeadingBackslashPermissible
    );

// end_ntosp

NTKERNELAPI
BOOLEAN
FsRtlIsHpfsDbcsLegal (
    __in ANSI_STRING DbcsName,
    __in BOOLEAN WildCardsPermissible,
    __in BOOLEAN PathNamePermissible,
    __in BOOLEAN LeadingBackslashPermissible
    );


//
//  Exception filter routines, implemented in Filter.c
//

NTKERNELAPI
NTSTATUS
FsRtlNormalizeNtstatus (
    __in NTSTATUS Exception,
    __in NTSTATUS GenericException
    );

NTKERNELAPI
BOOLEAN
FsRtlIsNtstatusExpected (
    __in NTSTATUS Exception
    );

//
//  The following procedures are used to allocate executive pool and raise
//  insufficient resource status if pool isn't currently available.
//

#define FsRtlAllocatePoolWithTag(PoolType, NumberOfBytes, Tag)                \
    ExAllocatePoolWithTag((POOL_TYPE)((PoolType) | POOL_RAISE_IF_ALLOCATION_FAILURE), \
                          NumberOfBytes,                                      \
                          Tag)


#define FsRtlAllocatePoolWithQuotaTag(PoolType, NumberOfBytes, Tag)           \
    ExAllocatePoolWithQuotaTag((POOL_TYPE)((PoolType) | POOL_RAISE_IF_ALLOCATION_FAILURE), \
                               NumberOfBytes,                                 \
                               Tag)

//
//  The following function allocates a resource from the FsRtl pool.
//

NTKERNELAPI
PERESOURCE
FsRtlAllocateResource (
    );


//
//  Large Integer Mapped Control Blocks routines, implemented in LargeMcb.c
//
//  Originally this structure was truly opaque and code outside largemcb was
//  never allowed to examine or alter the structures.  However, for performance
//  reasons we want to allow ntfs the ability to quickly truncate down the
//  mcb without the overhead of an actual call to largemcb.c.  So to do that we
//  need to export the structure.  This structure is not exact.  The Mapping field
//  is declared here as a pvoid but largemcb.c it is a pointer to mapping pairs.
//

typedef struct _BASE_MCB {
    ULONG MaximumPairCount;
    ULONG PairCount;
    POOL_TYPE PoolType;
    PVOID Mapping;
} BASE_MCB;
typedef BASE_MCB *PBASE_MCB;

typedef struct _LARGE_MCB {
    PKGUARDED_MUTEX GuardedMutex;
    BASE_MCB BaseMcb;
} LARGE_MCB;
typedef LARGE_MCB *PLARGE_MCB;


NTKERNELAPI
VOID
FsRtlInitializeLargeMcb (
    __in PLARGE_MCB Mcb,
    __in POOL_TYPE PoolType
    );

NTKERNELAPI
VOID
FsRtlUninitializeLargeMcb (
    __in PLARGE_MCB Mcb
    );

NTKERNELAPI
VOID
FsRtlResetLargeMcb (
    __in PLARGE_MCB Mcb,
    __in BOOLEAN SelfSynchronized
    );

NTKERNELAPI
VOID
FsRtlTruncateLargeMcb (
    __in PLARGE_MCB Mcb,
    __in LONGLONG Vbn
    );

NTKERNELAPI
BOOLEAN
FsRtlAddLargeMcbEntry (
    __in PLARGE_MCB Mcb,
    __in LONGLONG Vbn,
    __in LONGLONG Lbn,
    __in LONGLONG SectorCount
    );

NTKERNELAPI
VOID
FsRtlRemoveLargeMcbEntry (
    __in PLARGE_MCB Mcb,
    __in LONGLONG Vbn,
    __in LONGLONG SectorCount
    );

NTKERNELAPI
BOOLEAN
FsRtlLookupLargeMcbEntry (
    __in PLARGE_MCB Mcb,
    __in LONGLONG Vbn,
    __out_opt PLONGLONG Lbn,
    __out_opt PLONGLONG SectorCountFromLbn,
    __out_opt PLONGLONG StartingLbn,
    __out_opt PLONGLONG SectorCountFromStartingLbn,
    __out_opt PULONG Index
    );

NTKERNELAPI
BOOLEAN
FsRtlLookupLastLargeMcbEntry (
    __in PLARGE_MCB Mcb,
    __out PLONGLONG Vbn,
    __out PLONGLONG Lbn
    );

NTKERNELAPI
BOOLEAN
FsRtlLookupLastLargeMcbEntryAndIndex (
    __in PLARGE_MCB OpaqueMcb,
    __out PLONGLONG LargeVbn,
    __out PLONGLONG LargeLbn,
    __out PULONG Index
    );

NTKERNELAPI
ULONG
FsRtlNumberOfRunsInLargeMcb (
    __in PLARGE_MCB Mcb
    );

NTKERNELAPI
BOOLEAN
FsRtlGetNextLargeMcbEntry (
    __in PLARGE_MCB Mcb,
    __in ULONG RunIndex,
    __out PLONGLONG Vbn,
    __out PLONGLONG Lbn,
    __out PLONGLONG SectorCount
    );

NTKERNELAPI
BOOLEAN
FsRtlSplitLargeMcb (
    __in PLARGE_MCB Mcb,
    __in LONGLONG Vbn,
    __in LONGLONG Amount
    );

//
//  Unsynchronized base mcb functions. There is one of these for every
//  large mcb equivalent function - they are identical other than lack of
//  synchronization 
//  

NTKERNELAPI
VOID
FsRtlInitializeBaseMcb (
    __in PBASE_MCB Mcb,
    __in POOL_TYPE PoolType
    );

NTKERNELAPI
VOID
FsRtlUninitializeBaseMcb (
    __in PBASE_MCB Mcb
    );

NTKERNELAPI
VOID
FsRtlResetBaseMcb (
    __in PBASE_MCB Mcb
    );

NTKERNELAPI
VOID
FsRtlTruncateBaseMcb (
    __in PBASE_MCB Mcb,
    __in LONGLONG Vbn
    );

NTKERNELAPI
BOOLEAN
FsRtlAddBaseMcbEntry (
    __in PBASE_MCB Mcb,
    __in LONGLONG Vbn,
    __in LONGLONG Lbn,
    __in LONGLONG SectorCount
    );

NTKERNELAPI
VOID
FsRtlRemoveBaseMcbEntry (
    __in PBASE_MCB Mcb,
    __in LONGLONG Vbn,
    __in LONGLONG SectorCount
    );

NTKERNELAPI
BOOLEAN
FsRtlLookupBaseMcbEntry (
    __in PBASE_MCB Mcb,
    __in LONGLONG Vbn,
    __out_opt PLONGLONG Lbn,
    __out_opt PLONGLONG SectorCountFromLbn,
    __out_opt PLONGLONG StartingLbn,
    __out_opt PLONGLONG SectorCountFromStartingLbn,
    __out_opt PULONG Index
    );

NTKERNELAPI
BOOLEAN
FsRtlLookupLastBaseMcbEntry (
    __in PBASE_MCB Mcb,
    __out PLONGLONG Vbn,
    __out PLONGLONG Lbn
    );

NTKERNELAPI
BOOLEAN
FsRtlLookupLastBaseMcbEntryAndIndex (
    __in PBASE_MCB OpaqueMcb,
    __out PLONGLONG LargeVbn,
    __out PLONGLONG LargeLbn,
    __out PULONG Index
    );

NTKERNELAPI
ULONG
FsRtlNumberOfRunsInBaseMcb (
    __in PBASE_MCB Mcb
    );

NTKERNELAPI
BOOLEAN
FsRtlGetNextBaseMcbEntry (
    __in PBASE_MCB Mcb,
    __in ULONG RunIndex,
    __out PLONGLONG Vbn,
    __out PLONGLONG Lbn,
    __out PLONGLONG SectorCount
    );

NTKERNELAPI
BOOLEAN
FsRtlSplitBaseMcb (
    __in PBASE_MCB Mcb,
    __in LONGLONG Vbn,
    __in LONGLONG Amount
    );


//
//  Mapped Control Blocks routines, implemented in Mcb.c
//
//  An MCB is an opaque structure but we need to declare the size of
//  it here so that users can allocate space for one.  Consequently the
//  size computation here must be updated by hand if the MCB changes.
//

typedef struct _MCB {
    LARGE_MCB DummyFieldThatSizesThisStructureCorrectly;
} MCB;
typedef MCB *PMCB;

NTKERNELAPI
VOID
FsRtlInitializeMcb (
    __in PMCB Mcb,
    __in POOL_TYPE PoolType
    );

NTKERNELAPI
VOID
FsRtlUninitializeMcb (
    __in PMCB Mcb
    );

NTKERNELAPI
VOID
FsRtlTruncateMcb (
    __in PMCB Mcb,
    __in VBN Vbn
    );

NTKERNELAPI
BOOLEAN
FsRtlAddMcbEntry (
    __in PMCB Mcb,
    __in VBN Vbn,
    __in LBN Lbn,
    __in ULONG SectorCount
    );

NTKERNELAPI
VOID
FsRtlRemoveMcbEntry (
    __in PMCB Mcb,
    __in VBN Vbn,
    __in ULONG SectorCount
    );

NTKERNELAPI
BOOLEAN
FsRtlLookupMcbEntry (
    __in PMCB Mcb,
    __in VBN Vbn,
    __out PLBN Lbn,
    __out_opt PULONG SectorCount,
    __out PULONG Index
    );

NTKERNELAPI
BOOLEAN
FsRtlLookupLastMcbEntry (
    __in PMCB Mcb,
    __out PVBN Vbn,
    __out PLBN Lbn
    );

NTKERNELAPI
ULONG
FsRtlNumberOfRunsInMcb (
    __in PMCB Mcb
    );

NTKERNELAPI
BOOLEAN
FsRtlGetNextMcbEntry (
    __in PMCB Mcb,
    __in ULONG RunIndex,
    __out PVBN Vbn,
    __out PLBN Lbn,
    __out PULONG SectorCount
    );

//
//  Fault Tolerance routines, implemented in FaultTol.c
//
//  The routines in this package implement routines that help file
//  systems interact with the FT device drivers.
//

NTKERNELAPI
NTSTATUS
FsRtlBalanceReads (
    __in PDEVICE_OBJECT TargetDevice
    );

// end_ntifs
NTKERNELAPI
NTSTATUS
FsRtlSyncVolumes (
    __in PDEVICE_OBJECT TargetDevice,
    __in_opt PLARGE_INTEGER ByteOffset,
    __in PLARGE_INTEGER ByteCount
    );

// begin_ntifs

//
//  Oplock routines, implemented in Oplock.c
//
//  An OPLOCK is an opaque structure, we declare it as a PVOID and
//  allocate the actual memory only when needed.
//

typedef PVOID OPLOCK, *POPLOCK;

typedef
VOID
(*POPLOCK_WAIT_COMPLETE_ROUTINE) (
    __in PVOID Context,
    __in PIRP Irp
    );

typedef
VOID
(*POPLOCK_FS_PREPOST_IRP) (
    __in PVOID Context,
    __in PIRP Irp
    );

NTKERNELAPI
VOID
FsRtlInitializeOplock (
    __inout POPLOCK Oplock
    );

NTKERNELAPI
VOID
FsRtlUninitializeOplock (
    __inout POPLOCK Oplock
    );

NTKERNELAPI
NTSTATUS
FsRtlOplockFsctrl (
    __in POPLOCK Oplock,
    __in PIRP Irp,
    __in ULONG OpenCount
    );

NTKERNELAPI
NTSTATUS
FsRtlCheckOplock (
    __in POPLOCK Oplock,
    __in PIRP Irp,
    __in_opt PVOID Context,
    __in_opt POPLOCK_WAIT_COMPLETE_ROUTINE CompletionRoutine,
    __in_opt POPLOCK_FS_PREPOST_IRP PostIrpRoutine
    );

NTKERNELAPI
BOOLEAN
FsRtlOplockIsFastIoPossible (
    __in POPLOCK Oplock
    );

NTKERNELAPI
BOOLEAN
FsRtlCurrentBatchOplock (
    __in POPLOCK Oplock
    );


//
//  Volume lock/unlock notification routines, implemented in PnP.c
//
//  These routines provide PnP volume lock notification support
//  for all filesystems.
//

#define FSRTL_VOLUME_DISMOUNT           1
#define FSRTL_VOLUME_DISMOUNT_FAILED    2
#define FSRTL_VOLUME_LOCK               3
#define FSRTL_VOLUME_LOCK_FAILED        4
#define FSRTL_VOLUME_UNLOCK             5
#define FSRTL_VOLUME_MOUNT              6

NTKERNELAPI
NTSTATUS
FsRtlNotifyVolumeEvent (
    __in PFILE_OBJECT FileObject,
    __in ULONG EventCode
    );

//
//  Notify Change routines, implemented in Notify.c
//
//  These routines provide Notify Change support for all filesystems.
//  Any of the 'Full' notify routines will support returning the
//  change information into the user's buffer.
//

typedef PVOID PNOTIFY_SYNC;

typedef
BOOLEAN (*PCHECK_FOR_TRAVERSE_ACCESS) (
    __in PVOID NotifyContext,
    __in_opt PVOID TargetContext,
    __in PSECURITY_SUBJECT_CONTEXT SubjectContext
    );

typedef
BOOLEAN (*PFILTER_REPORT_CHANGE) (
    __in PVOID NotifyContext,
    __in PVOID FilterContext
    );

NTKERNELAPI
VOID
FsRtlNotifyInitializeSync (
    __in PNOTIFY_SYNC *NotifySync
    );

NTKERNELAPI
VOID
FsRtlNotifyUninitializeSync (
    __in PNOTIFY_SYNC *NotifySync
    );

// end_ntifs
NTKERNELAPI
VOID
FsRtlNotifyChangeDirectory (
    __in PNOTIFY_SYNC NotifySync,
    __in PVOID FsContext,
    __in PSTRING FullDirectoryName,
    __in PLIST_ENTRY NotifyList,
    __in BOOLEAN WatchTree,
    __in ULONG CompletionFilter,
    __in PIRP NotifyIrp
    );

// begin_ntifs
NTKERNELAPI
VOID
FsRtlNotifyFullChangeDirectory (
    __in PNOTIFY_SYNC NotifySync,
    __in PLIST_ENTRY NotifyList,
    __in PVOID FsContext,
    __in PSTRING FullDirectoryName,
    __in BOOLEAN WatchTree,
    __in BOOLEAN IgnoreBuffer,
    __in ULONG CompletionFilter,
    __in PIRP NotifyIrp,
    __in_opt PCHECK_FOR_TRAVERSE_ACCESS TraverseCallback,
    __in_opt PSECURITY_SUBJECT_CONTEXT SubjectContext
    );

NTKERNELAPI
VOID
FsRtlNotifyFilterChangeDirectory (
    __in PNOTIFY_SYNC NotifySync,
    __in PLIST_ENTRY NotifyList,
    __in PVOID FsContext,
    __in PSTRING FullDirectoryName,
    __in BOOLEAN WatchTree,
    __in BOOLEAN IgnoreBuffer,
    __in ULONG CompletionFilter,
    __in PIRP NotifyIrp,
    __in_opt PCHECK_FOR_TRAVERSE_ACCESS TraverseCallback,
    __in_opt PSECURITY_SUBJECT_CONTEXT SubjectContext,
    __in_opt PFILTER_REPORT_CHANGE FilterCallback
    );

NTKERNELAPI
VOID
FsRtlNotifyFilterReportChange (
    __in PNOTIFY_SYNC NotifySync,
    __in PLIST_ENTRY NotifyList,
    __in PSTRING FullTargetName,
    __in USHORT TargetNameOffset,
    __in_opt PSTRING StreamName,
    __in_opt PSTRING NormalizedParentName,
    __in ULONG FilterMatch,
    __in ULONG Action,
    __in_opt PVOID TargetContext,
    __in_opt PVOID FilterContext
    );

// end_ntifs
NTKERNELAPI
VOID
FsRtlNotifyReportChange (
    __in PNOTIFY_SYNC NotifySync,
    __in PLIST_ENTRY NotifyList,
    __in PSTRING FullTargetName,
    __in PSTRING TargetName,
    __in ULONG FilterMatch
    );

// begin_ntifs
NTKERNELAPI
VOID
FsRtlNotifyFullReportChange (
    __in PNOTIFY_SYNC NotifySync,
    __in PLIST_ENTRY NotifyList,
    __in PSTRING FullTargetName,
    __in USHORT TargetNameOffset,
    __in_opt PSTRING StreamName,
    __in_opt PSTRING NormalizedParentName,
    __in ULONG FilterMatch,
    __in ULONG Action,
    __in PVOID TargetContext
    );

NTKERNELAPI
VOID
FsRtlNotifyCleanup (
    __in PNOTIFY_SYNC NotifySync,
    __in PLIST_ENTRY NotifyList,
    __in PVOID FsContext
    );


//
//  Unicode Name support routines, implemented in Name.c
//
//  The routines here are used to manipulate unicode names
//

//
//  The following macro is used to determine if a character is wild.
//

#define FsRtlIsUnicodeCharacterWild(C) (                                \
      (((C) >= 0x40) ? FALSE : FlagOn( LEGAL_ANSI_CHARACTER_ARRAY[(C)], \
                                       FSRTL_WILD_CHARACTER ) )         \
)

NTKERNELAPI
VOID
FsRtlDissectName (
    __in UNICODE_STRING Path,
    __out PUNICODE_STRING FirstName,
    __out PUNICODE_STRING RemainingName
    );

NTKERNELAPI
BOOLEAN
FsRtlDoesNameContainWildCards (
    __in PUNICODE_STRING Name
    );

NTKERNELAPI
BOOLEAN
FsRtlAreNamesEqual (
    __in PCUNICODE_STRING ConstantNameA,
    __in PCUNICODE_STRING ConstantNameB,
    __in BOOLEAN IgnoreCase,
    __in_opt PCWCH UpcaseTable
    );

NTKERNELAPI
BOOLEAN
FsRtlIsNameInExpression (
    __in PUNICODE_STRING Expression,
    __in PUNICODE_STRING Name,
    __in BOOLEAN IgnoreCase,
    __in_opt PWCH UpcaseTable
    );


//
//  Stack Overflow support routine, implemented in StackOvf.c
//

typedef
VOID
(*PFSRTL_STACK_OVERFLOW_ROUTINE) (
    __in PVOID Context,
    __in PKEVENT Event
    );

NTKERNELAPI
VOID
FsRtlPostStackOverflow (
    __in PVOID Context,
    __in PKEVENT Event,
    __in PFSRTL_STACK_OVERFLOW_ROUTINE StackOverflowRoutine
    );

NTKERNELAPI
VOID
FsRtlPostPagingFileStackOverflow (
    __in PVOID Context,
    __in PKEVENT Event,
    __in PFSRTL_STACK_OVERFLOW_ROUTINE StackOverflowRoutine
    );

//
// UNC Provider support
//

NTKERNELAPI
NTSTATUS
FsRtlRegisterUncProvider(
    __inout PHANDLE MupHandle,
    __in PUNICODE_STRING RedirectorDeviceName,
    __in BOOLEAN MailslotsSupported
    );

NTKERNELAPI
VOID
FsRtlDeregisterUncProvider(
    __in HANDLE Handle
    );
// end_ntifs

// begin_ntifs

//
//  File System Filter PerStream Context Support
//

//
//  Filesystem filter drivers use these APIs to associate context
//  with open streams (for filesystems that support this).
//

//
//  OwnerId should uniquely identify a particular filter driver
//  (e.g. the address of the driver's device object).
//  InstanceId can be used to distinguish distinct contexts associated
//  by a filter driver with a single stream (e.g. the address of the
//  PerStream Context structure).
//

//
//  This structure needs to be embedded within the users context that
//  they want to associate with a given stream
//

typedef struct _FSRTL_PER_STREAM_CONTEXT {
    //
    //  This is linked into the StreamContext list inside the 
    //  FSRTL_ADVANCED_FCB_HEADER structure.
    //

    LIST_ENTRY Links;

    //
    //  A Unique ID for this filter (ex: address of Driver Object, Device
    //  Object, or Device Extension)
    //

    PVOID OwnerId;

    //
    //  An optional ID to differentiate different contexts for the same
    //  filter.
    //

    PVOID InstanceId;

    //
    //  A callback routine which is called by the underlying file system
    //  when the stream is being torn down.  When this routine is called
    //  the given context has already been removed from the context linked
    //  list.  The callback routine cannot recursively call down into the
    //  filesystem or acquire any of their resources which they might hold
    //  when calling the filesystem outside of the callback.  This must
    //  be defined.
    //

    PFREE_FUNCTION FreeCallback;

} FSRTL_PER_STREAM_CONTEXT, *PFSRTL_PER_STREAM_CONTEXT;


//
//  This will initialize the given FSRTL_PER_STREAM_CONTEXT structure.  This
//  should be used before calling "FsRtlInsertPerStreamContext".
//

#define FsRtlInitPerStreamContext( _fc, _owner, _inst, _cb)   \
    ((_fc)->OwnerId = (_owner),                               \
     (_fc)->InstanceId = (_inst),                             \
     (_fc)->FreeCallback = (_cb))

//
//  Given a FileObject this will return the StreamContext pointer that
//  needs to be passed into the other FsRtl PerStream Context routines.
//

#define FsRtlGetPerStreamContextPointer(_fo) \
    ((PFSRTL_ADVANCED_FCB_HEADER)((_fo)->FsContext))

//
//  This will test to see if PerStream contexts are supported for the given
//  FileObject
//

#define FsRtlSupportsPerStreamContexts(_fo)                     \
    ((NULL != FsRtlGetPerStreamContextPointer(_fo)) &&          \
     FlagOn(FsRtlGetPerStreamContextPointer(_fo)->Flags2,       \
                    FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS))

//
//  Associate the context at Ptr with the given stream.  The Ptr structure
//  should be filled in by the caller before calling this routine (see
//  FsRtlInitPerStreamContext).  If the underlying filesystem does not support
//  filter contexts, STATUS_INVALID_DEVICE_REQUEST will be returned.
//

NTKERNELAPI
NTSTATUS
FsRtlInsertPerStreamContext (
    __in PFSRTL_ADVANCED_FCB_HEADER PerStreamContext,
    __in PFSRTL_PER_STREAM_CONTEXT Ptr
    );

//
//  Lookup a filter context associated with the stream specified.  The first
//  context matching OwnerId (and InstanceId, if present) is returned.  By not
//  specifying InstanceId, a filter driver can search for any context that it
//  has previously associated with a stream.  If no matching context is found,
//  NULL is returned.  If the file system does not support filter contexts,
//  NULL is returned.
//

NTKERNELAPI
PFSRTL_PER_STREAM_CONTEXT
FsRtlLookupPerStreamContextInternal (
    __in PFSRTL_ADVANCED_FCB_HEADER StreamContext,
    __in_opt PVOID OwnerId,
    __in_opt PVOID InstanceId
    );

#define FsRtlLookupPerStreamContext(_sc, _oid, _iid)                          \
 (((NULL != (_sc)) &&                                                         \
   FlagOn((_sc)->Flags2,FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS) &&              \
   !IsListEmpty(&(_sc)->FilterContexts)) ?                                    \
        FsRtlLookupPerStreamContextInternal((_sc), (_oid), (_iid)) :          \
        NULL)

//
//  Normally, contexts should be deleted when the file system notifies the
//  filter that the stream is being closed.  There are cases when a filter
//  may want to remove all existing contexts for a specific volume.  This
//  routine should be called at those times.  This routine should NOT be
//  called for the following cases:
//      - Inside your FreeCallback handler - The underlying file system has
//        already removed it from the linked list).
//      - Inside your IRP_CLOSE handler - If you do this then you will not
//        be notified when the stream is torn down.
//
//  This functions identically to FsRtlLookupPerStreamContext, except that the
//  returned context has been removed from the list.
//

NTKERNELAPI
PFSRTL_PER_STREAM_CONTEXT
FsRtlRemovePerStreamContext (
    __in PFSRTL_ADVANCED_FCB_HEADER StreamContext,
    __in_opt PVOID OwnerId,
    __in_opt PVOID InstanceId
    );


//
//  APIs for file systems to use for initializing and cleaning up
//  the Advaned FCB Header fields for PerStreamContext support
//

//
//  This will properly initialize the advanced header so that it can be
//  used with PerStream contexts.  
//  Note:  A fast mutex must be placed in an advanced header.  It is the
//         caller's responsibility to properly create and initialize this
//         mutex before calling this macro.  The mutex field is only set
//         if a non-NULL value is passed in.
//

#define FsRtlSetupAdvancedHeader( _advhdr, _fmutx )                         \
{                                                                           \
    SetFlag( (_advhdr)->Flags, FSRTL_FLAG_ADVANCED_HEADER );                \
    SetFlag( (_advhdr)->Flags2, FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS );     \
    InitializeListHead( &(_advhdr)->FilterContexts );                       \
    if ((_fmutx) != NULL) {                                                 \
        (_advhdr)->FastMutex = (_fmutx);                                    \
    }                                                                       \
}

//
// File systems call this API to free any filter contexts still associated
// with an FSRTL_COMMON_FCB_HEADER that they are tearing down.
// The FreeCallback routine for each filter context will be called.
//

NTKERNELAPI
VOID
FsRtlTeardownPerStreamContexts (
    __in PFSRTL_ADVANCED_FCB_HEADER AdvancedHeader
    );

// end_ntifs


//
//  File System Filter PerFileObject Context Support
//

//
//  Filesystem filter drivers use these APIs to associate context
//  with individual open files.  For now these are only supported on file
//  objects with a FileObject extension which are only created by using
//  IoCreateFileSpecifyDeviceObjectHint.
//

//
//  OwnerId should uniquely identify a particular filter driver
//  (e.g. the address of the driver's device object).
//  InstanceId can be used to distinguish distinct contexts associated
//  by a filter driver with a single stream (e.g. the address of the
//  fileobject).
//

//
//  This structure needs to be embedded within the users context that
//  they want to associate with a given stream
//

typedef struct _FSRTL_PER_FILEOBJECT_CONTEXT {
    //
    //  This is linked into the File Object
    //

    LIST_ENTRY Links;

    //
    //  A Unique ID for this filter (ex: address of Driver Object, Device
    //  Object, or Device Extension)
    //

    PVOID OwnerId;

    //
    //  An optional ID to differentiate different contexts for the same
    //  filter.
    //

    PVOID InstanceId;

} FSRTL_PER_FILEOBJECT_CONTEXT, *PFSRTL_PER_FILEOBJECT_CONTEXT;


//
//  This will initialize the given FSRTL_PER_FILEOBJECT_CONTEXT structure.  This
//  should be used before calling "FsRtlInsertPerFileObjectContext".
//

#define FsRtlInitPerFileObjectContext( _fc, _owner, _inst )         \
    ((_fc)->OwnerId = (_owner),                                     \
     (_fc)->InstanceId = (_inst))                                   \

//
//  This will test to see if PerFileObject contexts are supported for the given
//  FileObject
//

#define FsRtlSupportsPerFileObjectContexts(_fo) \
    FlagOn((_fo)->Flags,FO_FILE_OBJECT_HAS_EXTENSION)

//
//  Associate the context at Ptr with the given FileObject.  The Ptr
//  structure should be filled in by the caller before calling this
//  routine (see FsRtlInitPerFileObjectContext).  If this file object does not
//  support filter contexts, STATUS_INVALID_DEVICE_REQUEST will be returned.
//

NTKERNELAPI
NTSTATUS
FsRtlInsertPerFileObjectContext (
    __in PFILE_OBJECT FileObject,
    __in PFSRTL_PER_FILEOBJECT_CONTEXT Ptr
    );

//
//  Lookup a filter context associated with the FileObject specified.  The first
//  context matching OwnerId (and InstanceId, if present) is returned.  By not
//  specifying InstanceId, a filter driver can search for any context that it
//  has previously associated with a stream.  If no matching context is found,
//  NULL is returned.  If the FileObject does not support contexts, 
//  NULL is returned.
//

NTKERNELAPI
PFSRTL_PER_FILEOBJECT_CONTEXT
FsRtlLookupPerFileObjectContext (
    __in PFILE_OBJECT FileObject,
    __in_opt PVOID OwnerId,
    __in_opt PVOID InstanceId
    );

//
//  Normally, contexts should be deleted when the IoManager notifies the
//  filter that the FileObject is being freed.  There are cases when a filter
//  may want to remove all existing contexts for a specific volume.  This
//  routine should be called at those times.  This routine should NOT be
//  called for the following case:
//      - Inside your FreeCallback handler - The IoManager has already removed
//        it from the linked list.
//
//  This functions identically to FsRtlLookupPerFileObjectContext, except that
//  the returned context has been removed from the list.
//

NTKERNELAPI
PFSRTL_PER_FILEOBJECT_CONTEXT
FsRtlRemovePerFileObjectContext (
    __in PFILE_OBJECT FileObject,
    __in_opt PVOID OwnerId,
    __in_opt PVOID InstanceId
    );

//
//  Internal routine to free the context control structure
//

VOID
FsRtlPTeardownPerFileObjectContexts (
    __in PFILE_OBJECT FileObject
    );


// begin_ntifs
//++
//
//  VOID
//  FsRtlCompleteRequest (
//      IN PIRP Irp,
//      IN NTSTATUS Status
//      );
//
//  Routine Description:
//
//      This routine is used to complete an IRP with the indicated
//      status.  It does the necessary raise and lower of IRQL.
//
//  Arguments:
//
//      Irp - Supplies a pointer to the Irp to complete
//
//      Status - Supplies the completion status for the Irp
//
//  Return Value:
//
//      None.
//
//--

#define FsRtlCompleteRequest(IRP,STATUS) {         \
    (IRP)->IoStatus.Status = (STATUS);             \
    IoCompleteRequest( (IRP), IO_DISK_INCREMENT ); \
}


//++
//
//  VOID
//  FsRtlEnterFileSystem (
//      );
//
//  Routine Description:
//
//      This routine is used when entering a file system (e.g., through its
//      Fsd entry point).  It ensures that the file system cannot be suspended
//      while running and thus block other file I/O requests.  Upon exit
//      the file system must call FsRtlExitFileSystem.
//
//  Arguments:
//
//  Return Value:
//
//      None.
//
//--

#define FsRtlEnterFileSystem() { \
    KeEnterCriticalRegion();     \
}

//++
//
//  VOID
//  FsRtlExitFileSystem (
//      );
//
//  Routine Description:
//
//      This routine is used when exiting a file system (e.g., through its
//      Fsd entry point).
//
//  Arguments:
//
//  Return Value:
//
//      None.
//
//--

#define FsRtlExitFileSystem() { \
    KeLeaveCriticalRegion();    \
}

NTKERNELAPI
VOID
FsRtlIncrementCcFastReadNotPossible(
    VOID
    );

NTKERNELAPI
VOID
FsRtlIncrementCcFastReadWait(
    VOID
    );

NTKERNELAPI
VOID
FsRtlIncrementCcFastReadNoWait(
    VOID
    );

NTKERNELAPI
VOID
FsRtlIncrementCcFastReadResourceMiss(
    VOID
    );

//
//  Returns TRUE if the given fileObject represents a paging file, returns
//  FALSE otherwise.
//

NTKERNELAPI
LOGICAL
FsRtlIsPagingFile (
    __in PFILE_OBJECT FileObject
    );

NTKERNELAPI
NTSTATUS
FsRtlCreateSectionForDataScan(
    OUT PHANDLE SectionHandle,
    OUT PVOID *SectionObject,
    OUT PLARGE_INTEGER SectionFileSize OPTIONAL,
    IN PFILE_OBJECT FileObject,
    IN ACCESS_MASK DesiredAccess,
    IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
    IN PLARGE_INTEGER MaximumSize OPTIONAL,
    IN ULONG SectionPageProtection,
    IN ULONG AllocationAttributes,
    IN ULONG Flags
    );

// end_ntifs

//
//  Callback for mm to inform the filesystems that a file has been mapped
// 

NTSTATUS 
FsRtlMappedFile( 
    IN PFILE_OBJECT FileObject,
    IN ULONG DesiredAccess,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length
    );

#endif // _FSRTL_
