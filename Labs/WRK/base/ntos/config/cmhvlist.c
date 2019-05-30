/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    cmhvlist.c

Abstract:

    Code to maintain registry node that lists where the roots of
    hives are and what files they map to.

--*/

#include "cmp.h"

#define HIVE_LIST L"\\registry\\machine\\system\\currentcontrolset\\control\\hivelist"

BOOLEAN
CmpGetHiveName(
    PCMHIVE         CmHive,
    PUNICODE_STRING HiveName
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,CmpAddToHiveFileList)
#pragma alloc_text(PAGE,CmpRemoveFromHiveFileList)
#pragma alloc_text(PAGE,CmpGetHiveName)
#endif


NTSTATUS
CmpAddToHiveFileList(
    PCMHIVE CmHive
    )
/*++

Routine Description:

    Add Hive to list of hives and their files in
    \registry\machine\system\currentcontrolset\control\hivelist

Arguments:

    HivePath - path to root of hive (e.g. \registry\machine\system)

    CmHive - pointer to CM_HIVE structure for hive.

Return Value:

    ntstatus

--*/
{
#define NAME_BUFFER_SIZE    512
    OBJECT_ATTRIBUTES   ObjectAttributes;
    HANDLE              KeyHandle;
    NTSTATUS            Status;
    PUCHAR              Buffer;
    ULONG               Length;
    PWSTR               FilePath;
    WCHAR               UnicodeNull=UNICODE_NULL;
    UNICODE_STRING      TempName;
    UNICODE_STRING      HivePath;

    //
    // create/open the hive list key
    //
    RtlInitUnicodeString(
        &TempName,
        HIVE_LIST
        );

    InitializeObjectAttributes(
        &ObjectAttributes,
        &TempName,
        OBJ_CASE_INSENSITIVE,
        (HANDLE)NULL,
        NULL
        );

    Status = ZwCreateKey(
                &KeyHandle,
                KEY_READ | KEY_WRITE,
                &ObjectAttributes,
                0,
                NULL,
                REG_OPTION_VOLATILE,
                NULL
                );

    if (!NT_SUCCESS(Status)) {
        CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"CmpAddToHiveFileList: "));
        CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"Create/Open of Hive list failed status = %08lx\n", Status));
        return Status;
    }

    //
    // allocate work buffers
    //
    Buffer = ExAllocatePool(PagedPool, NAME_BUFFER_SIZE + sizeof(WCHAR));
    if (Buffer == NULL) {
        NtClose(KeyHandle);
        return STATUS_NO_MEMORY;
    }

    //
    // compute name of hive
    //
    if (! CmpGetHiveName(CmHive, &HivePath)) {
        NtClose(KeyHandle);
        ExFreePool(Buffer);
        return STATUS_NO_MEMORY;
    }


    //
    // get name of file
    //
    if (!(CmHive->Hive.HiveFlags & HIVE_VOLATILE)) {
        Status = ZwQueryObject(
                    CmHive->FileHandles[HFILE_TYPE_PRIMARY],
                    ObjectNameInformation,
                    (PVOID)Buffer,
                    NAME_BUFFER_SIZE,
                    &Length
                    );
        Length -= sizeof(UNICODE_STRING);
        if (!NT_SUCCESS(Status)) {
            CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"CmpAddToHiveFileList: "));
            CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"Query of name2 failed status = %08lx\n", Status));
            NtClose(KeyHandle);
            ExFreePool(HivePath.Buffer);
            ExFreePool(Buffer);
            return  Status;
        }
        FilePath = ((POBJECT_NAME_INFORMATION)Buffer)->Name.Buffer;
        FilePath[Length/sizeof(WCHAR)] = UNICODE_NULL;
        Length+=sizeof(WCHAR);
    } else {
        FilePath = &UnicodeNull;
        Length = sizeof(UnicodeNull);
    }

    //
    // set entry in list
    //
    Status = ZwSetValueKey(
                KeyHandle,
                &HivePath,
                0,
                REG_SZ,
                FilePath,
                Length
                );
    if (!NT_SUCCESS(Status)) {
        CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"CmpAddToHiveFileList: "));
        CmKdPrintEx((DPFLTR_CONFIG_ID,CML_BUGCHECK,"Set of entry in Hive list failed status = %08lx\n", Status));
    }

    NtClose(KeyHandle);
    ExFreePool(HivePath.Buffer);
    ExFreePool(Buffer);
    return  Status;
}


VOID
CmpRemoveFromHiveFileList(
    PUNICODE_STRING  EntryName
    )
/*++

Routine Description:

    Remove hive name from hive file list key

Arguments:

    CmHive - pointer to CM_HIVE structure for hive.

Return Value:

    ntstatus

--*/
{
    NTSTATUS        Status;
    UNICODE_STRING  TempName;
    OBJECT_ATTRIBUTES   ObjectAttributes;
    HANDLE          KeyHandle;

    //
    // open the hive list key
    //
    RtlInitUnicodeString(
        &TempName,
        HIVE_LIST
        );

    InitializeObjectAttributes(
        &ObjectAttributes,
        &TempName,
        OBJ_CASE_INSENSITIVE,
        (HANDLE)NULL,
        NULL
        );

    Status = ZwOpenKey(
                &KeyHandle,
                KEY_READ | KEY_WRITE,
                &ObjectAttributes
                );

    if (!NT_SUCCESS(Status)) {
        return;
    }

    ZwDeleteValueKey(KeyHandle, EntryName);

    NtClose(KeyHandle);

    return;
}


BOOLEAN
CmpGetHiveName(
    PCMHIVE         CmHive,
    PUNICODE_STRING HiveName
    )
/*++

Routine Description:

    Compute full path to a hive.

Arguments:

    CmHive - pointer to CmHive structure

    HiveName - supplies pointer to unicode string structure that
                 will be filled in with pointer to name.

                CALL IS EXPECTED TO FREE BUFFER

Return Value:

    TRUE = it worked, FALSE = it failed (memory)

--*/
{
    HCELL_INDEX     RootCell;
    HCELL_INDEX     LinkCell;
    PCM_KEY_NODE    LinkKey;
    PCM_KEY_NODE    LinkParent;
    SIZE_T          size;
    SIZE_T          rsize;
    ULONG           KeySize;
    ULONG           ParentSize;
    PWCHAR          p;
    PCM_KEY_NODE    EntryKey;

    //
    // First find the link cell.
    //
    RootCell = CmHive->Hive.BaseBlock->RootCell;
    EntryKey = (PCM_KEY_NODE)HvGetCell((PHHIVE)CmHive, RootCell);
    if( EntryKey == NULL ) {
        //
        // we couldn't map a view for the bin containing this cell
        //
        return FALSE;
    }
    LinkCell = EntryKey->Parent;
    HvReleaseCell((PHHIVE)CmHive, RootCell);

    // for master we don't need to count cell usage
    ASSERT( ((PHHIVE)CmpMasterHive)->ReleaseCellRoutine == NULL );
    //
    // Compute the value entry name, which is of the form:
    //      \registry\<parent of link node name>\<link node name>
    //
    LinkKey = (PCM_KEY_NODE)HvGetCell((PHHIVE)CmpMasterHive, LinkCell);
    if( LinkKey == NULL ) {
        //
        // we couldn't map a view for the bin containing this cell
        //
        return FALSE;
    }
    LinkParent = (PCM_KEY_NODE)HvGetCell(
                                (PHHIVE)CmpMasterHive,
                                LinkKey->Parent
                                );
    if( LinkParent == NULL ) {
        //
        // we couldn't map a view for the bin containing this cell
        //
        return FALSE;
    }
    rsize = wcslen(L"\\REGISTRY\\");

    KeySize = CmpHKeyNameLen(LinkKey);
    ParentSize = CmpHKeyNameLen(LinkParent);
    size = KeySize + ParentSize +
           (rsize * sizeof(WCHAR)) + sizeof(WCHAR);

    HiveName->Buffer = ExAllocatePool(PagedPool, size);
    if (HiveName->Buffer == NULL) {
        return FALSE;
    }

#pragma prefast(suppress:12005, "no overflow. size is bounded by max key length (512)")
    HiveName->Length = (USHORT)size;
    HiveName->MaximumLength = (USHORT)size;
    p = HiveName->Buffer;

    RtlCopyMemory(
        (PVOID)p,
        (PVOID)L"\\REGISTRY\\",
        rsize * sizeof(WCHAR)
        );
    p += rsize;

    if (LinkParent->Flags & KEY_COMP_NAME) {
        CmpCopyCompressedName(p,
                              ParentSize,
                              LinkParent->Name,
                              LinkParent->NameLength);
    } else {
        RtlCopyMemory(
            (PVOID)p,
            (PVOID)&(LinkParent->Name[0]),
            ParentSize
            );
    }

    p += ParentSize / sizeof(WCHAR);

    *p = OBJ_NAME_PATH_SEPARATOR;
    p++;

    if (LinkKey->Flags & KEY_COMP_NAME) {
        CmpCopyCompressedName(p,
                              KeySize,
                              LinkKey->Name,
                              LinkKey->NameLength);

    } else {
        RtlCopyMemory(
            (PVOID)p,
            (PVOID)&(LinkKey->Name[0]),
            KeySize
            );
    }

    return TRUE;
}

