/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    gdtsup.c

Abstract:

    This module implements interfaces that support manipulation of i386 GDTs.
    These entry points only exist on i386 machines.

--*/

#include "ki.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGELK, KeI386SetGdtSelector)
#pragma alloc_text(PAGE, Ke386GetGdtEntryThread)
#endif

VOID
Ke386GetGdtEntryThread(
    IN PKTHREAD Thread,
    IN ULONG Offset,
    IN PKGDTENTRY Descriptor
    )
/*++

Routine Description:

    This routine returns the contents of an entry in the GDT.  If the
    entry is thread specific, the entry for the specified thread is
    created and returned (KGDT_LDT, and KGDT_R3_TEB).  If the selector
    is processor dependent, the entry for the current processor is
    returned (KGDT_R0_PCR).

Arguments:

    Thread -- Supplies a pointer to the thread to return the entry for.

    Offset -- Supplies the offset in the Gdt.  This value must be 0
        mod 8.

    Descriptor -- Returns the contents of the Gdt descriptor

Return Value:

    None.

--*/

{
    PKGDTENTRY Gdt;
    PKPROCESS Process;

    //
    // If the entry is out of range, don't return anything
    //

    if (Offset >= KGDT_NUMBER * sizeof(KGDTENTRY)) {
        return ;
    }

    if (Offset == KGDT_LDT) {

        //
        // Materialize Ldt selector
        //

        Process = Thread->Process;
        RtlCopyMemory( Descriptor,
            &(Process->LdtDescriptor),
            sizeof(KGDTENTRY)
            );

    } else {

        //
        // Copy Selector from Ldt
        //
        // N.B. We will change the base later, if it is KGDT_R3_TEB
        //


        Gdt = KiPcr()->GDT;

        RtlCopyMemory(Descriptor, (PCHAR)Gdt + Offset, sizeof(KGDTENTRY));

        //
        // if it is the TEB selector, fix the base
        //

        if (Offset == KGDT_R3_TEB) {
            Descriptor->BaseLow = (USHORT)((ULONG)(Thread->Teb) & 0xFFFF);
            Descriptor->HighWord.Bytes.BaseMid =
                (UCHAR) ( ( (ULONG)(Thread->Teb) & 0xFF0000L) >> 16);
            Descriptor->HighWord.Bytes.BaseHi =
                (CHAR)  ( ( (ULONG)(Thread->Teb) & 0xFF000000L) >> 24);
        }
    }

    return ;
}

NTSTATUS
KeI386SetGdtSelector (
    ULONG       Selector,
    PKGDTENTRY  GdtValue
    )
/*++

Routine Description:

    Sets a GDT entry obtained via KeI386AllocateGdtSelectors to the supplied
    GdtValue.

Arguments:

    Selector - Which GDT to set

    GdtValue - GDT value to set into GDT

Return Value:

    status code

--*/
{
    KAFFINITY       TargetSet;
    PKPRCB          Prcb;
    PKPCR           Pcr;
    PKGDTENTRY      GdtEntry;
    ULONG           GdtIndex, BitNumber;

    PAGED_CODE ();

    //
    // Verify GDT entry passed, and it's above the kernel GDT values
    //

    GdtIndex = Selector >> 3;
    if ((Selector & 0x7) != 0  || GdtIndex < KGDT_NUMBER) {
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Set GDT entry in each processor's GDT
    //

    TargetSet = KeActiveProcessors;
    while (TargetSet != 0) {
        KeFindFirstSetLeftAffinity(TargetSet, &BitNumber);
        ClearMember(BitNumber, TargetSet);

        Prcb = KiProcessorBlock[BitNumber];
        Pcr  = CONTAINING_RECORD (Prcb, KPCR, PrcbData);
        GdtEntry = Pcr->GDT + GdtIndex;

        // set it
        *GdtEntry = *GdtValue;
    }

    return STATUS_SUCCESS;
}

