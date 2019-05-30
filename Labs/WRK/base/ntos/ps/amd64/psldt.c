/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    psldt.c

Abstract:

    This module contains AMD64 stubs for the process and thread ldt support.

--*/

#include "psp.h"

NTSTATUS
PspQueryLdtInformation (
    IN PEPROCESS Process,
    OUT PVOID LdtInformation,
    IN ULONG LdtInformationLength,
    OUT PULONG ReturnLength
    )

/*++

Routine Description:

    This function is not implemented on AMD64.

Arguments:

    Process - Supplies a pointer to a executive process object.

    LdtInformation - Supplies a pointer to the information buffer.

    LdtInformationLength - Supplies the length of the information buffer.

    ReturnLength - Supplies a pointer to a variable that receives the number
        of bytes returned in the information buffer.

Return Value:

    STATUS_NOT_IMPLEMENTED

--*/

{

    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(LdtInformation);
    UNREFERENCED_PARAMETER(LdtInformationLength);
    UNREFERENCED_PARAMETER(ReturnLength);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
PspSetLdtSize(
    IN PEPROCESS Process,
    IN PVOID LdtSize,
    IN ULONG LdtSizeLength
    )

/*++

Routine Description:

    This function is not implemented on AMD64.

Arguments:

    Process -- Supplies a pointer to an executive process object.

    LdtSize -- Supplies a pointer to the LDT size information.

    LdtSizeLength - Supplies the length of the LDT size information.


Return Value:

    STATUS_NOT_IMPLEMENTED

--*/

{
    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(LdtSize);
    UNREFERENCED_PARAMETER(LdtSizeLength);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
PspSetLdtInformation(
    IN PEPROCESS Process,
    IN PVOID LdtInformation,
    IN ULONG LdtInformationLength
    )

/*++

Routine Description:

    This function is not implemented on AMD64.

Arguments:

    Process -- Supplies a pointer to an executive process object.

    LdtInformation -- Supplies a pointer to the information buffer.

    LdtInformationLength -- Supplies the length of the information buffer.

Return Value:

    STATUS_NOT_IMPLEMENTED

--*/

{

    UNREFERENCED_PARAMETER(Process);
    UNREFERENCED_PARAMETER(LdtInformation);
    UNREFERENCED_PARAMETER(LdtInformationLength);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
PspQueryDescriptorThread (
    PETHREAD Thread,
    PVOID ThreadInformation,
    ULONG ThreadInformationLength,
    PULONG ReturnLength
    )

/*++

Routine Description:

    This function is not implemented on AMD64.

Arguments:

    Thread - Supplies a pointer to an executive thread object.

    ThreadInformation - Supplies a pointer to the thread descriptor
        information.

    ThreadInformationLength - Supplies the length of the thread descriptor
        information.

    ReturnLength - Supplies a pointer to a variable that receives the number
        of bytes returned in the descriptor information buffer.

Return Value:

    STATUS_NOT_IMPLEMENTED

--*/

{

    UNREFERENCED_PARAMETER(Thread);
    UNREFERENCED_PARAMETER(ThreadInformation);
    UNREFERENCED_PARAMETER(ThreadInformationLength);
    UNREFERENCED_PARAMETER(ReturnLength);

    return STATUS_NOT_IMPLEMENTED;
}

VOID
PspDeleteLdt(
    IN PEPROCESS Process
    )

/*++

Routine Description:

    This function is not implemented on AMD64.

Arguments:

    Process -- Supplies a pointer to an executive process object.

Return Value:

    None.

--*/

{

    UNREFERENCED_PARAMETER(Process);

    return;
}

NTSTATUS
NtSetLdtEntries(
    __in ULONG Selector0,
    __in ULONG Entry0Low,
    __in ULONG Entry0Hi,
    __in ULONG Selector1,
    __in ULONG Entry1Low,
    __in ULONG Entry1Hi
    )

/*++

Routine Description:

    This function is not implemented on AMD64.

Arguments:

    Selector0 - Supplies the number of the first descriptor to set.

    Entry0Low - Supplies the low 32 bits of the descriptor.

    Entry0Hi - Supplies the high 32 bits of the descriptor.

    Selector1 - Supplies the number of the last descriptor to set.

    Entry1Low - Supplies the low 32 bits of the descriptor.

    Entry1Hi - Supplies the high 32 bits of the descriptor.

Return Value:

    None.

--*/

{
    UNREFERENCED_PARAMETER(Selector0);
    UNREFERENCED_PARAMETER(Entry0Low);
    UNREFERENCED_PARAMETER(Entry0Hi);
    UNREFERENCED_PARAMETER(Selector1);
    UNREFERENCED_PARAMETER(Entry1Low);
    UNREFERENCED_PARAMETER(Entry1Hi);

    return STATUS_NOT_IMPLEMENTED;
}

