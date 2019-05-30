/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    ntrtlp.h

Abstract:

    Include file for NT runtime routines that are callable by both
    kernel mode code in the executive and user mode code in various
    NT subsystems, but which are private interfaces.

--*/

#ifndef _NTRTLP_
#define _NTRTLP_
#include <ntos.h>
#include <nturtl.h>
#include <zwapi.h>

#if defined(_AMD64_)
#include "amd64\ntrtlamd64.h"

#elif defined(_X86_)
#include "i386\ntrtl386.h"

#else
#error "no target architecture"
#endif

#include "string.h"
#include "wchar.h"

//
// This is the maximum MaximumLength for a UNICODE_STRING.
//
#define MAX_USTRING ( sizeof(WCHAR) * (MAXUSHORT/sizeof(WCHAR)) )

#define ASSERT_WELL_FORMED_UNICODE_STRING(Str) \
    ASSERT( (!(((Str)->Length&1) || ((Str)->MaximumLength&1) )) && ((Str)->Length <= (Str)->MaximumLength) )

#define ASSERT_WELL_FORMED_UNICODE_STRING_IN(Str) \
    ASSERT( !((Str)->Length&1) )

#define ASSERT_WELL_FORMED_UNICODE_STRING_OUT(Str) \
    ASSERT( (!((Str)->MaximumLength&1)) && ((Str)->Length <= (Str)->MaximumLength) )

//
//  Machine state reporting.  See machine specific includes for more.
//

#if defined(_WIN64)

extern PVOID RtlpFunctionAddressTable[];
extern UNWIND_HISTORY_TABLE RtlpUnwindHistoryTable;

#endif

VOID
RtlCaptureImageExceptionValues(
    IN  PVOID Base,
    OUT PVOID *FunctionTable,
    OUT PULONG TableSize
    );

LONG
LdrpCompareResourceNames(
    IN ULONG ResourceName,
    IN const IMAGE_RESOURCE_DIRECTORY* ResourceDirectory,
    IN const IMAGE_RESOURCE_DIRECTORY_ENTRY* ResourceDirectoryEntry
    );

NTSTATUS
LdrpSearchResourceSection(
    IN PVOID DllHandle,
    IN const ULONG_PTR* ResourceIdPath,
    IN ULONG ResourceIdPathLength,
    IN BOOLEAN FindDirectoryEntry,
    OUT PVOID *ResourceDirectoryOrData
    );
PVOID
LdrpGetAlternateResourceModuleHandle(
    IN PVOID Module,
    IN LANGID LangId
    );
LONG
LdrpCompareResourceNames_U(
    IN ULONG_PTR ResourceName,
    IN const IMAGE_RESOURCE_DIRECTORY* ResourceDirectory,
    IN const IMAGE_RESOURCE_DIRECTORY_ENTRY* ResourceDirectoryEntry
    );

NTSTATUS
LdrpSearchResourceSection_U(
    IN PVOID DllHandle,
    IN const ULONG_PTR* ResourceIdPath,
    IN ULONG ResourceIdPathLength,
    IN ULONG Flags,
    OUT PVOID *ResourceDirectoryOrData
    );

NTSTATUS
LdrpAccessResourceData(
    IN PVOID DllHandle,
    IN const IMAGE_RESOURCE_DATA_ENTRY* ResourceDataEntry,
    OUT PVOID *Address OPTIONAL,
    OUT PULONG Size OPTIONAL
    );

NTSTATUS
LdrpAccessResourceDataNoMultipleLanguage(
    IN PVOID DllHandle,
    IN const IMAGE_RESOURCE_DATA_ENTRY* ResourceDataEntry,
    OUT PVOID *Address OPTIONAL,
    OUT PULONG Size OPTIONAL
    );

VOID
RtlpAnsiPszToUnicodePsz(
    IN PCHAR AnsiString,
    IN WCHAR *UnicodeString,
    IN USHORT AnsiStringLength
    );

BOOLEAN
RtlpDidUnicodeToOemWork(
    IN PCOEM_STRING OemString,
    IN PCUNICODE_STRING UnicodeString
    );

extern CONST CCHAR RtlpBitsClearAnywhere[256];
extern CONST CCHAR RtlpBitsClearLow[256];
extern CONST CCHAR RtlpBitsClearHigh[256];
extern CONST CCHAR RtlpBitsClearTotal[256];

//
//  Macro that tells how many contiguous bits are set (i.e., 1) in
//  a byte
//

#define RtlpBitSetAnywhere( Byte ) RtlpBitsClearAnywhere[ (~(Byte) & 0xFF) ]


//
//  Macro that tells how many contiguous LOW order bits are set
//  (i.e., 1) in a byte
//

#define RtlpBitsSetLow( Byte ) RtlpBitsClearLow[ (~(Byte) & 0xFF) ]


//
//  Macro that tells how many contiguous HIGH order bits are set
//  (i.e., 1) in a byte
//

#define RtlpBitsSetHigh( Byte ) RtlpBitsClearHigh[ (~(Byte) & 0xFF) ]


//
//  Macro that tells how many set bits (i.e., 1) there are in a byte
//

#define RtlpBitsSetTotal( Byte ) RtlpBitsClearTotal[ (~(Byte) & 0xFF) ]



//
// Upcase data table
//

extern PUSHORT Nls844UnicodeUpcaseTable;
extern PUSHORT Nls844UnicodeLowercaseTable;


//
// Macros for Upper Casing a Unicode Code Point.
//

#define LOBYTE(w)           ((UCHAR)((w)))
#define HIBYTE(w)           ((UCHAR)(((USHORT)((w)) >> 8) & 0xFF))
#define GET8(w)             ((ULONG)(((w) >> 8) & 0xff))
#define GETHI4(w)           ((ULONG)(((w) >> 4) & 0xf))
#define GETLO4(w)           ((ULONG)((w) & 0xf))

/***************************************************************************\
* TRAVERSE844W
*
* Traverses the 8:4:4 translation table for the given wide character.  It
* returns the final value of the 8:4:4 table, which is a WORD in length.
*
*   Broken Down Version:
*   --------------------
*       Incr = pTable[GET8(wch)];
*       Incr = pTable[Incr + GETHI4(wch)];
*       Value = pTable[Incr + GETLO4(wch)];
*
* DEFINED AS A MACRO.
*
\***************************************************************************/

#define TRAVERSE844W(pTable, wch)                                               \
    ( (pTable)[(pTable)[(pTable)[GET8((wch))] + GETHI4((wch))] + GETLO4((wch))] )

//
// NLS_UPCASE - Based on macros in nls.h
//
// We will have this upcase macro quickly shortcircuit out if the value
// is within the normal ANSI range (i.e., < 127).  We actually won't bother
// with the 5 values above 'z' because they won't happen very often and
// coding it this way lets us get out after 1 compare for value less than
// 'a' and 2 compares for lowercase a-z.
//

#define NLS_UPCASE(wch) (                                                   \
    ((wch) < 'a' ?                                                          \
        (wch)                                                               \
    :                                                                       \
        ((wch) <= 'z' ?                                                     \
            (wch) - ('a'-'A')                                               \
        :                                                                   \
            ((WCHAR)((wch) + TRAVERSE844W(Nls844UnicodeUpcaseTable,(wch)))) \
        )                                                                   \
    )                                                                       \
)

#define NLS_DOWNCASE(wch) (                                                 \
    ((wch) < 'A' ?                                                          \
        (wch)                                                               \
    :                                                                       \
        ((wch) <= 'Z' ?                                                     \
            (wch) + ('a'-'A')                                               \
        :                                                                   \
            ((WCHAR)((wch) + TRAVERSE844W(Nls844UnicodeLowercaseTable,(wch)))) \
        )                                                                   \
    )                                                                       \
)

#if DBG
#define RTL_PAGED_CODE() PAGED_CODE()
#else
#define RTL_PAGED_CODE()
#endif


//
// The follow definition is used to support the Rtl compression engine
// Every compression format that NT supports will need to supply
// these set of routines in order to be called by NtRtl.
//

typedef NTSTATUS (*PRTL_COMPRESS_WORKSPACE_SIZE) (
    IN USHORT CompressionEngine,
    OUT PULONG CompressBufferWorkSpaceSize,
    OUT PULONG CompressFragmentWorkSpaceSize
    );

typedef NTSTATUS (*PRTL_COMPRESS_BUFFER) (
    IN USHORT CompressionEngine,
    IN PUCHAR UncompressedBuffer,
    IN ULONG UncompressedBufferSize,
    OUT PUCHAR CompressedBuffer,
    IN ULONG CompressedBufferSize,
    IN ULONG UncompressedChunkSize,
    OUT PULONG FinalCompressedSize,
    IN PVOID WorkSpace
    );

typedef NTSTATUS (*PRTL_DECOMPRESS_BUFFER) (
    OUT PUCHAR UncompressedBuffer,
    IN ULONG UncompressedBufferSize,
    IN PUCHAR CompressedBuffer,
    IN ULONG CompressedBufferSize,
    OUT PULONG FinalUncompressedSize
    );

typedef NTSTATUS (*PRTL_DECOMPRESS_FRAGMENT) (
    OUT PUCHAR UncompressedFragment,
    IN ULONG UncompressedFragmentSize,
    IN PUCHAR CompressedBuffer,
    IN ULONG CompressedBufferSize,
    IN ULONG FragmentOffset,
    OUT PULONG FinalUncompressedSize,
    IN PVOID WorkSpace
    );

typedef NTSTATUS (*PRTL_DESCRIBE_CHUNK) (
    IN OUT PUCHAR *CompressedBuffer,
    IN PUCHAR EndOfCompressedBufferPlus1,
    OUT PUCHAR *ChunkBuffer,
    OUT PULONG ChunkSize
    );

typedef NTSTATUS (*PRTL_RESERVE_CHUNK) (
    IN OUT PUCHAR *CompressedBuffer,
    IN PUCHAR EndOfCompressedBufferPlus1,
    OUT PUCHAR *ChunkBuffer,
    IN ULONG ChunkSize
    );

//
// Here is the declarations of the LZNT1 routines
//

NTSTATUS
RtlCompressWorkSpaceSizeLZNT1 (
    IN USHORT CompressionEngine,
    OUT PULONG CompressBufferWorkSpaceSize,
    OUT PULONG CompressFragmentWorkSpaceSize
    );

NTSTATUS
RtlCompressBufferLZNT1 (
    IN USHORT CompressionEngine,
    IN PUCHAR UncompressedBuffer,
    IN ULONG UncompressedBufferSize,
    OUT PUCHAR CompressedBuffer,
    IN ULONG CompressedBufferSize,
    IN ULONG UncompressedChunkSize,
    OUT PULONG FinalCompressedSize,
    IN PVOID WorkSpace
    );

NTSTATUS
RtlDecompressBufferLZNT1 (
    OUT PUCHAR UncompressedBuffer,
    IN ULONG UncompressedBufferSize,
    IN PUCHAR CompressedBuffer,
    IN ULONG CompressedBufferSize,
    OUT PULONG FinalUncompressedSize
    );

NTSTATUS
RtlDecompressFragmentLZNT1 (
    OUT PUCHAR UncompressedFragment,
    IN ULONG UncompressedFragmentSize,
    IN PUCHAR CompressedBuffer,
    IN ULONG CompressedBufferSize,
    IN ULONG FragmentOffset,
    OUT PULONG FinalUncompressedSize,
    IN PVOID WorkSpace
    );

NTSTATUS
RtlDescribeChunkLZNT1 (
    IN OUT PUCHAR *CompressedBuffer,
    IN PUCHAR EndOfCompressedBufferPlus1,
    OUT PUCHAR *ChunkBuffer,
    OUT PULONG ChunkSize
    );

NTSTATUS
RtlReserveChunkLZNT1 (
    IN OUT PUCHAR *CompressedBuffer,
    IN PUCHAR EndOfCompressedBufferPlus1,
    OUT PUCHAR *ChunkBuffer,
    IN ULONG ChunkSize
    );


NTSTATUS
RtlpSecMemFreeVirtualMemory(
    IN HANDLE ProcessHandle,
    IN OUT PVOID *BaseAddress,
    IN OUT PSIZE_T RegionSize,
    IN ULONG FreeType
     );

//
// Define procedure prototypes for architecture specific debug support routines.
//

NTSTATUS
DebugPrint(
    IN PSTRING Output,
    IN ULONG ComponentId,
    IN ULONG Level
    );

ULONG
DebugPrompt(
    IN PSTRING Output,
    IN PSTRING Input
    );

#endif  // _NTRTLP_

//
// Procedure prototype for exception logging routines.

ULONG
RtlpLogExceptionHandler(
    IN PEXCEPTION_RECORD ExceptionRecord,
    IN PCONTEXT ContextRecord,
    IN ULONG_PTR ControlPc,
    IN PVOID HandlerData,
    IN ULONG Size
    );

VOID
RtlpLogLastExceptionDisposition(
    IN ULONG LogIndex,
    IN EXCEPTION_DISPOSITION Disposition
    );

#define NUMBER_OF(x) (sizeof(x) / sizeof((x)[0]))

