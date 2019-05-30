        TITLE   "Spin Locks"
;++
;
; Copyright (c) Microsoft Corporation. All rights reserved. 
;
; You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
; If you do not agree to the terms, do not use the code.
;
;
;  Module Name:
;
;     spinlock.asm
;
;  Abstract:
;
;     This module implements the routines for acquiring and releasing
;     spin locks.
;
;--

        PAGE

.586p

include ks386.inc
include callconv.inc                    ; calling convention macros
include i386\kimacro.inc
include mac386.inc
include irqli386.inc

        EXTRNP  _KeBugCheckEx,5


_TEXT$00   SEGMENT  PARA PUBLIC 'CODE'
        ASSUME  DS:FLAT, ES:FLAT, SS:NOTHING, FS:NOTHING, GS:NOTHING

        PAGE
        SUBTTL "Ke Acquire Spin Lock At DPC Level"

;++
;
;  VOID
;  KefAcquireSpinLockAtDpcLevel (
;     IN PKSPIN_LOCK SpinLock
;     )
;
;  Routine Description:
;
;     This function acquires a kernel spin lock.
;
;     N.B. This function assumes that the current IRQL is set properly.
;        It neither raises nor lowers IRQL.
;
;  Arguments:
;
;     (ecx) SpinLock - Supplies a pointer to an kernel spin lock.
;
;  Return Value:
;
;     None.
;
;--

align 16
cPublicFastCall KefAcquireSpinLockAtDpcLevel, 1
cPublicFpo 0, 0
if DBG
        push    ecx
        CurrentIrql
        pop     ecx

        cmp     al, DISPATCH_LEVEL
        jl      short asld50
endif

ifdef NT_UP
        fstRET    KefAcquireSpinLockAtDpcLevel
else
;
;   Attempt to assert the lock
;

asld10: ACQUIRE_SPINLOCK    ecx,<short asld20>
        fstRET    KefAcquireSpinLockAtDpcLevel

;
;   Lock is owned, spin till it looks free, then go get it again.
;

align 4
asld20: SPIN_ON_SPINLOCK    ecx,<short asld10>

endif

if DBG
asld50: stdCall   _KeBugCheckEx,<IRQL_NOT_GREATER_OR_EQUAL,ecx,eax,0,0>
        int       3                 ; help debugger backtrace.
endif

fstENDP KefAcquireSpinLockAtDpcLevel


;++
;
;  VOID
;  KeAcquireSpinLockAtDpcLevel (
;     IN PKSPIN_LOCK SpinLock
;     )
;
;  Routine Description:
;
;   Thunk for standard call callers
;
;--

cPublicProc _KeAcquireSpinLockAtDpcLevel, 1
cPublicFpo 1,0

ifndef NT_UP
        mov     ecx,[esp+4]         ; SpinLock

aslc10: ACQUIRE_SPINLOCK    ecx,<short aslc20>
        stdRET    _KeAcquireSpinLockAtDpcLevel

aslc20: SPIN_ON_SPINLOCK    ecx,<short aslc10>
endif
        stdRET    _KeAcquireSpinLockAtDpcLevel
stdENDP _KeAcquireSpinLockAtDpcLevel


        PAGE
        SUBTTL "Ke Release Spin Lock From Dpc Level"
;++
;
;  VOID
;  KefReleaseSpinLockFromDpcLevel (
;     IN PKSPIN_LOCK SpinLock
;     )
;
;  Routine Description:
;
;     This function releases a kernel spin lock.
;
;     N.B. This function assumes that the current IRQL is set properly.
;        It neither raises nor lowers IRQL.
;
;  Arguments:
;
;     (ecx) SpinLock - Supplies a pointer to an executive spin lock.
;
;  Return Value:
;
;     None.
;
;--
align 16
cPublicFastCall KefReleaseSpinLockFromDpcLevel  ,1
cPublicFpo 0,0
ifndef NT_UP
        RELEASE_SPINLOCK    ecx
endif
        fstRET    KefReleaseSpinLockFromDpcLevel

fstENDP KefReleaseSpinLockFromDpcLevel

;++
;
;  VOID
;  KeReleaseSpinLockFromDpcLevel (
;     IN PKSPIN_LOCK SpinLock
;     )
;
;  Routine Description:
;
;   Thunk for standard call callers
;
;--

cPublicProc _KeReleaseSpinLockFromDpcLevel, 1
cPublicFpo 1,0
ifndef NT_UP
        mov     ecx, [esp+4]            ; (ecx) = SpinLock
        RELEASE_SPINLOCK    ecx
endif
        stdRET    _KeReleaseSpinLockFromDpcLevel
stdENDP _KeReleaseSpinLockFromDpcLevel



        PAGE
        SUBTTL "Ki Acquire Kernel Spin Lock"

;++
;
;  VOID
;  FASTCALL
;  KiAcquireSpinLock (
;     IN PKSPIN_LOCK SpinLock
;     )
;
;  Routine Description:
;
;     This function acquires a kernel spin lock.
;
;     N.B. This function assumes that the current IRQL is set properly.
;        It neither raises nor lowers IRQL.
;
;  Arguments:
;
;     (ecx) SpinLock - Supplies a pointer to an kernel spin lock.
;
;  Return Value:
;
;     None.
;
;--

align 16
cPublicFastCall KiAcquireSpinLock  ,1
cPublicFpo 0,0
ifndef NT_UP

;
;   Attempt to assert the lock
;

asl10:  ACQUIRE_SPINLOCK    ecx,<short asl20>
        fstRET    KiAcquireSpinLock

;
;   Lock is owned, spin till it looks free, then go get it again.
;

align 4
asl20:  SPIN_ON_SPINLOCK    ecx,<short asl10>

else
        fstRET    KiAcquireSpinLock
endif

fstENDP KiAcquireSpinLock

        PAGE
        SUBTTL "Ki Release Kernel Spin Lock"
;++
;
;  VOID
;  FASTCALL
;  KiReleaseSpinLock (
;     IN PKSPIN_LOCK SpinLock
;     )
;
;  Routine Description:
;
;     This function releases a kernel spin lock.
;
;     N.B. This function assumes that the current IRQL is set properly.
;        It neither raises nor lowers IRQL.
;
;  Arguments:
;
;     (ecx) SpinLock - Supplies a pointer to an executive spin lock.
;
;  Return Value:
;
;     None.
;
;--
align 16
cPublicFastCall KiReleaseSpinLock  ,1
cPublicFpo 0,0
ifndef NT_UP

        RELEASE_SPINLOCK    ecx

endif
        fstRET    KiReleaseSpinLock

fstENDP KiReleaseSpinLock

        PAGE
        SUBTTL "Try to acquire Kernel Spin Lock"

;++
;
;  BOOLEAN
;  KeTryToAcquireSpinLock (
;     IN PKSPIN_LOCK SpinLock,
;     OUT PKIRQL     OldIrql
;     )
;
;  Routine Description:
;
;     This function attempts acquires a kernel spin lock.  If the
;     spinlock is busy, it is not acquire and FALSE is returned.
;
;  Arguments:
;
;     SpinLock (TOS+4) - Supplies a pointer to an kernel spin lock.
;     OldIrql  (TOS+8) = Location to store old irql
;
;  Return Value:
;     TRUE  - Spinlock was acquired & irql was raise
;     FALSE - SpinLock was not acquired - irql is unchanged.
;
;--

align dword
cPublicProc _KeTryToAcquireSpinLock  ,2
cPublicFpo 2,0

ifdef NT_UP
; UP Version of KeTryToAcquireSpinLock

        RaiseIrql DISPATCH_LEVEL

        mov     ecx, [esp+8]        ; (ecx) -> ptr to OldIrql
        mov     [ecx], al           ; save OldIrql

        mov     eax, 1              ; Return TRUE
        stdRET    _KeTryToAcquireSpinLock

else
; MP Version of KeTryToAcquireSpinLock

        mov     edx,[esp+4]         ; (edx) -> spinlock

;
; First check the spinlock without asserting a lock
;

        TEST_SPINLOCK       edx,<short ttsl10>

;
; Spinlock looks free raise irql & try to acquire it
;

;
; raise to dispatch_level
;

        RaiseIrql DISPATCH_LEVEL

        mov     edx, [esp+4]        ; (edx) -> spinlock
        mov     ecx, [esp+8]        ; (ecx) = Return OldIrql

        ACQUIRE_SPINLOCK    edx,<short ttsl20>

        mov     [ecx], al           ; save OldIrql
        mov     eax, 1              ; spinlock was acquired, return TRUE

        stdRET    _KeTryToAcquireSpinLock

ttsl10: xor     eax, eax            ; return FALSE
        YIELD
        stdRET    _KeTryToAcquireSpinLock

ttsl20:
        YIELD
        mov     ecx, eax            ; (ecx) = OldIrql
        LowerIrql ecx
        xor     eax, eax            ; return FALSE
        stdRET    _KeTryToAcquireSpinLock
endif

stdENDP _KeTryToAcquireSpinLock

        PAGE
        SUBTTL "Ki Try to acquire Kernel Spin Lock"
;++
;
;  BOOLEAN
;  FASTCALL
;  KeTryToAcquireSpinLockAtDpcLevel (
;     IN PKSPIN_LOCK SpinLock
;     )
;
;  Routine Description:
;
;     This function attempts acquires a kernel spin lock.  If the
;     spinlock is busy, it is not acquire and FALSE is returned.
;
;  Arguments:
;
;     SpinLock (ecx) - Supplies a pointer to an kernel spin lock.
;
;  Return Value:
;
;     TRUE  - Spinlock was acquired
;     FALSE - SpinLock was not acquired
;
;--

align dword
cPublicFastCall KeTryToAcquireSpinLockAtDpcLevel  ,1
cPublicFpo 0, 0

;
; First check the spinlock without asserting a lock
;

ifndef NT_UP

        TEST_SPINLOCK       ecx, <short atsl20>

;
; Spinlock looks free try to acquire it.
;

        ACQUIRE_SPINLOCK    ecx, <short atsl20>

endif

        mov     eax, 1              ; spinlock was acquired, return TRUE

        fstRET  KeTryToAcquireSpinLockAtDpcLevel

ifndef NT_UP

atsl20: YIELD                       ;
        xor     eax, eax            ; return FALSE

        fstRET  KeTryToAcquireSpinLockAtDpclevel

endif

fstENDP KeTryToAcquireSpinLockAtDpcLevel

;++
;
;  BOOLEAN
;  KeTestSpinLock (
;     IN PKSPIN_LOCK SpinLock
;     )
;
;  Routine Description:
;
;     This function tests a kernel spin lock.  If the spinlock is
;     busy, FALSE is returned.  If not, TRUE is returned.  The spinlock
;     is never acquired.  This is provided to allow code to spin at low
;     IRQL, only raising the IRQL when there is a reasonable hope of
;     acquiring the lock.
;
;  Arguments:
;
;     SpinLock (ecx) - Supplies a pointer to a kernel spin lock.
;
;  Return Value:
;     TRUE  - Spinlock appears available
;     FALSE - SpinLock is busy
;
;--

cPublicFastCall KeTestSpinLock  ,1
        TEST_SPINLOCK       ecx,<short tso10>
        mov       eax, 1
        fstRET    KeTestSpinLock

tso10:  YIELD
        xor       eax, eax
        fstRET    KeTestSpinLock

fstENDP KeTestSpinLock

        page    ,132
        subttl  "Acquire In Stack Queued SpinLock At Dpc Level"

ifdef QLOCK_STAT_GATHER

        EXTRNP  KiQueueStatTrySucceeded,2,,FASTCALL
        EXTRNP  KiQueueStatTryFailed,1,,FASTCALL
        EXTRNP  KiQueueStatTry,1,,FASTCALL
        EXTRNP  KiQueueStatAcquireQueuedLock,1,,FASTCALL
        EXTRNP  KiQueueStatAcquireQueuedLockRTS,1,,FASTCALL
        EXTRNP  KiQueueStatTryAcquire,3,,FASTCALL
        EXTRNP  KiQueueStatReleaseQueuedLock,2,,FASTCALL
        EXTRNP  KiQueueStatAcquire,1,,FASTCALL
        EXTRNP  KiQueueStatRelease,1,,FASTCALL
        EXTRNP  KiAcquireQueuedLock,1,,FASTCALL
        EXTRNP  KiReleaseQueuedLock,1,,FASTCALL

;
; The following routines are used to wrap the actual calls to the 
; real routines which have been usurped here by patching the import
; table.
;

cPublicFastCall __cap_KeAcquireQueuedSpinLock,1
        sub     esp, 8          ; make room to save time
        push    ecx             ; save args
        rdtsc                   ; get time
        mov     [esp].4, eax    ; save low part
        mov     [esp].8, edx    ; save high part
        mov     ecx, [esp]      ; restore arg
        fstCall KiQueueStatAcquireQueuedLock
acqst:  mov     ecx, esp        ; set arg pointer for data accum
        push    eax             ; save result
        fstCall KiQueueStatAcquire
        pop     eax             ; restore result
        add     esp, 12         ; restore stack pointer
        fstRET  __cap_KeAcquireQueuedSpinLock
        fstENDP __cap_KeAcquireQueuedSpinLock

cPublicFastCall __cap_KeAcquireQueuedSpinLockRaiseToSynch,1
        sub     esp, 8          ; make room to save time
        push    ecx             ; save args
        rdtsc                   ; get time
        mov     [esp].4, eax    ; save low part
        mov     [esp].8, edx    ; save high part
        mov     ecx, [esp]      ; restore arg
        fstCall KiQueueStatAcquireQueuedLockRTS
        jmp     short acqst     ; use common code to finish
        fstENDP __cap_KeAcquireQueuedSpinLockRaiseToSynch

cPublicFastCall __cap_KeTryToAcquireQueuedSpinLockRaiseToSynch,2
        push    ecx             ; save arg
        push    SYNCH_LEVEL
tryst:  fstCall KiQueueStatTryAcquire
        push    eax             ; save result
        mov     ecx, esp
        fstCall KiQueueStatTry
        pop     eax             ; restore result
        add     esp, 4          ; drop saved arg
        or      eax, eax        ; some assembly callers expect appropriate flags
        fstRET  __cap_KeTryToAcquireQueuedSpinLockRaiseToSynch
        fstENDP __cap_KeTryToAcquireQueuedSpinLockRaiseToSynch

cPublicFastCall __cap_KeTryToAcquireQueuedSpinLock,2
        push    ecx             ; save arg
        push    DISPATCH_LEVEL
        jmp     short tryst     ; use common code to finish
        fstENDP __cap_KeTryToAcquireQueuedSpinLock

cPublicFastCall __cap_KeReleaseQueuedSpinLock,2
        push    ecx             ; save args
        mov     ecx, esp        ; set arg for stat release routine
        push    edx             ; save other arg
        fstCall KiQueueStatRelease
        pop     edx
        pop     ecx
        fstCall KiQueueStatReleaseQueuedLock
        fstRET  __cap_KeReleaseQueuedSpinLock
        fstENDP __cap_KeReleaseQueuedSpinLock

;
; KeAcquireQueuedSpinLockAtDpcLevel
; KeReleaseQueuedSpinLockFromDpcLevel
;
; These two routines are defined here in assembly code so
; as to capture the caller's address.
;

cPublicFastCall KeAcquireQueuedSpinLockAtDpcLevel,1
        sub     esp, 8          ; make room to save time
        push    ecx             ; save args
        rdtsc                   ; get time
        mov     [esp].4, eax    ; save low part
        mov     [esp].8, edx    ; save high part
        mov     ecx, [esp]      ; restore arg
        fstCall KiAcquireQueuedLock
        mov     ecx, esp
        fstCall KiQueueStatAcquire
        add     esp, 12         ; restore SP
        fstRET  KeAcquireQueuedSpinLockAtDpcLevel
        fstENDP KeAcquireQueuedSpinLockAtDpcLevel

cPublicFastCall KeReleaseQueuedSpinLockFromDpcLevel,1
        push    ecx             ; save args
        mov     ecx, esp        ; set arg for stat release routine
        fstCall KiQueueStatRelease
        pop     ecx
        fstCall KiReleaseQueuedLock
        fstRET  KeReleaseQueuedSpinLockFromDpcLevel
        fstENDP KeReleaseQueuedSpinLockFromDpcLevel

;
; KiCaptureQueuedSpinlockRoutines
;
; Replace the import table entries for the x86 HAL queued spinlock
; routines with our statistic capturing variety.
;

        EXTRNP  KeAcquireQueuedSpinLock,1,IMPORT,FASTCALL
        EXTRNP  KeAcquireQueuedSpinLockRaiseToSynch,1,IMPORT,FASTCALL
        EXTRNP  KeTryToAcquireQueuedSpinLockRaiseToSynch,2,IMPORT,FASTCALL
        EXTRNP  KeTryToAcquireQueuedSpinLock,2,IMPORT,FASTCALL
        EXTRNP  KeReleaseQueuedSpinLock,2,IMPORT,FASTCALL

cPublicFastCall KiCaptureQueuedSpinlockRoutines,0

        mov     eax, @__cap_KeAcquireQueuedSpinLock@4
        mov     [__imp_@KeAcquireQueuedSpinLock@4], eax

        mov     eax, @__cap_KeAcquireQueuedSpinLockRaiseToSynch@4
        mov     [__imp_@KeAcquireQueuedSpinLockRaiseToSynch@4], eax

        mov     eax, @__cap_KeTryToAcquireQueuedSpinLockRaiseToSynch@8
        mov     [__imp_@KeTryToAcquireQueuedSpinLockRaiseToSynch@8], eax

        mov     eax, @__cap_KeTryToAcquireQueuedSpinLock@8
        mov     [__imp_@KeTryToAcquireQueuedSpinLock@8], eax

        mov     eax, @__cap_KeReleaseQueuedSpinLock@8
        mov     [__imp_@KeReleaseQueuedSpinLock@8], eax

        fstRet  KiCaptureQueuedSpinlockRoutines
        fstENDP KiCaptureQueuedSpinlockRoutines

else

;++
;
; VOID
; FASTCALL
; KeAcquireInStackQueuedSpinLockAtDpcLevel (
;     IN PKSPIN_LOCK SpinLock,
;     IN PKLOCK_QUEUE_HANDLE LockHandle
;     )
;
; Routine Description:
;
;    This function acquires the specified in stack queued spin lock at the
;    current IRQL.
;
; Arguments:
;
;    SpinLock (ecx) - Supplies the address of a spin lock.
;
;    LockHandle (edx) - Supplies the address of an in stack lock handle.
;
; Return Value:
;
;    None.
;--

align 16
cPublicFastCall KeAcquireInStackQueuedSpinLockAtDpcLevel,2
cPublicFpo 0,0

ifndef NT_UP

        xor     eax, eax                 ; set next link to NULL
        mov     [edx].LqhNext, eax       ;
        mov     [edx].LqhLock, ecx       ; set spin lock address
        lea     ecx, dword ptr [edx+LqhNext] ; compute address of lock queue
        jmp     short @KeAcquireQueuedSpinLockAtDpcLevel@4 ; finish in common code

else

        fstRET  KeAcquireInStackQueuedSpinLockAtDpcLevel

endif

fstENDP KeAcquireInStackQueuedSpinLockAtDpcLevel

        page    ,132
        subttl  "Acquire Queued SpinLock"

;++
;
; VOID
; KeAcquireQueuedSpinLockAtDpcLevel (
;     IN PKSPIN_LOCK_QUEUE QueuedLock
;     )
;
; Routine Description:
;
;    This function acquires the specified queued spinlock.
;    No change to IRQL is made, IRQL is not returned.  It is
;    expected IRQL is sufficient to avoid context switch.
;
;    Unlike the equivalent Ke versions of these routines,
;    the argument to this routine is the address of the
;    lock queue entry (for the lock to be acquired) in the
;    PRCB rather than the LockQueueNumber.  This saves us
;    a couple of instructions as the address can be calculated
;    at compile time.
;
;    NOTE: This code may be modified for use during textmode
;    setup if this is an MP kernel running with a UP HAL.
;
; Arguments:
;
;    LockQueueEntry (ecx) - Supplies the address of the queued
;                           spinlock entry in this processor's
;                           PRCB.
;
; Return Value:
;
;    None.
;
;    N.B. ecx is preserved, assembly code callers can take advantage
;    of this by avoiding setting up ecx for the call to release if
;    the caller can preserve the lock that long.
;
;--

        ; compile time assert sizeof(KSPIN_LOCK_QUEUE) == 8

.errnz  (LOCK_QUEUE_HEADER_SIZE - 8)

align 16
cPublicFastCall KeAcquireQueuedSpinLockAtDpcLevel,1
cPublicFpo 0,0

ifndef NT_UP

        ; Get address of the actual lock.

        mov     edx, [ecx].LqLock
        mov     eax, ecx                        ; save Lock Queue entry address

        ; Exchange the value of the lock with the address of this
        ; Lock Queue entry.

        xchg    [edx], eax

        cmp     eax, 0                          ; check if lock is held
        jnz     short @f                        ; jiff held

        ; note: the actual lock address will be word aligned, we use
        ; the bottom two bits as indicators, bit 0 is LOCK_QUEUE_WAIT,
        ; bit 1 is LOCK_QUEUE_OWNER.

        or      edx, LOCK_QUEUE_OWNER           ; mark self as lock owner
        mov     [ecx].LqLock, edx

        ; lock has been acquired, return.

aqsl20:

endif

        fstRET  KeAcquireQueuedSpinLockAtDpcLevel

ifndef NT_UP

@@:

if DBG

        ; make sure it isn't already held by THIS processor.

        test    edx, LOCK_QUEUE_OWNER
        jz      short @f

        ; KeBugCheckEx(SPIN_LOCK_ALREADY_OWNED,
        ;             actual lock address,
        ;             my context,
        ;             previous acquirer,
        ;             2);

        stdCall _KeBugCheckEx,<SPIN_LOCK_ALREADY_OWNED,edx,ecx,eax,2>
@@:

endif
        ; The lock is already held by another processor.  Set the wait
        ; bit in this processor's Lock Queue entry, then set the next
        ; field in the Lock Queue entry of the last processor to attempt
        ; to acquire the lock (this is the address returned by the xchg
        ; above) to point to THIS processor's lock queue entry.

        or      edx, LOCK_QUEUE_WAIT            ; set lock bit
        mov     [ecx].LqLock, edx

        mov     [eax].LqNext, ecx               ; set previous acquirer's
                                                ; next field.
        ; Wait.
@@:
        test    [ecx].LqLock, LOCK_QUEUE_WAIT   ; check if still waiting
        jz      short aqsl20                    ; jif lock acquired
        YIELD                                   ; fire avoidance.
        jmp     short @b                        ; else, continue waiting

endif

fstENDP KeAcquireQueuedSpinLockAtDpcLevel

        page    ,132
        subttl  "Release In Stack Queued SpinLock From Dpc Level"
;++
;
; VOID
; FASTCALL
; KeReleaseInStackQueuedSpinLockFromDpcLevel (
;     IN PKLOCK_QUEUE_HANDLE LockHandle
;     )
;
; Routine Description:
;
;    This function releases a queued spinlock and preserves the current
;    IRQL.
;
; Arguments:
;
;    LockHandle (ecx) - Supplies the address of a lock handle.
;
; Return Value:
;
;    None.
;
;--

cPublicFastCall KeReleaseInStackQueuedSpinLockFromDpcLevel,1
cPublicFpo 0,0

ifndef NT_UP

        lea     ecx, dword ptr[ecx+LqhNext] ; compute address of lock queue
        jmp     short @KeReleaseQueuedSpinLockFromDpcLevel@4 ; finish in common code

else

        fstRET  KeReleaseInStackQueuedSpinLockFromDpcLevel

endif

fstENDP KeReleaseInStackQueuedSpinLockFromDpcLevel

        page    ,132
        subttl  "Release Queued SpinLock"

;++
;
; VOID
; KeReleaseQueuedSpinLockFromDpcLevel (
;     IN PKSPIN_LOCK_QUEUE QueuedLock
;     )
;
; Routine Description:
;
;    This function releases a queued spinlock.
;    No change to IRQL is made, IRQL is not returned.  It is
;    expected IRQL is sufficient to avoid context switch.
;
;    NOTE: This code may be modified for use during textmode
;    setup if this is an MP kernel running with a UP HAL.
;
; Arguments:
;
;    LockQueueEntry (ecx) - Supplies the address of the queued
;                           spinlock entry in this processor's
;                           PRCB.
;
; Return Value:
;
;    None.
;
;--

cPublicFastCall KeReleaseQueuedSpinLockFromDpcLevel,1
cPublicFpo 0,0

.errnz  (LOCK_QUEUE_OWNER - 2)           ; error if not bit 1 for btr

ifndef NT_UP

        mov     eax, ecx                        ; need in eax for cmpxchg
        mov     edx, [ecx].LqNext
        mov     ecx, [ecx].LqLock

        ; Quick check: If Lock Queue entry's Next field is not NULL,
        ; there is another waiter.  Don't bother with ANY atomic ops
        ; in this case.
        ;
        ; N.B. Careful ordering, the test will clear the CF bit and set
        ; the ZF bit appropriately if the Next Field (in EDX) is zero.
        ; The BTR will set the CF bit to the previous value of the owner
        ; bit.

        test    edx, edx

        ; Clear the "I am owner" field in the Lock entry.

        btr     ecx, 1                          ; clear owner bit

if DBG
        jnc     short rqsl90                    ; bugcheck if was not set
                                                ; tests CF
endif

        mov     [eax].LqLock, ecx               ; clear lock bit in queue entry
        jnz     short rqsl40                    ; jif another processor waits
                                                ; tests ZF

        xor     edx, edx                        ; new lock owner will be NULL
        push    eax                             ; save &PRCB->LockQueue[Number]

        ; Use compare exchange to attempt to clear the actual lock.
        ; If there are still no processors waiting for the lock when
        ; the compare exchange happens, the old contents of the lock
        ; should be the address of this lock entry (eax).

        lock cmpxchg [ecx], edx                 ; store 0 if no waiters
        pop     eax                             ; restore lock queue address
        jnz     short rqsl60                    ; jif store failed

        ; The lock has been released.  Return to caller.

endif

        fstRET  KeReleaseQueuedSpinLockFromDpcLevel

ifndef NT_UP

        ; Another processor is waiting on this lock.   Hand the lock
        ; to that processor by getting the address of its LockQueue
        ; entry, turning ON its owner bit and OFF its wait bit.

rqsl40: xor     [edx].LqLock, (LOCK_QUEUE_OWNER+LOCK_QUEUE_WAIT)

        ; Done, the other processor now owns the lock, clear the next
        ; field in my LockQueue entry (to preserve the order for entering
        ; the queue again) and return.

        mov     [eax].LqNext, 0
        fstRET  KeReleaseQueuedSpinLockFromDpcLevel

        ; We get here if another processor is attempting to acquire
        ; the lock but had not yet updated the next field in this
        ; processor's Queued Lock Next field.   Wait for the next
        ; field to be updated.

rqsl60: mov     edx, [eax].LqNext
        test    edx, edx                        ; check if still 0
        jnz     short rqsl40                    ; jif Next field now set.
        YIELD                                   ; wait a bit
        jmp     short rqsl60                    ; continue waiting

if DBG

rqsl90:
        stdCall _KeBugCheckEx,<SPIN_LOCK_NOT_OWNED,ecx,eax,0,0>
        int     3                               ; help debugger back trace.

endif

endif

fstENDP KeReleaseQueuedSpinLockFromDpcLevel

endif

        page    ,132
        subttl  "Try to Acquire Queued SpinLock"

;++
;
; LOGICAL
; KeTryToAcquireQueuedSpinLockAtRaisedIrql (
;     IN PKSPIN_LOCK_QUEUE QueuedLock
;     )
;
; Routine Description:
;
;    This function attempts to acquire the specified queued spinlock.
;    No change to IRQL is made, IRQL is not returned.  It is
;    expected IRQL is sufficient to avoid context switch.
;
;    NOTE: This code may be modified for use during textmode
;    setup if this is an MP kernel running with a UP HAL.
;
; Arguments:
;
;    LockQueueEntry (ecx) - Supplies the address of the queued
;                           spinlock entry in this processor's
;                           PRCB.
;
; Return Value:
;
;    TRUE if the lock was acquired, FALSE otherwise.
;    N.B. ZF is set if FALSE returned, clear otherwise.
;
;--


align 16
cPublicFastCall KeTryToAcquireQueuedSpinLockAtRaisedIrql,1
cPublicFpo 0,0

ifndef NT_UP

        ; Get address of Lock Queue entry

        mov     edx, [ecx].LqLock

        ; Store the Lock Queue entry address in the lock ONLY if the
        ; current lock value is 0.

        xor     eax, eax                        ; old value must be 0
        lock cmpxchg [edx], ecx
        jnz     short taqsl60

        ; Lock has been acquired.

        ; note: the actual lock address will be word aligned, we use
        ; the bottom two bits as indicators, bit 0 is LOCK_QUEUE_WAIT,
        ; bit 1 is LOCK_QUEUE_OWNER.

        or      edx, LOCK_QUEUE_OWNER           ; mark self as lock owner
        mov     [ecx].LqLock, edx

ifdef QLOCK_STAT_GATHER

        mov     edx, [esp]
        fstCall KiQueueStatTrySucceeded

endif

        or      eax, 1                          ; return TRUE

        fstRET  KeTryToAcquireQueuedSpinLockAtRaisedIrql

taqsl60:

        ; The lock is already held by another processor.  Indicate
        ; failure to the caller.

ifdef QLOCK_STAT_GATHER

        fstCall KiQueueStatTryFailed

endif

        xor     eax, eax                        ; return FALSE
        fstRET  KeTryToAcquireQueuedSpinLockAtRaisedIrql

        ; In the event that this is an MP kernel running with a UP
        ; HAL, the following UP version is copied over the MP version
        ; during kernel initialization.

        public  _KeTryToAcquireQueuedSpinLockAtRaisedIrqlUP
_KeTryToAcquireQueuedSpinLockAtRaisedIrqlUP:

endif

        ; UP version, always succeed.

        xor     eax, eax
        or      eax, 1
        fstRet  KeTryToAcquireQueuedSpinLockAtRaisedIrql

fstENDP KeTryToAcquireQueuedSpinLockAtRaisedIrql


_TEXT$00   ends

        end

