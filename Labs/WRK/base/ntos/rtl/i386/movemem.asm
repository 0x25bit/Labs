        title  "User Mode Zero and Move Memory functions"
;++
;
; Copyright (c) Microsoft Corporation. All rights reserved. 
;
; You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
; If you do not agree to the terms, do not use the code.
;
;
; Module Name:
;
;    movemem.asm
;
; Abstract:
;
;    This module implements functions to zero and copy blocks of memory
;
;--

.386p
        .xlist
include ks386.inc
include callconv.inc            ; calling convention macros
        .list

if DBG
_DATA   SEGMENT  DWORD PUBLIC 'DATA'

    public _RtlpZeroCount
    public _RtlpZeroBytes

_RtlpZeroCount dd 0
_RtlpZeroBytes dd 0

_MsgUnalignedPtr    db  'RTL: RtlCompare/FillMemoryUlong called with unaligned pointer (%x)\n',0
_MsgUnalignedCount  db  'RTL: RtlCompare/FillMemoryUlong called with unaligned count (%x)\n',0

_DATA ENDS

        extrn   _KdDebuggerEnabled:BYTE
        EXTRNP  _DbgBreakPoint,0
        extrn   _DbgPrint:near
endif

;
; Alignment parameters for zeroing and moving memory.
;

ZERO_MEMORY_ALIGNMENT = 4
ZERO_MEMORY_ALIGNMENT_LOG2 = 2
ZERO_MEMORY_ALIGNMENT_MASK = ZERO_MEMORY_ALIGNMENT - 1

MEMORY_ALIGNMENT = 4
MEMORY_ALIGNMENT_LOG2 = 2
MEMORY_ALIGNMENT_MASK = MEMORY_ALIGNMENT - 1


;
; Alignment for functions in this module
;

CODE_ALIGNMENT macro
    align   16
endm


_TEXT$00   SEGMENT PARA PUBLIC 'CODE'
        ASSUME  DS:FLAT, ES:FLAT, SS:NOTHING, FS:NOTHING, GS:NOTHING

        page , 132
        subttl "RtlCompareMemory"
;++
;
; ULONG
; RtlCompareMemory (
;    IN PVOID Source1,
;    IN PVOID Source2,
;    IN ULONG Length
;    )
;
; Routine Description:
;
;    This function compares two blocks of memory and returns the number
;    of bytes that compared equal.
;
; Arguments:
;
;    Source1 (esp+4) - Supplies a pointer to the first block of memory to
;       compare.
;
;    Source2 (esp+8) - Supplies a pointer to the second block of memory to
;       compare.
;
;    Length (esp+12) - Supplies the Length, in bytes, of the memory to be
;       compared.
;
; Return Value:
;
;    The number of bytes that compared equal is returned as the function
;    value. If all bytes compared equal, then the length of the original
;    block of memory is returned.
;
;--

RcmSource1      equ     [esp+12]
RcmSource2      equ     [esp+16]
RcmLength       equ     [esp+20]

CODE_ALIGNMENT
cPublicProc _RtlCompareMemory,3
cPublicFpo 3,0

        push    esi                     ; save registers
        push    edi                     ;
        cld                             ; clear direction
        mov     esi,RcmSource1          ; (esi) -> first block to compare
        mov     edi,RcmSource2          ; (edi) -> second block to compare

;
;   Compare dwords, if any.
;

rcm10:  mov     ecx,RcmLength           ; (ecx) = length in bytes
        shr     ecx,2                   ; (ecx) = length in dwords
        jz      rcm20                   ; no dwords, try bytes
        repe    cmpsd                   ; compare dwords
        jnz     rcm40                   ; mismatch, go find byte

;
;   Compare residual bytes, if any.
;

rcm20:  mov     ecx,RcmLength           ; (ecx) = length in bytes
        and     ecx,3                   ; (ecx) = length mod 4
        jz      rcm30                   ; 0 odd bytes, go do dwords
        repe    cmpsb                   ; compare odd bytes
        jnz     rcm50                   ; mismatch, go report how far we got

;
;   All bytes in the block match.
;

rcm30:  mov     eax,RcmLength           ; set number of matching bytes
        pop     edi                     ; restore registers
        pop     esi                     ;
        stdRET  _RtlCompareMemory

;
;   When we come to rcm40, esi (and edi) points to the dword after the
;   one which caused the mismatch.  Back up 1 dword and find the byte.
;   Since we know the dword didn't match, we can assume one byte won't.
;

rcm40:  sub     esi,4                   ; back up
        sub     edi,4                   ; back up
        mov     ecx,5                   ; ensure that ecx doesn't count out
        repe    cmpsb                   ; find mismatch byte

;
;   When we come to rcm50, esi points to the byte after the one that
;   did not match, which is TWO after the last byte that did match.
;

rcm50:  dec     esi                     ; back up
        sub     esi,RcmSource1          ; compute bytes that matched
        mov     eax,esi                 ;
        pop     edi                     ; restore registers
        pop     esi                     ;
        stdRET  _RtlCompareMemory

stdENDP _RtlCompareMemory


       subttl  "RtlCompareMemory"
EcmlSource       equ     [esp + 4 + 4]
EcmlLength       equ     [esp + 4 + 8]
EcmlPattern      equ     [esp + 4 + 12]

; end of arguments

CODE_ALIGNMENT
cPublicProc _RtlCompareMemoryUlong  ,3

;
; Save the non-volatile registers that we will use, without the benefit of
; a frame pointer.  No exception handling in this routine.
;

        push    edi

;
; Setup the registers for using REP STOS instruction to zero memory.
;
;   edi -> memory to zero
;   ecx = number of 32-bit words to zero
;   edx = number of extra 8-bit bytes to zero at the end (0 - 3)
;   eax = value to store in destination
;   direction flag is clear for auto-increment
;

        mov     edi,EcmlSource
if DBG
        test    edi,3
        jz      @F
        push    edi
        push    offset FLAT:_MsgUnalignedPtr
        call    _DbgPrint
        add     esp, 2 * 4
        cmp     _KdDebuggerEnabled,0
        je      @F
        call    _DbgBreakPoint@0
@@:
endif
        mov     ecx,EcmlLength
        mov     eax,EcmlPattern
        shr     ecx,ZERO_MEMORY_ALIGNMENT_LOG2


;
; If number of 32-bit words to compare is non-zero, then do it.
;

        repe    scasd
        je      @F
        sub     edi,4
@@:
        sub     edi,EcmlSource
        mov     eax,edi
        pop     edi
        stdRET    _RtlCompareMemoryUlong

stdENDP _RtlCompareMemoryUlong


       subttl  "RtlFillMemory"
;++
;
; VOID
; RtlFillMemory (
;    IN PVOID Destination,
;    IN ULONG Length,
;    IN UCHAR Fill
;    )
;
; Routine Description:
;
;    This function fills memory with a byte value.
;
; Arguments:
;
;    Destination - Supplies a pointer to the memory to zero.
;
;    Length - Supplies the Length, in bytes, of the memory to be zeroed.
;
;    Fill - Supplies the byte value to fill memory with.
;
; Return Value:
;
;    None.
;
;--

; definitions for arguments
; (TOS) = Return address

EfmDestination  equ     [esp + 4 + 4]
EfmLength       equ     [esp + 4 + 8]
EfmFill         equ     byte ptr [esp + 4 + 12]

; end of arguments

CODE_ALIGNMENT
cPublicProc _RtlFillMemory  ,3
cPublicFpo 3,1

;
; Save the non-volatile registers that we will use, without the benefit of
; a frame pointer.  No exception handling in this routine.
;

        push    edi

;
; Setup the registers for using REP STOS instruction to zero memory.
;
;   edi -> memory to zero
;   ecx = number of 32-bit words to zero
;   edx = number of extra 8-bit bytes to zero at the end (0 - 3)
;   eax = value to store in destination
;   direction flag is clear for auto-increment
;

        mov     edi,EfmDestination
        mov     ecx,EfmLength
        mov     al,EfmFill
        mov     ah,al
        shl     eax,16
        mov     al,EfmFill
        mov     ah,al
        cld

        mov     edx,ecx
        and     edx,ZERO_MEMORY_ALIGNMENT_MASK
        shr     ecx,ZERO_MEMORY_ALIGNMENT_LOG2


;
; If number of 32-bit words to zero is non-zero, then do it.
;

        rep     stosd

;
; If number of extra 8-bit bytes to zero is non-zero, then do it.  In either
; case restore non-volatile registers and return.
;

        or      ecx,edx
        jnz     @F
        pop     edi
        stdRET    _RtlFillMemory
@@:
        rep     stosb
        pop     edi
        stdRET    _RtlFillMemory

stdENDP _RtlFillMemory

       subttl  "RtlFillMemory"
;++
;
; VOID
; RtlFillMemoryUlonglong (
;    IN PVOID Destination,
;    IN ULONG Length,
;    IN ULONG Fill
;    )
;
; Routine Description:
;
;    This function fills memory with a 64-bit value.  The Destination pointer
;    must be aligned on an 8 byte boundary and the low order two bits of the
;    Length parameter are ignored.
;
; Arguments:
;
;    Destination - Supplies a pointer to the memory to zero.
;
;    Length - Supplies the Length, in bytes, of the memory to be zeroed.
;
;    Fill - Supplies the 64-bit value to fill memory with.
;
; Return Value:
;
;    None.
;
;--

; definitions for arguments
; (TOS) = Return address

EfmlDestination  equ     [esp + 0ch]
EfmlLength       equ     [esp + 10h]
EfmlFillLow      equ     [esp + 14h]
EfmlFillHigh     equ     [esp + 18h]

; end of arguments

CODE_ALIGNMENT
cPublicProc _RtlFillMemoryUlonglong  ,4
cPublicFpo 4,1

;
; Save the non-volatile registers that we will use, without the benefit of
; a frame pointer.  No exception handling in this routine.
;

        push    esi
        push    edi

;
; Setup the registers for using REP MOVSD instruction to zero memory.
;
;   edi -> memory to fill
;   esi -> first 8 byte chunk of the memory destination to fill
;   ecx = number of 32-bit words to zero
;   eax = value to store in destination
;   direction flag is clear for auto-increment
;

        mov     ecx,EfmlLength              ; # of bytes
        mov     esi,EfmlDestination         ; Destination pointer

if DBG
        test    ecx,7
        jz      @F
        push    ecx
        push    offset FLAT:_MsgUnalignedPtr
        call    _DbgPrint
        add     esp, 2 * 4
        mov     ecx,EfmlLength              ; # of bytes
        cmp     _KdDebuggerEnabled,0
        je      @F
        call    _DbgBreakPoint@0
@@:

        test    esi,3
        jz      @F
        push    esi
        push    offset FLAT:_MsgUnalignedPtr
        call    _DbgPrint
        add     esp, 2 * 4
        cmp     _KdDebuggerEnabled,0
        je      @F
        call    _DbgBreakPoint@0
@@:
endif
        mov     eax,EfmlFillLow             ; get low portion of the fill arg
        shr     ecx,ZERO_MEMORY_ALIGNMENT_LOG2      ; convert bytes to dwords
        sub     ecx,2                       ; doing the 1st one by hand
        mov     [esi],eax                   ; fill 1st highpart
        mov     eax,EfmlFillHigh            ; get high portion of the fill arg
        lea     edi,[esi+08]                ; initialize the dest pointer
        mov     [esi+04],eax                ; fill 1st lowpart

        rep     movsd                       ; ripple the rest

        pop     edi
        pop     esi
        stdRET    _RtlFillMemoryUlonglong

stdENDP _RtlFillMemoryUlonglong

       subttl  "RtlZeroMemory"
;++
;
; VOID
; RtlFillMemoryUlong (
;    IN PVOID Destination,
;    IN ULONG Length,
;    IN ULONG Fill
;    )
;
; Routine Description:
;
;    This function fills memory with a 32-bit value.  The Destination pointer
;    must be aligned on a 4 byte boundary and the low order two bits of the
;    Length parameter are ignored.
;
; Arguments:
;
;    Destination - Supplies a pointer to the memory to zero.
;
;    Length - Supplies the Length, in bytes, of the memory to be zeroed.
;
;    Fill - Supplies the 32-bit value to fill memory with.
;
; Return Value:
;
;    None.
;
;--

; definitions for arguments
; (TOS) = Return address

EfmlDestination  equ     [esp + 4 + 4]
EfmlLength       equ     [esp + 4 + 8]
EfmlFill         equ     [esp + 4 + 12]

; end of arguments

CODE_ALIGNMENT
cPublicProc _RtlFillMemoryUlong  ,3
cPublicFpo 3,1

;
; Save the non-volatile registers that we will use, without the benefit of
; a frame pointer.  No exception handling in this routine.
;

        push    edi

;
; Setup the registers for using REP STOS instruction to zero memory.
;
;   edi -> memory to zero
;   ecx = number of 32-bit words to zero
;   edx = number of extra 8-bit bytes to zero at the end (0 - 3)
;   eax = value to store in destination
;   direction flag is clear for auto-increment
;

        mov     edi,EfmlDestination
if DBG
        test    edi,3
        jz      @F
        push    edi
        push    offset FLAT:_MsgUnalignedPtr
        call    _DbgPrint
        add     esp, 2 * 4
        cmp     _KdDebuggerEnabled,0
        je      @F
        call    _DbgBreakPoint@0
@@:
endif
        mov     ecx,EfmlLength
        mov     eax,EfmlFill
        shr     ecx,ZERO_MEMORY_ALIGNMENT_LOG2


;
; If number of 32-bit words to zero is non-zero, then do it.
;

        rep     stosd

        pop     edi
        stdRET    _RtlFillMemoryUlong

stdENDP _RtlFillMemoryUlong

       subttl  "RtlZeroMemory"
;++
;
; VOID
; RtlZeroMemory (
;    IN PVOID Destination,
;    IN ULONG Length
;    )
;
; Routine Description:
;
;    This function zeros memory.
;
; Arguments:
;
;    Destination - Supplies a pointer to the memory to zero.
;
;    Length - Supplies the Length, in bytes, of the memory to be zeroed.
;
; Return Value:
;
;    None.
;
;--

; definitions for arguments
; (TOS) = Return address

EzmDestination  equ     [esp + 4 + 4]
EzmLength       equ     [esp + 4 + 8]

; end of arguments

CODE_ALIGNMENT
cPublicProc _RtlZeroMemory  ,2
cPublicFpo 2,1

;
; Save the non-volatile registers that we will use, without the benefit of
; a frame pointer.  No exception handling in this routine.
;

        push    edi

;
; Setup the registers for using REP STOS instruction to zero memory.
;
;   edi -> memory to zero
;   ecx = number of 32-bit words to zero
;   edx = number of extra 8-bit bytes to zero at the end (0 - 3)
;   eax = zero (value to store in destination)
;   direction flag is clear for auto-increment
;

        mov     edi,EzmDestination
        mov     ecx,EzmLength
        xor     eax,eax
        cld

        mov     edx,ecx
        and     edx,ZERO_MEMORY_ALIGNMENT_MASK
        shr     ecx,ZERO_MEMORY_ALIGNMENT_LOG2


;
; If number of 32-bit words to zero is non-zero, then do it.
;

        rep     stosd

;
; If number of extra 8-bit bytes to zero is non-zero, then do it.  In either
; case restore non-volatile registers and return.
;

        or      ecx,edx
        jnz     @F
        pop     edi
        stdRET    _RtlZeroMemory
@@:
        rep     stosb
        pop     edi
        stdRET    _RtlZeroMemory

stdENDP _RtlZeroMemory

        page , 132
        subttl  "RtlMoveMemory"
;++
;
; VOID
; RtlMoveMemory (
;    IN PVOID Destination,
;    IN PVOID Source OPTIONAL,
;    IN ULONG Length
;    )
;
; Routine Description:
;
;    This function moves memory either forward or backward, aligned or
;    unaligned, in 4-byte blocks, followed by any remaining bytes.
;
; Arguments:
;
;    Destination - Supplies a pointer to the destination of the move.
;
;    Source - Supplies a pointer to the memory to move.
;
;    Length - Supplies the Length, in bytes, of the memory to be moved.
;
; Return Value:
;
;    None.
;
;--

; Definitions of arguments
; (TOS) = Return address

EmmDestination  equ     [esp + 8 + 4]
EmmSource       equ     [esp + 8 + 8]
EmmLength       equ     [esp + 8 + 12]

; End of arguments

CODE_ALIGNMENT
cPublicProc _RtlMoveMemory  ,3
cPublicFpo 3,2

;
; Save the non-volatile registers that we will use, without the benefit of
; a frame pointer.  No exception handling in this routine.
;

        push    esi
        push    edi

;
; Setup the registers for using REP MOVS instruction to move memory.
;
;   esi -> memory to move (NULL implies the destination will be zeroed)
;   edi -> destination of move
;   ecx = number of 32-bit words to move
;   edx = number of extra 8-bit bytes to move at the end (0 - 3)
;   direction flag is clear for auto-increment
;

        mov     esi,EmmSource
        mov     edi,EmmDestination
        mov     ecx,EmmLength
if DBG
        inc     _RtlpZeroCount
        add     _RtlpZeroBytes,ecx
endif
        cld

        cmp     esi,edi                 ; Special case if Source > Destination
        jbe     overlap

nooverlap:
        mov     edx,ecx
        and     edx,MEMORY_ALIGNMENT_MASK
        shr     ecx,MEMORY_ALIGNMENT_LOG2

;
; If number of 32-bit words to move is non-zero, then do it.
;

        rep     movsd

;
; If number of extra 8-bit bytes to move is non-zero, then do it.  In either
; case restore non-volatile registers and return.
;

        or      ecx,edx
        jnz     @F
        pop     edi
        pop     esi
        stdRET    _RtlMoveMemory
@@:
        rep     movsb

movedone:
        pop     edi
        pop     esi
        stdRET    _RtlMoveMemory

;
; Here to handle special case when Source > Destination and therefore is a
; potential overlapping move.  If Source == Destination, then nothing to do.
; Otherwise, increment the Source and Destination pointers by Length and do
; the move backwards, a byte at a time.
;

overlap:
        je      movedone
        mov     eax,edi
        sub     eax,esi
        cmp     ecx,eax
        jbe     nooverlap

        std
        add     esi,ecx
        add     edi,ecx
        dec     esi
        dec     edi
        rep     movsb
        cld
        jmp     short movedone

stdENDP _RtlMoveMemory

       subttl  "RtlCopyMemoryNonTemporal"
;
; Register Definitions (for instruction macros).
;

rEAX            equ     0
rECX            equ     1
rEDX            equ     2
rEBX            equ     3
rESP            equ     4
rEBP            equ     5
rESI            equ     6
rEDI            equ     7

MEMORY_ALIGNMENT_MASK0  = 63
MEMORY_ALIGNMENT_LOG2_0 = 6

MEMORY_ALIGNMENT_MASK1  = 3
MEMORY_ALIGNMENT_LOG2_1 = 2

sfence            macro
                   db      0FH, 0AEH, 0F8H
                  endm

prefetchnta_short macro   GeneralReg, Offset
                   db      0FH, 018H,  040H + GeneralReg, Offset
                  endm

prefetchnta_long  macro   GeneralReg, Offset
                   db      0FH, 018H,  080h + GeneralReg
                   dd      Offset
                  endm

movnti_eax        macro   GeneralReg, Offset
                   db  0FH, 0C3H, 040H + GeneralReg, Offset
                  endm

movnti_eax_0_disp macro   GeneralReg
                   db  0FH, 0C3H, 000H + GeneralReg
                  endm

movnti_ebx        macro   GeneralReg, Offset
                   db  0FH, 0C3H, 058H + GeneralReg, Offset
                  endm

;
;
; Macro that moves 64bytes (1 cache line using movnti (eax and ebx registers)
;
;

movnticopy64bytes  macro
                    mov    eax, [esi]
                    mov    ebx, [esi + 4]
                    movnti_eax_0_disp rEDI
                    movnti_ebx rEDI, 4

                    mov    eax, [esi + 8]
                    mov    ebx, [esi + 12]
                    movnti_eax rEDI, 8
                    movnti_ebx rEDI, 12

                    mov    eax, [esi + 16]
                    mov    ebx, [esi + 20]
                    movnti_eax rEDI, 16
                    movnti_ebx rEDI, 20

                    mov    eax, [esi + 24]
                    mov    ebx, [esi + 28]
                    movnti_eax rEDI, 24
                    movnti_ebx rEDI, 28

                    mov    eax, [esi + 32]
                    mov    ebx, [esi + 36]
                    movnti_eax rEDI,32
                    movnti_ebx rEDI, 36

                    mov    eax, [esi + 40]
                    mov    ebx, [esi + 44]
                    movnti_eax rEDI, 40
                    movnti_ebx rEDI,  44

                    mov    eax, [esi + 48]
                    mov    ebx, [esi + 52]
                    movnti_eax rEDI,48
                    movnti_ebx rEDI, 52

                    mov    eax, [esi + 56]
                    mov    ebx, [esi + 60]
                    movnti_eax rEDI, 56
                    movnti_ebx rEDI, 60
                  endm


;++
;
; VOID
; RtlCopyMemoryNonTemporal(
;    IN PVOID Destination,
;    IN PVOID Source ,
;    IN ULONG Length
;    )
;
; Routine Description:
;
;   This function copies nonoverlapping from one buffer to another
;   using nontemporal moves that do not pollute the cache.
;
; Arguments:
;
;    Destination - Supplies a pointer to the destination of the move.
;
;    Source - Supplies a pointer to the memory to move.
;
;    Length - Supplies the Length, in bytes, of the memory to be moved.
;
; Return Value:
;
;    None.
;
;--

cPublicProc _RtlCopyMemoryNonTemporal ,3  

; Definitions of arguments
; (TOS) = Return address

CPNDestination  equ     [ebp + 4 + 4]
CPNSource       equ     [ebp + 4 + 8]
CPNLength       equ     [ebp + 4 + 12]

        push    ebp
        mov     ebp, esp
        push    esi
        push    edi
        push    ebx
       
        mov     esi, CPNSource
        mov     edi, CPNDestination
        mov     ecx, CPNLength


;
; Before prefetching we must guarantee the TLB is valid.
;
        mov     eax, [esi]

        cld

;
;Check if less than 64 bytes 
;
 
        mov     edx, ecx
        and     ecx, MEMORY_ALIGNMENT_MASK0
        shr     edx, MEMORY_ALIGNMENT_LOG2_0
        je      Copy4
        dec     edx
        je      copy64

        prefetchnta_short rESI, 128
        dec     edx
        je      copy128

        prefetchnta_short rESI, 192
        dec     edx
        je      copy192


         
copyLoop:

        prefetchnta_long rESI, 256

        movnticopy64bytes
        lea     esi, [esi + 64]
        lea     edi, [edi + 64]
        
        dec     edx
        jnz     copyLoop


copy192:


        movnticopy64bytes
        lea     esi, [esi + 64]
        lea     edi, [edi + 64]
       
copy128:


        movnticopy64bytes
        lea     esi, [esi + 64]
        lea     edi, [edi + 64]

copy64:

        movnticopy64bytes

        or     ecx, ecx  ; anything less than 64 to do?
        jz     ExitRoutine

        prefetchnta_short rESI, 0
;
;Update pointer for last copy    
;
        
        lea     esi, [esi + 64]
        lea     edi, [edi + 64]

;
;Handle extra bytes here in 32 bit chuncks and then 8-bit bytes    
;

Copy4:
         mov    edx, ecx
         and    ecx, MEMORY_ALIGNMENT_MASK1
         shr    edx, MEMORY_ALIGNMENT_LOG2_1

;
; If the number of 32-bit words to move is non-zero, then do it
;         
         jz     RemainingBytes 

Copy4Loop:
         mov    eax, [esi]
         movnti_eax_0_disp rEDI
         lea    esi, [esi+4]
         lea    edi, [edi+4]
         dec    edx
         jnz    Copy4Loop
         
RemainingBytes:
         or     ecx, ecx
         jz     ExitRoutine
         rep     movsb

ExitRoutine:     

        sfence            ;Make all stores globally visible 
        pop     ebx
        pop     edi
        pop     esi
        pop     ebp
        stdRET  _RtlCopyMemoryNonTemporal 

stdENDP _RtlCopyMemoryNonTemporal

       subttl  "RtlPrefetchMemoryNonTemporal"

;++
;
; VOID
; FASTCALL
; RtlPrefetchMemoryNonTemporal(
;    IN PVOID Source,
;    IN SIZE_T Length
;    )
;
; Routine Description:
;
;   This function prefetches memory at Source, for Length bytes into the
;   closest cache to the processor.
;
; Arguments:
;
;    Source - Supplies a pointer to the memory to prefetch.
;
;    Length - Supplies the Length, in bytes, of the memory to be moved.
;
; Return Value:
;
;    None.
;
;--

        extrn   _KePrefetchNTAGranularity:DWORD

cPublicFastCall RtlPrefetchMemoryNonTemporal ,2

;
; The following instruction will be patched out at boot time if
; this processor supports the prefetch instruction.
;

        ret                                 ; patched out at boot.

        mov     eax, _KePrefetchNTAGranularity  ; get d-cache line size
@@:     prefetchnta_short rECX, 0           ; prefetch line
        add     ecx, eax                    ; bump prefetch address
        sub     edx, eax                    ; decrement length
        ja      short @b                    ; loop if more to get

        fstRET  RtlPrefetchMemoryNonTemporal ; return

fstENDP RtlPrefetchMemoryNonTemporal


_TEXT$00   ends
        end

