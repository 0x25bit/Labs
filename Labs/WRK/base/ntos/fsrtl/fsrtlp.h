/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    FsRtlP.h

Abstract:

    This module defines private part of the File System Rtl component

--*/

#ifndef _FSRTLP_
#define _FSRTLP_

#pragma warning(disable:4214)   // bit field types other than int
#pragma warning(disable:4201)   // nameless struct/union
#pragma warning(disable:4127)   // condition expression is constant
#pragma warning(disable:4115)   // named type definition in parentheses
#pragma warning(disable:4706)   // assignment within conditional expression

#include <ntos.h>
#include <FsRtl.h>
#include <NtDdFt.h>
#include <zwapi.h>

#define FsRtlAllocatePool(PoolType, NumberOfBytes )                \
    ExAllocatePoolWithTag((POOL_TYPE)((PoolType) | POOL_RAISE_IF_ALLOCATION_FAILURE), \
                          NumberOfBytes,                                      \
                          'trSF')


#define FsRtlAllocatePoolWithQuota(PoolType, NumberOfBytes )           \
    ExAllocatePoolWithQuotaTag((POOL_TYPE)((PoolType) | POOL_RAISE_IF_ALLOCATION_FAILURE), \
                               NumberOfBytes,                                 \
                               'trSF')

#define FsRtlpAllocatePool(a,b)  FsRtlAllocatePoolWithTag((a),(b),MODULE_POOL_TAG)

//
//  The global FsRtl debug level variable, its values are:
//
//      0x00000000      Always gets printed (used when about to bugcheck)
//
//      0x00000001      Error conditions
//      0x00000002      Debug hooks
//      0x00000004
//      0x00000008
//
//      0x00000010
//      0x00000020
//      0x00000040
//      0x00000080
//
//      0x00000100
//      0x00000200
//      0x00000400
//      0x00000800
//
//      0x00001000
//      0x00002000
//      0x00004000
//      0x00008000
//
//      0x00010000
//      0x00020000
//      0x00040000
//      0x00080000
//
//      0x00100000
//      0x00200000
//      0x00400000
//      0x00800000
//
//      0x01000000
//      0x02000000
//      0x04000000      NotifyChange routines
//      0x08000000      Oplock routines
//
//      0x10000000      Name routines
//      0x20000000      FileLock routines
//      0x40000000      Vmcb routines
//      0x80000000      Mcb routines
//

//
//  Debug trace support
//

#ifdef FSRTLDBG

extern LONG FsRtlDebugTraceLevel;
extern LONG FsRtlDebugTraceIndent;

#define DebugTrace(INDENT,LEVEL,X,Y) {                        \
    LONG _i;                                                  \
    if (((LEVEL) == 0) || (FsRtlDebugTraceLevel & (LEVEL))) { \
        _i = (ULONG)PsGetCurrentThread();                     \
        DbgPrint("%08lx:",_i);                                 \
        if ((INDENT) < 0) {                                   \
            FsRtlDebugTraceIndent += (INDENT);                \
        }                                                     \
        if (FsRtlDebugTraceIndent < 0) {                      \
            FsRtlDebugTraceIndent = 0;                        \
        }                                                     \
        for (_i=0; _i<FsRtlDebugTraceIndent; _i+=1) {         \
            DbgPrint(" ");                                     \
        }                                                     \
        DbgPrint(X,Y);                                         \
        if ((INDENT) > 0) {                                   \
            FsRtlDebugTraceIndent += (INDENT);                \
        }                                                     \
    }                                                         \
}

#define DebugDump(STR,LEVEL,PTR) {                            \
    ULONG _i;                                                 \
    VOID FsRtlDump();                                         \
    if (((LEVEL) == 0) || (FsRtlDebugTraceLevel & (LEVEL))) { \
        _i = (ULONG)PsGetCurrentThread();                     \
        DbgPrint("%08lx:",_i);                                 \
        DbgPrint(STR);                                         \
        if (PTR != NULL) {FsRtlDump(PTR);}                    \
        DbgBreakPoint();                                      \
    }                                                         \
}

#else

#define DebugTrace(INDENT,LEVEL,X,Y)     {NOTHING;}

#define DebugDump(STR,LEVEL,PTR)         {NOTHING;}

#endif // FSRTLDBG


//
//  Miscellaneous support routines
//

VOID
FsRtlInitializeFileLocks (
    VOID
    );

VOID
FsRtlInitializeLargeMcbs (
    VOID
    );

VOID
FsRtlInitializeTunnels(
    VOID
    );

NTSTATUS
FsRtlInitializeWorkerThread (
    VOID
    );

//
//  This macro returns TRUE if a flag in a set of flags is on and FALSE
//  otherwise
//

#define FlagOn(Flags,SingleFlag)        ((Flags) & (SingleFlag))

#define BooleanFlagOn(Flags,SingleFlag) ((BOOLEAN)(((Flags) & (SingleFlag)) != 0))

#define SetFlag(F,SF) { \
    (F) |= (SF);        \
}

#define ClearFlag(F,SF) { \
    (F) &= ~(SF);         \
}

//
//  This macro takes a pointer (or ulong) and returns its rounded up word
//  value
//

#define WordAlign(Ptr) (                \
    ((((ULONG_PTR)(Ptr)) + 1) & -2) \
    )

//
//  This macro takes a pointer (or ulong) and returns its rounded up longword
//  value
//

#define LongAlign(Ptr) (                \
    ((((ULONG_PTR)(Ptr)) + 3) & -4) \
    )

//
//  This macro takes a pointer (or ulong) and returns its rounded up quadword
//  value
//

#define QuadAlign(Ptr) (                \
    ((((ULONG_PTR)(Ptr)) + 7) & -8) \
    )

//
//  This macro takes a ulong and returns its value rounded up to a sector
//  boundary
//

#define SectorAlign(Ptr) (                \
    ((((ULONG_PTR)(Ptr)) + 511) & -512) \
    )

//
//  This macro takes a number of bytes and returns the number of sectors
//  required to contain that many bytes, i.e., it sector aligns and divides
//  by the size of a sector.
//

#define SectorsFromBytes(bytes) ( \
    ((bytes) + 511) / 512         \
    )

//
//  This macro takes a number of sectors and returns the number of bytes
//  contained in that many sectors.
//

#define BytesFromSectors(sectors) ( \
    (sectors) * 512                 \
    )

//
//  The following types and macros are used to help unpack the packed and
//  misaligned fields found in the Bios parameter block
//

typedef union _UCHAR1 {
    UCHAR  Uchar[1];
    UCHAR  ForceAlignment;
} UCHAR1, *PUCHAR1;

typedef union _UCHAR2 {
    UCHAR  Uchar[2];
    USHORT ForceAlignment;
} UCHAR2, *PUCHAR2;

typedef union _UCHAR4 {
    UCHAR  Uchar[4];
    ULONG  ForceAlignment;
} UCHAR4, *PUCHAR4;

//
//  This macro copies an unaligned src byte to an aligned dst byte
//

#define CopyUchar1(Dst,Src) {                                \
    *((UCHAR1 *)(Dst)) = *((UNALIGNED UCHAR1 *)(Src)); \
    }

//
//  This macro copies an unaligned src word to an aligned dst word
//

#define CopyUchar2(Dst,Src) {                                \
    *((UCHAR2 *)(Dst)) = *((UNALIGNED UCHAR2 *)(Src)); \
    }

//
//  This macro copies an unaligned src longword to an aligned dsr longword
//

#define CopyUchar4(Dst,Src) {                                \
    *((UCHAR4 *)(Dst)) = *((UNALIGNED UCHAR4 *)(Src)); \
    }


//
//  The following macros are used to establish the semantics needed
//  to do a return from within a try-finally clause.  As a rule every
//  try clause must end with a label call try_exit.  For example,
//
//      try {
//              :
//              :
//
//      try_exit: NOTHING;
//      } finally {
//
//              :
//              :
//      }
//
//  Every return statement executed inside of a try clause should use the
//  try_return macro.  If the compiler fully supports the try-finally construct
//  then the macro should be
//
//      #define try_return(S)  { return(S); }
//
//  If the compiler does not support the try-finally construct then the macro
//  should be
//
//      #define try_return(S)  { S; goto try_exit; }
//

#define try_return(S) { S; goto try_exit; }

#define GET_FAST_IO_DISPATCH(DevObj) \
    ((DevObj)->DriverObject->FastIoDispatch)

#define GET_FS_FILTER_CALLBACKS(DevObj) \
    ((DevObj)->DriverObject->DriverExtension->FsFilterCallbacks)
    
//
//  Macro for validating the FastIo dispatch routines before calling
//  them in the FastIo pass through functions.
//

#define VALID_FAST_IO_DISPATCH_HANDLER(FastIoDispatchPtr, FieldName) \
    (((FastIoDispatchPtr) != NULL) && \
     (((FastIoDispatchPtr)->SizeOfFastIoDispatch) >= \
      (FIELD_OFFSET(FAST_IO_DISPATCH, FieldName) + sizeof(VOID *))) && \
     ((FastIoDispatchPtr)->FieldName != NULL))

#define VALID_FS_FILTER_CALLBACK_HANDLER(FsFilterCallbackPtr, FieldName) \
    (((FsFilterCallbackPtr) != NULL) && \
     (((FsFilterCallbackPtr)->SizeOfFsFilterCallbacks) >= \
      (FIELD_OFFSET(FS_FILTER_CALLBACKS, FieldName) + sizeof(VOID *))) && \
     ((FsFilterCallbackPtr)->FieldName != NULL))

#define FSRTL_FILTER_MEMORY_TAG    'gmSF'

typedef struct _FS_FILTER_COMPLETION_NODE {

    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT FileObject;
    PVOID CompletionContext;
    PFS_FILTER_COMPLETION_CALLBACK CompletionCallback;
    
} FS_FILTER_COMPLETION_NODE, *PFS_FILTER_COMPLETION_NODE;

#define FS_FILTER_DEFAULT_STACK_SIZE    15

typedef struct _FS_FILTER_COMPLETION_STACK {

    USHORT StackLength;
    USHORT NextStackPosition;
    PFS_FILTER_COMPLETION_NODE Stack;
    FS_FILTER_COMPLETION_NODE DefaultStack[FS_FILTER_DEFAULT_STACK_SIZE];
    
} FS_FILTER_COMPLETION_STACK, *PFS_FILTER_COMPLETION_STACK;

typedef struct _FS_FILTER_CTRL {

    FS_FILTER_CALLBACK_DATA Data;
    
    ULONG Flags;
    ULONG Reserved;
    
    FS_FILTER_COMPLETION_STACK CompletionStack;
    
} FS_FILTER_CTRL, *PFS_FILTER_CTRL;

//
//  Flag values for FS_FILTER_CTRL
//

#define FS_FILTER_ALLOCATED_COMPLETION_STACK    0x00000001
#define FS_FILTER_USED_RESERVE_POOL             0x00000002
#define FS_FILTER_CHANGED_DEVICE_STACKS         0x00000004

NTSTATUS
FsFilterInit(
    );

NTSTATUS
FsFilterAllocateCompletionStack (
    IN PFS_FILTER_CTRL FsFilterCtrl,
    IN BOOLEAN CanFail,
    OUT PULONG AllocationSize
    );

VOID
FsFilterFreeCompletionStack (
    IN PFS_FILTER_CTRL FsFilterCtrl
    );

NTSTATUS
FsFilterCtrlInit (
    IN OUT PFS_FILTER_CTRL FsFilterCtrl,
    IN UCHAR Operation,
    IN PDEVICE_OBJECT DeviceObject,
    IN PDEVICE_OBJECT BaseFsDeviceObject,
    IN PFILE_OBJECT FileObject,
    IN BOOLEAN CanFail
    );

VOID
FsFilterCtrlFree (
    IN PFS_FILTER_CTRL FsFilterCtrl
    );

#define PUSH_COMPLETION_NODE( completionStack ) \
    (((completionStack)->NextStackPosition < (completionStack)->StackLength ) ? \
        &(completionStack)->Stack[(completionStack)->NextStackPosition++] : \
        (ASSERT( FALSE ), NULL) )

#define POP_COMPLETION_NODE( completionStack ) \
    (ASSERT((completionStack)->NextStackPosition > 0), \
     ((completionStack)->NextStackPosition--))

#define GET_COMPLETION_NODE( completionStack ) \
    (ASSERT((completionStack)->NextStackPosition > 0),\
     (&(completionStack)->Stack[(completionStack)->NextStackPosition-1]))

#define FS_FILTER_HAVE_COMPLETIONS( fsFilterCtrl ) \
    ((fsFilterCtrl)->CompletionStack.NextStackPosition > 0)

VOID
FsFilterGetCallbacks (
    IN UCHAR Operation,
    IN PDEVICE_OBJECT DeviceObject,
    OUT PFS_FILTER_CALLBACK *PreOperationCallback,
    OUT PFS_FILTER_COMPLETION_CALLBACK *PostOperationCallback
    );

NTSTATUS
FsFilterPerformCallbacks (
    IN PFS_FILTER_CTRL FsFilterCtrl,
    IN BOOLEAN AllowFilterToFail,
    IN BOOLEAN AllowBaseFsToFail,
    OUT BOOLEAN *BaseFsFailedOp
    );

VOID
FsFilterPerformCompletionCallbacks(
    IN PFS_FILTER_CTRL FsFilterCtrl,
    IN NTSTATUS OperationStatus
    );

#endif // _FSRTLP_

