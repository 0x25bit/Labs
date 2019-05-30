/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    cmdown.c

Abstract:

    This module cleans up all the memory used by CM.

    This routine is intended to be called at system shutdown
    in order to detect memory leaks. It is supposed to free 
    all registry data that is not freed by CmShutdownSystem.

--*/

#include    "cmp.h"

//
// externals
//
extern  PUCHAR                      CmpStashBuffer;
extern  ULONG                       CmpDelayedCloseSize;

extern  BOOLEAN                     HvShutdownComplete;

extern  BOOLEAN                     CmFirstTime;

extern HIVE_LIST_ENTRY CmpMachineHiveList[];

VOID
CmpFreeAllMemory(
    VOID
    );

VOID
CmpDereferenceNameControlBlockWithLock(
    PCM_NAME_CONTROL_BLOCK   Ncb
    );

VOID
CmpDumpKeyBodyList(
    IN PCM_KEY_CONTROL_BLOCK   kcb,
    IN PULONG                  Count,
    IN PVOID                   Context 
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,CmpFreeAllMemory)
#pragma alloc_text(PAGE,CmShutdownSystem)
#endif


VOID
CmpFreeAllMemory(
    VOID
    )
/*++

Routine Description:

    - All hives are freed
    - KCB table is freed 
    - Name hash table is freed
    - delay close table is freed - question: We need to clean/free all delayed close KCBs
    - all notifications/postblocks-aso.

    * equivalent with MmReleaseAllMemory

Arguments:


Return Value:


--*/

{

    PCMHIVE                 CmHive;
    LONG                    i;
    PCM_KEY_CONTROL_BLOCK   KeyControlBlock;
    PLIST_ENTRY             NotifyPtr;
    PCM_NOTIFY_BLOCK        NotifyBlock;
    PCM_POST_BLOCK          PostBlock;
    PCM_KEY_HASH            Current;
    PLIST_ENTRY             AnchorAddr;
    ULONG                   Count;
    BOOLEAN                 MessageDisplayed;

    CmpRunDownDelayDerefKCBEngine(NULL,TRUE);
    //
    // Iterate through the list of the hives in the system
    //
    while (IsListEmpty(&CmpHiveListHead) == FALSE) {
        //
        // Remove the hive from the list
        //
        CmHive = (PCMHIVE)RemoveHeadList(&CmpHiveListHead);
        CmHive = (PCMHIVE)CONTAINING_RECORD(CmHive,
                                            CMHIVE,
                                            HiveList);

        //
        // close hive handles (the ones that are open)
        //
        for (i=0; i<HFILE_TYPE_MAX; i++) {
            // these should be closed by CmShutdownSystem
            ASSERT( CmHive->FileHandles[i] == NULL );
        }
        
        //
        // free the hive lock  and view lock
        //
        CmpFreeMutex(CmHive->ViewLock);
#if DBG
        CmpFreeResource(CmHive->FlusherLock);
#endif

        //
        // Spew in the debugger the names of the keynodes having notifies still set
        //
        NotifyPtr = &(CmHive->NotifyList);
        NotifyPtr = NotifyPtr->Flink;
        MessageDisplayed = FALSE;
        while( NotifyPtr != NULL ) {
            NotifyBlock = CONTAINING_RECORD(NotifyPtr, CM_NOTIFY_BLOCK, HiveList);
            
            AnchorAddr = &(NotifyBlock->PostList);
            PostBlock = (PCM_POST_BLOCK)(NotifyBlock->PostList.Flink);
            // 
            // walk through the list and spew the keynames and postblock types.
            //
            while ( PostBlock != (PCM_POST_BLOCK)AnchorAddr ) {
                PostBlock = CONTAINING_RECORD(PostBlock,
                                              CM_POST_BLOCK,
                                              NotifyList);

                if( PostBlock->PostKeyBody ) {
                    if( MessageDisplayed == FALSE ){
                        MessageDisplayed = TRUE;
                        DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"Dumping untriggered notifications for hive (%lx) (%.*S) \n\n",CmHive,
                            HBASE_NAME_ALLOC / sizeof(WCHAR),CmHive->Hive.BaseBlock->FileName);
                    }
                    switch (PostBlockType(PostBlock)) {
                        case PostSynchronous:
                            DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"Synchronous ");
                            break;
                        case PostAsyncUser:
                            DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"AsyncUser   ");
                            break;
                        case PostAsyncKernel:
                            DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"AsyncKernel ");
                            break;
                    }
                    DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"Notification, PostBlock %p not triggered on KCB %p\n",PostBlock,
                        PostBlock->PostKeyBody->KeyBody->KeyControlBlock);
                }


                //
                // skip to the next element
                //
                PostBlock = (PCM_POST_BLOCK)(PostBlock->NotifyList.Flink);

            }
            NotifyPtr = NotifyPtr->Flink;
        }

        //
        // free security cache
        //
        CmpDestroySecurityCache (CmHive);
        
        //
        // free the hv level structure
        //
        HvFreeHive(&(CmHive->Hive));

        //
        // free the cm level structure
        //
        CmpFree(CmHive, sizeof(CMHIVE));
        
    }

    //
    // Now free the CM globals
    //
    
    // the stash buffer
    if( CmpStashBuffer != NULL ) {
        ExFreePool( CmpStashBuffer );
    }

    //
    // Spew open handles and associated processes
    //
    Count = 0;
    MessageDisplayed = FALSE;
    for (i=0; i<(LONG)CmpHashTableSize; i++) {
        Current = CmpCacheTable[i].Entry;
        while (Current) {
            KeyControlBlock = CONTAINING_RECORD(Current, CM_KEY_CONTROL_BLOCK, KeyHash);
            if( MessageDisplayed == FALSE ){
                MessageDisplayed = TRUE;
                DbgPrintEx(DPFLTR_CONFIG_ID,DPFLTR_ERROR_LEVEL,"\nDumping open handles : \n\n");
            }
            CmpDumpKeyBodyList(KeyControlBlock,&Count,NULL);
            Current = Current->NextHash;
        }
    }
    
    if( Count != 0 ) {
        //
        // there some open handles; bugcheck 
        //
        CM_BUGCHECK( REGISTRY_ERROR,HANDLES_STILL_OPEN_AT_SHUTDOWN,1,Count,0);
    }

    //
    // in case of private alloc, free pages 
    //
    CmpDestroyCmPrivateAlloc();
    CmpDestroyCmPrivateDelayAlloc();
    //
    // For the 3 tables below, the objects actually pointed from inside 
    // should be cleaned up (freed) at the last handle closure time
    // the related handles are closed
    //
    // KCB cache table
    ASSERT( CmpCacheTable != NULL );
    ExFreePool(CmpCacheTable);

    // NameCacheTable
    ASSERT( CmpNameCacheTable != NULL );
    ExFreePool( CmpNameCacheTable );


}

VOID
CmShutdownSystem(
    VOID
    )
/*++

Routine Description:

    Shuts down the registry.

Arguments:

    NONE

Return Value:

    NONE

--*/
{

    PLIST_ENTRY p;
    PCMHIVE     CmHive;
    NTSTATUS    Status;
    PVOID       RegistryRoot;

    CM_PAGED_CODE();

    if (CmpRegistryRootHandle) {
        Status = ObReferenceObjectByHandle(CmpRegistryRootHandle,
                                           KEY_READ,
                                           NULL,
                                           KernelMode,
                                           &RegistryRoot,
                                           NULL);

        if (NT_SUCCESS(Status)) {
            // We want to dereference the object twice -- once for the
            // reference we just made, and once for the reference
            // fromCmpCreateRegistryRoot.
            ObDereferenceObject(RegistryRoot);
            ObDereferenceObject(RegistryRoot);
        }

        ObCloseHandle(CmpRegistryRootHandle, KernelMode);
    }
    
    CmpLockRegistryExclusive();

    //
    // Stop the workers; only if registry has been initializeed
    //
    if( CmFirstTime == FALSE ) {
        CmpShutdownWorkers();
    }
    
    //
    // shut down the registry
    //
    CmpDoFlushAll(TRUE);

    //
    // try to compress the system hive
    //
    CmCompressKey( &(CmpMachineHiveList[SYSTEM_HIVE_INDEX].CmHive->Hive) );

    //
    // close all the hive files
    //
    p = CmpHiveListHead.Flink;
    while(p != &CmpHiveListHead) {
        CmHive = CONTAINING_RECORD(p, CMHIVE, HiveList);
        //
        // we need to unmap all views mapped for this hive first
        //
        CmpDestroyHiveViewList(CmHive);
        CmpUnJoinClassOfTrust(CmHive);
        //
        // dereference the fileobject (if any).
        //
        CmpDropFileObjectForHive(CmHive);

        //
        // now we can safely close all the handles
        //
        CmpCmdHiveClose(CmHive);

        p=p->Flink;
    }

    HvShutdownComplete = TRUE;      // Tell HvSyncHive to ignore all further requests

    if((PoCleanShutdownEnabled() & PO_CLEAN_SHUTDOWN_REGISTRY) && (CmFirstTime == FALSE)){
        //
        // Free aux memory used internally by CM
        //
        CmpFreeAllMemory();
    }

    CmpUnlockRegistry();
    return;
}
