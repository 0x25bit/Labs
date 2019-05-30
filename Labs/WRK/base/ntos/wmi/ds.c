/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    ds.c

Abstract:

    WMI data provider registration code

--*/

#include "wmikmp.h"

#include <strsafe.h>

void WmipEnableCollectionForNewGuid(
    LPGUID Guid,
    PBINSTANCESET InstanceSet
    );

void WmipDisableCollectionForRemovedGuid(
    LPGUID Guid,
    PBINSTANCESET InstanceSet
    );

ULONG WmipDetermineInstanceBaseIndex(
    LPGUID Guid,
    PWCHAR BaseName,
    ULONG InstanceCount
    );

NTSTATUS
WmipMangleInstanceName(
    IN  LPGUID  Guid,
    IN  PWCHAR  Name,
    IN  SIZE_T  MaxMangledNameLen,
    OUT PWCHAR  MangledName
    );

NTSTATUS WmipBuildInstanceSet(
    PWMIREGGUID RegGuid,
    PWMIREGINFOW WmiRegInfo,
    ULONG BufferSize,
    PBINSTANCESET InstanceSet,
    ULONG ProviderId,
    LPCTSTR MofImagePath
    );

NTSTATUS WmipLinkDataSourceToList(
    PBDATASOURCE DataSource,
    BOOLEAN AddDSToList
    );

void WmipSendGuidUpdateNotifications(
    NOTIFICATIONTYPES NotificationType,
    ULONG GuidCount,
    PTRCACHE *GuidList
    );

void WmipGenerateBinaryMofNotification(
    PBINSTANCESET BinaryMofInstanceSet,
    LPCGUID Guid        
    );

void WmipGenerateRegistrationNotification(
    PBDATASOURCE DataSource,
    ULONG NotificationCode
    );

NTSTATUS WmipAddMofResource(
    PBDATASOURCE DataSource,
    LPWSTR ImagePath,
    BOOLEAN IsImagePath,
    LPWSTR MofResourceName,
    PBOOLEAN NewMofResource
    );

PBINSTANCESET WmipFindISInDSByGuid(
    PBDATASOURCE DataSource,
    LPGUID Guid
    );

ULONG WmipUpdateAddGuid(
    PBDATASOURCE DataSource,
    PWMIREGGUID RegGuid,
    PWMIREGINFO WmiRegInfo,
    ULONG BufferSize,
    PBINSTANCESET *AddModInstanceSet
    );

PWCHAR GuidToString(
    PWCHAR s,
    ULONG SizeInBytes,
    LPGUID piid
    );

BOOLEAN WmipUpdateRemoveGuid(
    PBDATASOURCE DataSource,
    PWMIREGGUID RegGuid,
    PBINSTANCESET *AddModInstanceSet
    );

BOOLEAN WmipIsEqualInstanceSets(
    PBINSTANCESET InstanceSet1,
    PBINSTANCESET InstanceSet2
    );

ULONG WmipUpdateModifyGuid(
    PBDATASOURCE DataSource,
    PWMIREGGUID RegGuid,
    PWMIREGINFO WmiRegInfo,
    ULONG BufferSize,
    PBINSTANCESET *AddModInstanceSet
    );

void WmipCachePtrs(
    LPGUID Ptr1,
    PBINSTANCESET Ptr2,
    ULONG *PtrCount,
    ULONG *PtrMax,
    PTRCACHE **PtrArray
    );

NTSTATUS WmipUpdateDataSource(
    PREGENTRY RegEntry,
    PWMIREGINFOW WmiRegInfo,
    ULONG RetSize
    );

void WmipRemoveDataSourceByDS(
    PBDATASOURCE DataSource
    );

NTSTATUS WmipRemoveDataSource(
    PREGENTRY RegEntry
    );

NTSTATUS WmipInitializeDataStructs(
    void
);

NTSTATUS WmipEnumerateMofResources(
    PWMIMOFLIST MofList,
    ULONG BufferSize,
    ULONG *RetSize
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT,WmipInitializeDataStructs)
#pragma alloc_text(PAGE,WmipEnableCollectionForNewGuid)
#pragma alloc_text(PAGE,WmipDisableCollectionForRemovedGuid)
#pragma alloc_text(PAGE,WmipDetermineInstanceBaseIndex)
#pragma alloc_text(PAGE,WmipMangleInstanceName)
#pragma alloc_text(PAGE,WmipBuildInstanceSet)
#pragma alloc_text(PAGE,WmipLinkDataSourceToList)
#pragma alloc_text(PAGE,WmipSendGuidUpdateNotifications)
#pragma alloc_text(PAGE,WmipGenerateBinaryMofNotification)
#pragma alloc_text(PAGE,WmipGenerateMofResourceNotification)
#pragma alloc_text(PAGE,WmipGenerateRegistrationNotification)
#pragma alloc_text(PAGE,WmipAddMofResource)
#pragma alloc_text(PAGE,WmipAddDataSource)
#pragma alloc_text(PAGE,WmipFindISInDSByGuid)
#pragma alloc_text(PAGE,WmipUpdateAddGuid)
#pragma alloc_text(PAGE,WmipUpdateRemoveGuid)
#pragma alloc_text(PAGE,WmipIsEqualInstanceSets)
#pragma alloc_text(PAGE,WmipUpdateModifyGuid)
#pragma alloc_text(PAGE,WmipCachePtrs)
#pragma alloc_text(PAGE,WmipUpdateDataSource)
#pragma alloc_text(PAGE,WmipRemoveDataSourceByDS)
#pragma alloc_text(PAGE,WmipRemoveDataSource)
#pragma alloc_text(PAGE,WmipEnumerateMofResources)
 
#if DBG
#pragma alloc_text(PAGE,GuidToString)
#endif
#endif


#ifdef ALLOC_DATA_PRAGMA
#pragma const_seg("PAGECONST")
#endif

const GUID WmipBinaryMofGuid = BINARY_MOF_GUID;

// {4EE0B301-94BC-11d0-A4EC-00A0C9062910}
const GUID RegChangeNotificationGuid =
{ 0x4ee0b301, 0x94bc, 0x11d0, { 0xa4, 0xec, 0x0, 0xa0, 0xc9, 0x6, 0x29, 0x10 } };


void WmipEnableCollectionForNewGuid(
    LPGUID Guid,
    PBINSTANCESET InstanceSet
    )
{
    WNODE_HEADER Wnode;
    PBGUIDENTRY GuidEntry;
    ULONG Status;
    BOOLEAN IsTraceLog;

    PAGED_CODE();
    
    GuidEntry = WmipFindGEByGuid(Guid, FALSE);

    if (GuidEntry != NULL)
    {
        memset(&Wnode, 0, sizeof(WNODE_HEADER));
        memcpy(&Wnode.Guid, Guid, sizeof(GUID));
        Wnode.BufferSize = sizeof(WNODE_HEADER);

        WmipEnterSMCritSection();
        if ((GuidEntry->EventRefCount > 0) &&
            ((InstanceSet->Flags & IS_ENABLE_EVENT) == 0))

        {
            //
            // Events were previously enabled for this guid, but not for this
            // instance set so call data source for instance set to enable
            // the events. First set the in progress flag and InstanceSet
            // set flag to denote that events have been enabled for the
            // instance set.
            InstanceSet->Flags |= IS_ENABLE_EVENT;

            //
            // If it is Tracelog, NewGuid notifications are piggybacked with
            // Registration call return. 
            //
            IsTraceLog = ((InstanceSet->Flags & IS_TRACED) == IS_TRACED) ? TRUE : FALSE;
            if (IsTraceLog) 
            {
                if (!(InstanceSet->DataSource->Flags & DS_KERNEL_MODE) ) 
                {
                    if (GuidEntry != NULL)
                    {
                        WmipUnreferenceGE(GuidEntry);
                    }
                    WmipLeaveSMCritSection();
                    return;
                }
            
                //
                // For the Kernel Mode Trace Providers pass on the context
                //
                Wnode.HistoricalContext = GuidEntry->LoggerContext;
            }

            GuidEntry->Flags |= GE_FLAG_NOTIFICATION_IN_PROGRESS;

            WmipLeaveSMCritSection();
            WmipDeliverWnodeToDS(IRP_MN_ENABLE_EVENTS,
                                 InstanceSet->DataSource,
                                 &Wnode,
                                 Wnode.BufferSize);
            WmipEnterSMCritSection();

            //
            // Now we need to check if events were disabled while the enable
            // request was in progress. If so go do the work to actually
            // disable them.
            if (GuidEntry->EventRefCount == 0)
            {
                Status = WmipDoDisableRequest(GuidEntry,
                                          TRUE,
                                             IsTraceLog,
                                           GuidEntry->LoggerContext,
                                          GE_FLAG_NOTIFICATION_IN_PROGRESS);

            } else {
                GuidEntry->Flags &= ~GE_FLAG_NOTIFICATION_IN_PROGRESS;
            }
        }

        //
        // Now check to see if collection needs to be enabled for this guid
        //
        if ((GuidEntry->CollectRefCount > 0) &&
            ((InstanceSet->Flags & IS_ENABLE_COLLECTION) == 0)  &&
            (InstanceSet->Flags & IS_EXPENSIVE) )

        {
            //
            // Collection was previously enabled for this guid, but not
            // for this instance set so call data source for instance set
            // to enable collection. First set the in progress flag and
            // InstanceSet set flag to denote that collection has been enabled
            //  for the instance set.
            //
            GuidEntry->Flags |= GE_FLAG_COLLECTION_IN_PROGRESS;
            InstanceSet->Flags |= IS_ENABLE_COLLECTION;

            WmipLeaveSMCritSection();
            WmipDeliverWnodeToDS(IRP_MN_ENABLE_COLLECTION,
                                 InstanceSet->DataSource,
                                 &Wnode,
                                 Wnode.BufferSize);
            WmipEnterSMCritSection();

            //
            // Now we need to check if events were disabled while the enable
            // request was in progress. If so go do the work to actually
            // disable them.
            //
            if (GuidEntry->CollectRefCount == 0)
            {
                Status = WmipDoDisableRequest(GuidEntry,
                                          FALSE,
                                             FALSE,
                                           0,
                                          GE_FLAG_COLLECTION_IN_PROGRESS);

            } else {
                GuidEntry->Flags &= ~GE_FLAG_COLLECTION_IN_PROGRESS;
        
                //
                   // If there are any other threads that were waiting 
                // until all of the enable/disable work completed, we 
                // close the event handle to release them from their wait.
                //
                WmipReleaseCollectionEnabled(GuidEntry);
            }
        }
        WmipUnreferenceGE(GuidEntry);
        WmipLeaveSMCritSection();
    } else {
        WmipAssert(FALSE);
    }
}

void WmipDisableCollectionForRemovedGuid(
    LPGUID Guid,
    PBINSTANCESET InstanceSet
    )
{
    WNODE_HEADER Wnode;
    PBGUIDENTRY GuidEntry;
    ULONG Status;
    BOOLEAN IsTraceLog;

    PAGED_CODE();
    
    GuidEntry = WmipFindGEByGuid(Guid, FALSE);

    if (GuidEntry != NULL)
    {
        memset(&Wnode, 0, sizeof(WNODE_HEADER));
        memcpy(&Wnode.Guid, Guid, sizeof(GUID));
        Wnode.BufferSize = sizeof(WNODE_HEADER);

        WmipEnterSMCritSection();

        if ((GuidEntry->EventRefCount > 0) &&
               ((InstanceSet->Flags & IS_ENABLE_EVENT) != 0))

        {
            // Events were previously enabled for this guid, but not for this
            // instance set so call data source for instance set to enable
            // the events. First set the in progress flag and InstanceSet
            // set flag to denote that events have been enabled for the
            // instance set.
            InstanceSet->Flags &= ~IS_ENABLE_EVENT;

            //
            // If it is Tracelog, RemoveGuid notifications are handled
            // through UnregisterGuids call. 
            //
            IsTraceLog = ((InstanceSet->Flags & IS_TRACED) == IS_TRACED) ? TRUE : FALSE;
            if (IsTraceLog)
            {
                if ( !(InstanceSet->DataSource->Flags & DS_KERNEL_MODE)) 
                {
                    WmipUnreferenceGE(GuidEntry);
                    WmipLeaveSMCritSection();
                    return;
                }
                Wnode.HistoricalContext = GuidEntry->LoggerContext;
            }


            GuidEntry->Flags |= GE_FLAG_NOTIFICATION_IN_PROGRESS;

            WmipLeaveSMCritSection();
            WmipDeliverWnodeToDS(IRP_MN_DISABLE_EVENTS,
                                 InstanceSet->DataSource,
                                 &Wnode,
                                 Wnode.BufferSize);
            WmipEnterSMCritSection();

            //
            // Now we need to check if events were disabled while the enable
            // request was in progress. If so go do the work to actually
            // disable them.
            if (GuidEntry->EventRefCount == 0)
            {
                Status = WmipDoDisableRequest(GuidEntry,
                                          TRUE,
                                             IsTraceLog,
                                           GuidEntry->LoggerContext,
                                          GE_FLAG_NOTIFICATION_IN_PROGRESS);

            } else {
                GuidEntry->Flags &= ~GE_FLAG_NOTIFICATION_IN_PROGRESS;
            }
        }

        //
        // Now check to see if collection needs to be enabled for this guid
        if ((GuidEntry->CollectRefCount > 0) &&
            ((InstanceSet->Flags & IS_ENABLE_COLLECTION) != 0))

        {
            // Collection was previously enabled for this guid, but not
            // for this instance set so call data source for instance set
            // to enable collection. First set the in progress flag and
            // InstanceSet set flag to denote that collection has been enabled
            //  for the instance set.
            GuidEntry->Flags |= GE_FLAG_COLLECTION_IN_PROGRESS;
            InstanceSet->Flags &= ~IS_ENABLE_COLLECTION;

            WmipLeaveSMCritSection();
            WmipDeliverWnodeToDS(IRP_MN_DISABLE_COLLECTION,
                                 InstanceSet->DataSource,
                                 &Wnode,
                                 Wnode.BufferSize);
            WmipEnterSMCritSection();

            //
            // Now we need to check if events were disabled while the enable
            // request was in progress. If so go do the work to actually
            // disable them.
            if (GuidEntry->CollectRefCount == 0)
            {
                Status = WmipDoDisableRequest(GuidEntry,
                                          FALSE,
                                             FALSE,
                                           0,
                                          GE_FLAG_COLLECTION_IN_PROGRESS);

            } else {
                GuidEntry->Flags &= ~GE_FLAG_COLLECTION_IN_PROGRESS;
        
                //
                // If there are any other threads that were waiting 
                // until all of the enable/disable work completed, we 
                // close the event handle to release them from their wait.
                //
                WmipReleaseCollectionEnabled(GuidEntry);
            }
        }
        WmipUnreferenceGE(GuidEntry);
        WmipLeaveSMCritSection();
    } else {
        WmipAssert(FALSE);
    }
}

ULONG WmipDetermineInstanceBaseIndex(
    LPGUID Guid,
    PWCHAR BaseName,
    ULONG InstanceCount
    )
/*++

Routine Description:

    Figure out the base index for the instance names specified by a base
    instance name. We walk the list of instances sets for the guid and if
    there is a match in the base instance name we set the base instance index
    above that used by the previously registered instance set.

Arguments:

    Guid points at guid for the instance names
    BaseName points at the base name for the instances
    InstanceCount is the count of instance names

Return Value:

    Base index for instance name

--*/
{
    PBGUIDENTRY GuidEntry;
    ULONG BaseIndex = 0;
    PLIST_ENTRY InstanceSetList;
    PBINSTANCESET InstanceSet;
    ULONG LastBaseIndex;

    PAGED_CODE();

    UNREFERENCED_PARAMETER (InstanceCount);

    WmipEnterSMCritSection();
    
    GuidEntry = WmipFindGEByGuid(Guid, FALSE);
    if (GuidEntry != NULL)
    {
        InstanceSetList = GuidEntry->ISHead.Flink;
        while (InstanceSetList != &GuidEntry->ISHead)
        {
            InstanceSet = CONTAINING_RECORD(InstanceSetList,
                                            INSTANCESET,
                                            GuidISList);
            if (InstanceSet->Flags & IS_INSTANCE_BASENAME)
            {
                if (wcscmp(BaseName, InstanceSet->IsBaseName->BaseName) == 0)
                {
                    LastBaseIndex = InstanceSet->IsBaseName->BaseIndex + InstanceSet->Count;
                    if (BaseIndex <= LastBaseIndex)
                    {
                        BaseIndex = LastBaseIndex;
                    }
                }
            }
            InstanceSetList = InstanceSetList->Flink;
        }
        WmipUnreferenceGE(GuidEntry);
    }
    
    WmipLeaveSMCritSection();
    
    WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: Static instance name %ws has base index %x\n",
                    BaseName, BaseIndex));
    return(BaseIndex);
}

NTSTATUS
WmipMangleInstanceName(
    IN  LPGUID  Guid,
    IN  PWCHAR  OriginalName,
    IN  SIZE_T  MaxMangledNameLen,
    OUT PWCHAR  MangledName
    )
/*++

Routine Description:

    Copies a static instance name from the input buffer to the output
    buffer, mangling it if the name collides with another name for the
    same guid.

Arguments:

    Guid - points at guid for the instance name
    
    OriginalName - points at the proposed, null-terminated instance name
    
    MaxMangledNameLen - has the maximum number of chars in mangled name buffer,
        including the terminating NULL
        
    MangledName - points at buffer to return the null-terminated mangled name

Return Value:

    Normal NTSTATUS code.
    
Note:

    This algorithm is busted! It's supposed to try up to (26^6) mangled name
    variations, but it actually only tries up to (26 * 6) variations.
    
    Note that, if we wanted to, we could actually go for (62^6) variations
    if we used uppercase letters, lowercase letters and numbers.

--*/
{
    PBGUIDENTRY     GuidEntry;
    ULONG           InstanceIndex;
    PBINSTANCESET   InstanceSet;
    SIZE_T          ManglePos;
    WCHAR           ManglingChar;
    SIZE_T          NameLength;
    NTSTATUS        Status;

    PAGED_CODE();

    WmipAssert(Guid != NULL);
    WmipAssert(OriginalName != NULL);
    WmipAssert(MangledName != NULL);

    GuidEntry = NULL;

    if (Guid == NULL ||
        OriginalName == NULL ||
        MangledName == NULL) {

        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    NameLength = wcslen(OriginalName);
    
    WmipAssert(MaxMangledNameLen >= NameLength + 1);

    //
    // To make prefast happy, we have to wcsncpy the string and manually
    // terminate it.
    //

    wcsncpy(MangledName, OriginalName, NameLength);
    MangledName[NameLength] = UNICODE_NULL;

    Status = STATUS_SUCCESS;

    GuidEntry = WmipFindGEByGuid(Guid, FALSE);

    if (GuidEntry == NULL) {
        goto Exit;
    }

    //
    // Set ManglePos to the last valid character in the name and set
    // ManglingChar to 'Z' -- note that this will trigger the loop below
    // to move ManglePos one position forward (to the first spot we actually
    // want to mangle) and reset ManglingChar to 'A' to really start the
    // mangling.
    //

    ManglePos = NameLength - 1;
    ManglingChar = L'Z';

    //
    // Loop until we get a unique name.
    //

    InstanceSet = WmipFindISinGEbyName(GuidEntry, MangledName, &InstanceIndex);

    while (InstanceSet != NULL) {

        WmipUnreferenceIS(InstanceSet);
        WmipDebugPrintEx((DPFLTR_WMICORE_ID,
                          DPFLTR_INFO_LEVEL,
                          "WMI: Need to mangle name %ws\n",
                          MangledName));
        
        if (ManglingChar == L'Z') {

            //
            // Reset back to 'A' and move to the next character position to
            // try and mangle.
            //

            ManglingChar = L'A';
            ++ManglePos;
            
            if (ManglePos == MaxMangledNameLen - 1) {

                //
                // We're out of room, so fail.
                //

                WmipDebugPrintEx((DPFLTR_WMICORE_ID,
                                  DPFLTR_INFO_LEVEL,
                                  "WMI: Instance Name could not be mangled\n"));
                
                Status = STATUS_UNSUCCESSFUL;
                goto Exit;
            }

            //
            // Make sure to re-terminate the string.
            //

            MangledName[ManglePos + 1] = UNICODE_NULL;
        } else {
            ++ManglingChar;
        }

        MangledName[ManglePos] = ManglingChar;
        
        InstanceSet = WmipFindISinGEbyName(GuidEntry,
                                           MangledName,
                                           &InstanceIndex) ;
    }

Exit:

    if (GuidEntry != NULL) {
        WmipUnreferenceGE(GuidEntry);
    }

    return Status;
}

NTSTATUS
WmipBuildInstanceSet(
    IN  PWMIREGGUID     RegGuid,
    IN  PWMIREGINFOW    WmiRegInfo,
    IN  ULONG           BufferSize,
    IN  PBINSTANCESET   InstanceSet,
    IN  ULONG           ProviderId,
    IN  LPCTSTR         MofImagePath
    )
/*++

Routine Description:

    This routine sets up the passed in InstanceSet with the appropriate
    instance names as described in RegGuid.
    
Arguments:

    RegGuid - pointer to registration information about this instance set
    
    WmiRegInfo - the WMI registration blob that contains RegGuid
    
    BufferSize - total size of WmiRegInfo
    
    InstanceSet - the instance set to fill out
    
    ProviderId - id of the provider registering with WMI
    
    MofImagePath - path the binary containing this provider's MOF data
    
Return Value:

    Normal NTSTATUS code.

--*/
{
    ULONG           i;
    ULONG           InstanceCount;
    PWCHAR          InstanceName;
    SIZE_T          InstanceNameOffset;
    PWCHAR          InstanceNamePtr;
    PBISBASENAME    IsBaseName;
    PBISSTATICNAMES IsStaticName;
    SIZE_T          MaxStaticInstanceNameLength;
    SIZE_T          NameLength;
    SIZE_T          SizeNeeded;
    PWCHAR          StaticInstanceNameBuffer;
    PWCHAR          StaticNames;
    NTSTATUS        Status;

    PAGED_CODE();

    WmipAssert(RegGuid != NULL);
    WmipAssert(WmiRegInfo != NULL);
    WmipAssert(BufferSize > 0);
    WmipAssert(InstanceSet != NULL);

    UNREFERENCED_PARAMETER(MofImagePath);
    
    StaticInstanceNameBuffer = NULL;

    if (RegGuid == NULL ||
        WmiRegInfo == NULL ||
        BufferSize == 0 ||
        InstanceSet == NULL) {

        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    //
    // Remember the count of instances for the guid in the DS.
    //

    InstanceCount = RegGuid->InstanceCount;

    InstanceSet->Count = InstanceCount;
    InstanceSet->ProviderId = ProviderId;

    //
    // Reset the cached size of space needed for all instance names in a
    // "query all data" request. This will be recomputed when the next
    // request comes in.
    //

    InstanceSet->WADInstanceNameSize = 0;
    
    //
    // Reset any flags that might be changed by a new REGGUID.
    //
    
    InstanceSet->Flags &= ~(IS_EXPENSIVE |
                            IS_EVENT_ONLY |
                            IS_PDO_INSTANCENAME |
                            IS_INSTANCE_STATICNAMES |
                            IS_INSTANCE_BASENAME);

    //
    // Finish initializing the Instance Set flags.
    //

    if (RegGuid->Flags & WMIREG_FLAG_EXPENSIVE) {
        InstanceSet->Flags |= IS_EXPENSIVE;
    }

    if (RegGuid->Flags & WMIREG_FLAG_TRACED_GUID) {

        //
        // This guid is not queryable, but is used for sending trace
        // events. We mark the InstanceSet as special.
        //
        
        InstanceSet->Flags |= IS_TRACED;

        if (RegGuid->Flags & WMIREG_FLAG_TRACE_CONTROL_GUID) {
            InstanceSet->Flags |= IS_CONTROL_GUID;
        }
    }

    if (RegGuid->Flags & WMIREG_FLAG_EVENT_ONLY_GUID) {

        //
        // This guid is not queryable, but is only used for sending
        // events. We mark the InstanceSet as special.
        //
        
        InstanceSet->Flags |= IS_EVENT_ONLY;
    }

    InstanceNameOffset = (SIZE_T)RegGuid->InstanceNameList;
    InstanceName = (PWCHAR)OffsetToPtr(WmiRegInfo, InstanceNameOffset);

    if (RegGuid->Flags & WMIREG_FLAG_INSTANCE_LIST) {

        //
        // We have static list of instance names that might need mangling
        // We assume that any name mangling that must occur can be
        // done with a suffix of 6 or fewer characters. This allows
        // up to 100,000 identical static instance names within the
        // same guid. First lets get the amount of memory we'll need.
        //

        SizeNeeded = FIELD_OFFSET(ISSTATICENAMES, StaticNamePtr[0]) + 1;
        MaxStaticInstanceNameLength = 0;

        for (i = 0; i < InstanceCount; ++i) {

            Status = WmipValidateWmiRegInfoString(WmiRegInfo,
                                                  BufferSize,
                                                  (ULONG)InstanceNameOffset,
                                                  &InstanceNamePtr);
                        
            if (!NT_SUCCESS(Status) || (InstanceNamePtr == NULL)) {

                WmipDebugPrintEx((DPFLTR_WMICORE_ID,
                                  DPFLTR_INFO_LEVEL,
                                  "WMI: WmipBuildInstanceSet: bad static instance name %x\n",
                                  InstanceNamePtr));
                
                WmipReportEventLog(EVENT_WMI_INVALID_REGINFO,
                                   EVENTLOG_WARNING_TYPE,
                                   0,
                                   WmiRegInfo->BufferSize,
                                   WmiRegInfo,
                                   1,
                                   MofImagePath != NULL ? MofImagePath : TEXT("Unknown"));
                
                Status = STATUS_INVALID_PARAMETER;
                goto Exit;
            }

            //
            // InstanceNamePtr now points to the beginning of a counted string.
            //

            NameLength = (*InstanceNamePtr) / sizeof(WCHAR);

            //
            // Keep track of the longest instance name.
            //

            if (NameLength > MaxStaticInstanceNameLength) {
                MaxStaticInstanceNameLength = NameLength;
            }

            //
            // Assume the worst case -- that this instance name will need all
            // possible suffix characters for mangling.
            //

            SizeNeeded += (sizeof(PWCHAR) +
                           ((NameLength + MAXBASENAMESUFFIXLENGTH + 1) * sizeof(WCHAR)));
                        
            InstanceNameOffset += ((1 + NameLength) * sizeof(WCHAR));
        }

        IsStaticName = (PBISSTATICNAMES)WmipAlloc(SizeNeeded);
        
        if (IsStaticName == NULL) {
            
            WmipDebugPrintEx((DPFLTR_WMICORE_ID,
                              DPFLTR_INFO_LEVEL,
                              "WMI: WmipBuildInstanceSet: alloc static instance names\n"));
            
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }

        //
        // Once we assign IsStaticName to the InstanceSet, it will be cleaned
        // up when the InstanceSet goes away.
        //

        InstanceSet->Flags |= IS_INSTANCE_STATICNAMES;
        InstanceSet->IsStaticNames = IsStaticName;

        //
        // Allocate a temporary buffer big enough for the longest possible
        // mangled, null-terminated, string.
        //

        StaticInstanceNameBuffer = WmipAlloc((MaxStaticInstanceNameLength + 1) * sizeof(WCHAR));
        
        if (StaticInstanceNameBuffer == NULL) {
            
            WmipDebugPrintEx((DPFLTR_WMICORE_ID,
                              DPFLTR_INFO_LEVEL,
                              "WMI: WmipBuildInstanceSet: couldn't alloc StaticInstanceNameBuffer\n"));
            
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }

        //
        // The actual instance names start immediately following the array
        // of StaticNamePtrs.
        //

        StaticNames = (PWCHAR)&IsStaticName->StaticNamePtr[InstanceCount];
        InstanceNamePtr = InstanceName;

        for (i = 0; i < InstanceCount; ++i) {

            IsStaticName->StaticNamePtr[i] = StaticNames;
            
            NameLength = (*InstanceNamePtr) / sizeof(WCHAR);
            ++InstanceNamePtr;
            
            //
            // Copy the counted instance name into a temporary, null-terminated
            // buffer.
            //

            wcsncpy(StaticInstanceNameBuffer, InstanceNamePtr, NameLength);
            StaticInstanceNameBuffer[NameLength] = UNICODE_NULL;
            
            //
            // Mangle the name if it needs it.
            //

            Status = WmipMangleInstanceName(&RegGuid->Guid,
                                            StaticInstanceNameBuffer,
                                            NameLength + MAXBASENAMESUFFIXLENGTH + 1,
                                            StaticNames);

            if (!NT_SUCCESS(Status)) {
                goto Exit;
            }

            //
            // Update the string pointers for both the new blob of strings
            // (StaticNames) and the original blob of strings (InstanceNamePtr).
            //

            StaticNames += (wcslen(StaticNames) + 1);
            InstanceNamePtr += NameLength;
        }

    } else if (RegGuid->Flags & WMIREG_FLAG_INSTANCE_BASENAME) {

        //
        // We have static instance names built from a base name.
        //

        Status = WmipValidateWmiRegInfoString(WmiRegInfo,
                                              BufferSize,
                                              (ULONG)InstanceNameOffset,
                                              &InstanceNamePtr);
                        
        if (!NT_SUCCESS(Status) || (InstanceNamePtr == NULL)) {

            WmipDebugPrintEx((DPFLTR_WMICORE_ID,
                              DPFLTR_INFO_LEVEL,
                              "WMI: WmipBuildInstanceSet: Invalid instance base name %x\n",
                              InstanceName));

            WmipReportEventLog(EVENT_WMI_INVALID_REGINFO,
                               EVENTLOG_WARNING_TYPE,
                               0,
                               WmiRegInfo->BufferSize,
                               WmiRegInfo,
                               1,
                               MofImagePath ? MofImagePath : TEXT("Unknown"));
            
            Status = STATUS_INVALID_PARAMETER;
            goto Exit;
        }

        NameLength = (*InstanceNamePtr) / sizeof(WCHAR);
        ++InstanceNamePtr;

        IsBaseName = (PBISBASENAME)WmipAlloc(FIELD_OFFSET(ISBASENAME, BaseName[0]) +
                                             ((NameLength + 1) * sizeof(WCHAR)));

        if (IsBaseName == NULL) {

            WmipDebugPrintEx((DPFLTR_WMICORE_ID,
                              DPFLTR_INFO_LEVEL,
                              "WMI: WmipBuildInstanceSet: alloc ISBASENAME failed\n"));
            
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }

        //
        // Once we assign IsBaseName to the InstanceSet, it will be cleaned
        // up when the InstanceSet goes away.
        //

        InstanceSet->Flags |= IS_INSTANCE_BASENAME;
        InstanceSet->IsBaseName = IsBaseName;

        if (RegGuid->Flags & WMIREG_FLAG_INSTANCE_PDO) {
            InstanceSet->Flags |= IS_PDO_INSTANCENAME;
        }

        //
        // Copy the counted instance name into BaseName as a null-terminated
        // string.
        //

        wcsncpy(&IsBaseName->BaseName[0], InstanceNamePtr, NameLength);
        IsBaseName->BaseName[NameLength] = UNICODE_NULL;

        IsBaseName->BaseIndex = WmipDetermineInstanceBaseIndex(&RegGuid->Guid,
                                                               &IsBaseName->BaseName[0],
                                                               RegGuid->InstanceCount);
    }

    Status = STATUS_SUCCESS;

Exit:

    if (StaticInstanceNameBuffer != NULL) {
        WmipFree(StaticInstanceNameBuffer);
    }

    return Status;
}

NTSTATUS WmipLinkDataSourceToList(
    PBDATASOURCE DataSource,
    BOOLEAN AddDSToList
    )
/*++

Routine Description:

    This routine will take a DataSource that was just registered or updated
    and link any new InstanceSets to an appropriate GuidEntry. Then if the
    AddDSToList is TRUE the DataSource itself will be added to the main
    data source list.

    This routine will do all of the linkages within a critical section so the
    data source and its new instances are added atomically. The routine will
    also determine if the guid entry associated with a InstanceSet is a
    duplicate of another that is already on the main guid entry list and if
    so will use the preexisting guid entry.

    This routine assumes that the SM critical section has been taken

Arguments:

    DataSource is a based pointer to a DataSource structure

    AddDSToList    is TRUE then data source will be added to the main list
        of data sources

Return Value:

    ERROR_SUCCESS or an error code

--*/
{
    PBINSTANCESET InstanceSet;
    PLIST_ENTRY InstanceSetList;
    PBGUIDENTRY GuidEntry;

    PAGED_CODE();
    
    InstanceSetList = DataSource->ISHead.Flink;
    while (InstanceSetList != &DataSource->ISHead)
    {
        InstanceSet = CONTAINING_RECORD(InstanceSetList,
                                        INSTANCESET,
                                        DSISList);
        //
        // If this instance set has just been registered then we need to
        // get it on a GuidEntry list.
        if (InstanceSet->Flags & IS_NEWLY_REGISTERED)
        {
            //
            // See if there is already a GUID entry for the instance set.
            // If not go allocate a new guid entry and place it on the
            // main guid list. If there already is a GuidEntry for the
            // InstanceSet we will assign the ref count that was given by
            // the WmipFindGEByGuid to the DataSource which will unreference
            // the GuidEntry when the DataSource is unregistered.
            GuidEntry = WmipFindGEByGuid((LPGUID)InstanceSet->GuidEntry, 
                                          FALSE);
            if (GuidEntry == NULL)
            {
                GuidEntry = WmipAllocGuidEntry();
                if (GuidEntry == NULL)
                {
                    WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: WmipLinkDataSourceToList: WmipAllocGuidEntry failed\n"));
                    return(STATUS_INSUFFICIENT_RESOURCES);
                }

                //
                // Initialize the new GuidEntry and place it on the master
                // GuidEntry list.
                memcpy(&GuidEntry->Guid,
                       (LPGUID)InstanceSet->GuidEntry,
                       sizeof(GUID));
           
                InsertHeadList(WmipGEHeadPtr, &GuidEntry->MainGEList);
            }
            InstanceSet->GuidEntry = GuidEntry;
            InstanceSet->Flags &= ~IS_NEWLY_REGISTERED;
            InsertTailList(&GuidEntry->ISHead, &InstanceSet->GuidISList);
            GuidEntry->ISCount++;
        }

        InstanceSetList = InstanceSetList->Flink;
    }


    if (AddDSToList)
    {
        WmipAssert(! (DataSource->Flags & FLAG_ENTRY_ON_INUSE_LIST));

        DataSource->Flags |= FLAG_ENTRY_ON_INUSE_LIST;
        InsertTailList(WmipDSHeadPtr, &DataSource->MainDSList);
    }

    return(STATUS_SUCCESS);
}

void WmipSendGuidUpdateNotifications(
    NOTIFICATIONTYPES NotificationType,
    ULONG GuidCount,
    PTRCACHE *GuidList
    )
{
    PUCHAR WnodeBuffer;
    PWNODE_SINGLE_INSTANCE Wnode;
    ULONG WnodeSize;
    LPGUID GuidPtr;
    ULONG i;
    PWCHAR InstanceName;
    PMSWmi_GuidRegistrationInfo RegInfo;
    ULONG DataBlockSize;
    GUID RegChangeGuid = MSWmi_GuidRegistrationInfoGuid;
#define REGUPDATENAME L"REGUPDATEINFO"

    PAGED_CODE();

    DataBlockSize = sizeof(MSWmi_GuidRegistrationInfo) +
                    GuidCount*sizeof(GUID) - sizeof(GUID);

    WnodeSize = sizeof(WNODE_SINGLE_INSTANCE) +
                sizeof(USHORT) + sizeof(REGUPDATENAME) + 8 + DataBlockSize;
    
    WnodeBuffer = WmipAlloc(WnodeSize);
    if (WnodeBuffer != NULL)
    {
        Wnode = (PWNODE_SINGLE_INSTANCE)WnodeBuffer;

        //
        // Setup a WNODE_SINGLE_INSTANCE event with the updated guid
        // registration information
        //
        memset(Wnode, 0, sizeof(WNODE_HEADER));
        Wnode->WnodeHeader.Guid = RegChangeGuid;
        Wnode->WnodeHeader.BufferSize = WnodeSize;
        Wnode->WnodeHeader.Flags = WNODE_FLAG_SINGLE_INSTANCE |
                                   WNODE_FLAG_EVENT_ITEM;
        Wnode->OffsetInstanceName = sizeof(WNODE_SINGLE_INSTANCE);
        Wnode->DataBlockOffset = ((Wnode->OffsetInstanceName +
                                   sizeof(USHORT) + sizeof(REGUPDATENAME) + 7) & ~7);
        Wnode->SizeDataBlock = DataBlockSize;

        InstanceName = (PWCHAR)OffsetToPtr(Wnode, Wnode->OffsetInstanceName);
        *InstanceName++ = sizeof(REGUPDATENAME);
        StringCbCopy(InstanceName, sizeof(REGUPDATENAME), REGUPDATENAME);

        RegInfo = (PMSWmi_GuidRegistrationInfo)OffsetToPtr(Wnode,
                                                       Wnode->DataBlockOffset);
        RegInfo->Operation = NotificationType; 
        RegInfo->GuidCount = GuidCount;
        
        GuidPtr = (LPGUID)RegInfo->GuidList;
        for (i = 0; i < GuidCount; i++)
        {
            *GuidPtr++ =  *GuidList[i].Guid;
        }

        WmipProcessEvent((PWNODE_HEADER)Wnode, TRUE, FALSE);

        WmipFree(WnodeBuffer);
    }

}


void WmipGenerateBinaryMofNotification(
    PBINSTANCESET BinaryMofInstanceSet,
    LPCGUID Guid        
    )
{
    PWNODE_SINGLE_INSTANCE Wnode;
    SIZE_T ImagePathLen, ResourceNameLen, InstanceNameLen, BufferSize;
    PWCHAR Ptr;
    ULONG i;
    HRESULT hr;

    PAGED_CODE();
    
    if (BinaryMofInstanceSet->Count == 0)
    {
        return;
    }

    for (i = 0; i < BinaryMofInstanceSet->Count; i++)
    {
        ImagePathLen = sizeof(USHORT);
        InstanceNameLen = (sizeof(USHORT) + 7) & ~7;

        if (BinaryMofInstanceSet->Flags & IS_INSTANCE_STATICNAMES)
        {
            ResourceNameLen = ((wcslen(BinaryMofInstanceSet->IsStaticNames->StaticNamePtr[i])+1) * sizeof(WCHAR)) + sizeof(USHORT);
        } else if (BinaryMofInstanceSet->Flags & IS_INSTANCE_BASENAME) {
            ResourceNameLen = (((wcslen(BinaryMofInstanceSet->IsBaseName->BaseName) +
                             MAXBASENAMESUFFIXLENGTH) * sizeof(WCHAR)) + sizeof(USHORT));
        } else {
            return;
        }

        BufferSize = FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData) +
                      InstanceNameLen +
                      ImagePathLen +
                      ResourceNameLen;

        Wnode = (PWNODE_SINGLE_INSTANCE)WmipAlloc(BufferSize);
        if (Wnode != NULL)
        {
            Wnode->WnodeHeader.BufferSize = (ULONG) BufferSize;
            Wnode->WnodeHeader.ProviderId = MOFEVENT_ACTION_BINARY_MOF;
            Wnode->WnodeHeader.Version = 1;
            Wnode->WnodeHeader.Linkage = 0;
            Wnode->WnodeHeader.Flags = (WNODE_FLAG_EVENT_ITEM |
                                        WNODE_FLAG_SINGLE_INSTANCE);
            memcpy(&Wnode->WnodeHeader.Guid,
                   Guid,
                   sizeof(GUID));
            WmiInsertTimestamp(&Wnode->WnodeHeader);
            Wnode->OffsetInstanceName = FIELD_OFFSET(WNODE_SINGLE_INSTANCE,
                                                 VariableData);
            Wnode->DataBlockOffset = (ULONG)(Wnode->OffsetInstanceName + 
                                      InstanceNameLen);
            Wnode->SizeDataBlock = (ULONG)(ImagePathLen + ResourceNameLen);
            Ptr = (PWCHAR)&Wnode->VariableData;

            *Ptr++ = 0;              // Empty instance name
            
            Ptr = (PWCHAR)OffsetToPtr(Wnode, Wnode->DataBlockOffset);
            *Ptr++ = 0;              // Empty image path

            // Instance name for binary mof resource
            ResourceNameLen -= sizeof(USHORT);
            if (BinaryMofInstanceSet->Flags & IS_INSTANCE_STATICNAMES)
            {
                *Ptr++ = (USHORT)ResourceNameLen;
                hr = StringCbCopy(Ptr,
                             ResourceNameLen,
                             BinaryMofInstanceSet->IsStaticNames->StaticNamePtr[i]);
                WmipAssert(hr == S_OK);
            } else if (BinaryMofInstanceSet->Flags & IS_INSTANCE_BASENAME) {
                hr = (USHORT)StringCbPrintfEx(Ptr+1,
                                                ResourceNameLen,
                                                NULL,
                                                NULL,
                                                STRSAFE_FILL_BEHIND_NULL,
                                                L"%ws%d",
                                                BinaryMofInstanceSet->IsBaseName->BaseName,
                                                BinaryMofInstanceSet->IsBaseName->BaseIndex+i) * sizeof(WCHAR);
                WmipAssert(hr == S_OK);
                *Ptr = (USHORT)ResourceNameLen;
            }

            WmipProcessEvent((PWNODE_HEADER)Wnode, TRUE, FALSE);
            WmipFree(Wnode);
        }
    }
}

void WmipGenerateMofResourceNotification(
    LPWSTR ImagePath,
    LPWSTR ResourceName,
    LPCGUID Guid,
    ULONG ActionCode
    )
{
    PWNODE_SINGLE_INSTANCE Wnode;
    SIZE_T ImagePathLen, ResourceNameLen, InstanceNameLen, BufferSize;
    PWCHAR Ptr;

    PAGED_CODE();

    ImagePathLen = (wcslen(ImagePath) + 2) * sizeof(WCHAR);

    ResourceNameLen = (wcslen(ResourceName) + 2) * sizeof(WCHAR);
    InstanceNameLen = ( sizeof(USHORT)+7 ) & ~7;
    BufferSize = FIELD_OFFSET(WNODE_SINGLE_INSTANCE, VariableData) +
                      InstanceNameLen +
                      ImagePathLen +
                      ResourceNameLen;

    Wnode = (PWNODE_SINGLE_INSTANCE)WmipAlloc(BufferSize);
    if (Wnode != NULL)
    {
        Wnode->WnodeHeader.BufferSize = (ULONG) BufferSize;
        Wnode->WnodeHeader.ProviderId = ActionCode;
        Wnode->WnodeHeader.Version = 1;
        Wnode->WnodeHeader.Linkage = 0;
        Wnode->WnodeHeader.Flags = (WNODE_FLAG_EVENT_ITEM |
                                    WNODE_FLAG_SINGLE_INSTANCE |
                                    WNODE_FLAG_INTERNAL);
        memcpy(&Wnode->WnodeHeader.Guid,
               Guid,
               sizeof(GUID));
        WmiInsertTimestamp(&Wnode->WnodeHeader);
        Wnode->OffsetInstanceName = FIELD_OFFSET(WNODE_SINGLE_INSTANCE,
                                                 VariableData);
        Wnode->DataBlockOffset = (ULONG)(Wnode->OffsetInstanceName + InstanceNameLen);
        Wnode->SizeDataBlock = (ULONG)(ImagePathLen + ResourceNameLen);
        Ptr = (PWCHAR)&Wnode->VariableData;

        *Ptr = 0;              // Empty instance name

                                 // ImagePath name
        Ptr = (PWCHAR)OffsetToPtr(Wnode, Wnode->DataBlockOffset);
        ImagePathLen -= sizeof(USHORT);
        *Ptr++ = (USHORT)ImagePathLen;
        memcpy(Ptr, ImagePath, ImagePathLen);
        Ptr += (ImagePathLen / sizeof(WCHAR));

                                 // MofResource Name
        ResourceNameLen -= sizeof(USHORT);
        *Ptr++ = (USHORT)ResourceNameLen;
        memcpy(Ptr, ResourceName, ResourceNameLen);

        WmipProcessEvent((PWNODE_HEADER)Wnode, TRUE, FALSE);
        WmipFree(Wnode);
    }
}

void WmipGenerateRegistrationNotification(
    PBDATASOURCE DataSource,
    NOTIFICATIONTYPES NotificationType
    )
{
    PTRCACHE *Guids;
    ULONG GuidCount, GuidMax;
    PLIST_ENTRY InstanceSetList;
    PBINSTANCESET InstanceSet;
    LPGUID Guid;

    PAGED_CODE();
    
    WmipReferenceDS(DataSource);

    //
    // Loop over all instance sets for this data source
    //
    GuidCount = 0;
    GuidMax = 0;
    Guids = NULL;
    InstanceSetList =  DataSource->ISHead.Flink;
    while (InstanceSetList != &DataSource->ISHead)
    {

        InstanceSet = CONTAINING_RECORD(InstanceSetList,
                                        INSTANCESET,
                                        DSISList);

        //
        // Cache the guid and instance set so we can send registration
        // change notifications
        //
        Guid = &InstanceSet->GuidEntry->Guid;
        WmipCachePtrs(Guid,
                      InstanceSet,
                      &GuidCount,
                      &GuidMax,
                      &Guids);

        //
        // If we are adding a guid and it is already enabled then we
        // need to send an enable irp. Likewise if the guid is being
        // removed and is enabled then we need to send a disable
        //
        if (NotificationType == RegistrationAdd)
        {
            WmipEnableCollectionForNewGuid(Guid, InstanceSet);
        } else if (NotificationType == RegistrationDelete) {
            WmipDisableCollectionForRemovedGuid(Guid, InstanceSet);
        }

        InstanceSetList = InstanceSetList->Flink;
    }

    //
    // Send out event that informs about guid registration changes
    //
    WmipSendGuidUpdateNotifications(NotificationType,
                                    GuidCount,
                                    Guids);

    if (Guids != NULL)
    {
        WmipFree(Guids);
    }
    
    WmipUnreferenceDS(DataSource);
}

NTSTATUS WmipAddMofResource(
    PBDATASOURCE DataSource,
    LPWSTR ImagePath,
    BOOLEAN IsImagePath,
    LPWSTR MofResourceName,
    PBOOLEAN NewMofResource
    )
/*++

Routine Description:

    This routine will build MOFCLASSINFO structures for each guid that is 
    described in the MOF for the data source. If there are any errors in the
    mof resource then no mof information from the resource is retained and the
    resource data is unloaded. 

Arguments:

    DataSource is the data source structure of the data provider
        
    ImagePath points at a string that has the full path to the image
        file that contains the MOF resource
            
    MofResourceName points at a string that has the name of the MOF
        resource
        
Return Value:


--*/        
{
    PMOFRESOURCE MofResource;
    ULONG NewMofResourceCount;
    ULONG i;
    BOOLEAN FreeBuffer;
    size_t RegPathLen, ResNameLen;
    HRESULT hr;

    PAGED_CODE();
    
    MofResource = WmipFindMRByNames(ImagePath, 
                                    MofResourceName);
                     
    if (MofResource == NULL)
    {
        //
        // Mof Resource not previously specified, so allocate a new one
        MofResource = WmipAllocMofResource();
        if (MofResource == NULL)
        {    
            return(STATUS_INSUFFICIENT_RESOURCES);
        }

        if (IsImagePath)
        {
            MofResource->Flags |= MR_FLAG_USER_MODE;
        }

        RegPathLen = (wcslen(ImagePath)+1) * sizeof(WCHAR);
        MofResource->RegistryPath = WmipAlloc(RegPathLen);
        ResNameLen = (wcslen(MofResourceName) + 1) * sizeof(WCHAR);
        MofResource->MofResourceName = WmipAlloc(ResNameLen);

        if ((MofResource->RegistryPath == NULL) || 
            (MofResource->MofResourceName == NULL))
        {
            //
            // Allocation cleanup routine will free any memory allocated for MR
            WmipUnreferenceMR(MofResource);
            return(STATUS_INSUFFICIENT_RESOURCES);
        }
    
        hr = StringCbCopy(MofResource->RegistryPath, RegPathLen, ImagePath);
        WmipAssert(hr == S_OK);
        hr = StringCbCopy(MofResource->MofResourceName, ResNameLen, MofResourceName);
        WmipAssert(hr == S_OK);

        WmipEnterSMCritSection();
        InsertTailList(WmipMRHeadPtr, &MofResource->MainMRList);
        WmipLeaveSMCritSection();
        *NewMofResource = TRUE;
    } else {
        *NewMofResource = FALSE;
    }
    
    if (DataSource != NULL)
    {
        WmipEnterSMCritSection();
        for (i = 0; i < DataSource->MofResourceCount; i++)
        {
            if (DataSource->MofResources[i] == MofResource)
            {
                //
                // If this mof resource is already been registered for
                // this data source then we do not need to worry about
                // it anymore.
                //
                WmipUnreferenceMR(MofResource);
                break;
            }
            
            if (DataSource->MofResources[i] == NULL)
            {
                DataSource->MofResources[i] = MofResource;
                break;
            }
        }
            
        if (i == DataSource->MofResourceCount)
        {
            NewMofResourceCount = DataSource->MofResourceCount + 
                                  AVGMOFRESOURCECOUNT;
            if (DataSource->MofResources != 
                     DataSource->StaticMofResources)
            {
                FreeBuffer = TRUE;
            } else {
                FreeBuffer = FALSE;
            }
        
            if (WmipRealloc((PVOID *)&DataSource->MofResources,
                         DataSource->MofResourceCount * sizeof(PMOFRESOURCE),
                         NewMofResourceCount * sizeof(PMOFRESOURCE),
                         FreeBuffer )  )
            {
                DataSource->MofResourceCount = NewMofResourceCount;
                DataSource->MofResources[i] = MofResource;
            }
        }
        WmipLeaveSMCritSection();
    }

    return(STATUS_SUCCESS);
}


NTSTATUS WmipAddDataSource(
    IN PREGENTRY RegEntry,
    IN PWMIREGINFOW WmiRegInfo,
    IN ULONG BufferSize,
    IN PWCHAR RegPath,
    IN PWCHAR ResourceName,
    IN PWMIGUIDOBJECT RequestObject,
    IN BOOLEAN IsUserMode
    )
/*+++

Routine Description:

    This routine will register a information in the WMI database for a 
    new DataSource or add additional guids to an existing data source.
        
Arguments:

    RegEntry is the regentry for the data provider
        
    WmiRegInfo is the registration information to register
        
    BufferSize is the size of WmiRegInfo in bytes
        
    RegPath is a pointer into WmiRegInfo to a counted string that is the 
        registry path (or image path for UM providers).
            
    ResourceName is a pointer into WmiRegInfo to a counted string that is the 
        resource name
            
    RequestObject is the request object associated with the UM provider.
        If this is NULL then the registration is for a driver
                        

Return Value:

    STATUS_SUCCESS or an error code

---*/
{
    PBDATASOURCE DataSource;
    PWMIREGGUID RegGuid;
    ULONG i;
    NTSTATUS Status, Status2;
    PBINSTANCESET InstanceSet;
    PBINSTANCESET BinaryMofInstanceSet = NULL;
    PWCHAR MofRegistryPath;
    PWCHAR MofResourceName;
    BOOLEAN AppendToDS;
    BOOLEAN NewMofResource;

    PAGED_CODE();    
    
    if (RegEntry->DataSource != NULL)
    {
        DataSource = RegEntry->DataSource;
        WmipAssert(DataSource != NULL);
        AppendToDS = TRUE;
    } else {
        DataSource = WmipAllocDataSource();
        AppendToDS = FALSE;
    }
    
    if (DataSource != NULL)
    {
        //
        // Loop over each guid being registered and build instance sets and
        // guid entries.
        //
        if (! AppendToDS)
        {
            DataSource->ProviderId = RegEntry->ProviderId;
            if (RequestObject != NULL)
            {
                DataSource->Flags |= DS_USER_MODE;
                DataSource->RequestObject = RequestObject;
            } else {
                DataSource->Flags |= DS_KERNEL_MODE;
            }
        
        }
    
        RegGuid = WmiRegInfo->WmiRegGuid;


        for (i = 0; i < WmiRegInfo->GuidCount; i++, RegGuid++)
        {
            if (! (RegGuid->Flags & WMIREG_FLAG_REMOVE_GUID))
            {

                //
                // Only trace control guids are registered. Trace transaction
                // guids will not be registered since they can not be enabled or
                // disabled individually. They will be kept on the ControlGuids'
                // instance set structure. 
                //

                if ( ( (RegGuid->Flags & WMIREG_FLAG_TRACED_GUID) != WMIREG_FLAG_TRACED_GUID ) || 
                       (RegGuid->Flags & WMIREG_FLAG_TRACE_CONTROL_GUID) )
                { 

                    //
                    // Allocate an instance set for this new set of instances
                    //
                    InstanceSet = WmipAllocInstanceSet();
                    if (InstanceSet == NULL)
                    {
                        WmipUnreferenceDS(DataSource);
                        WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: WmipAddDataSource: WmipAllocInstanceSet failed\n"));
                        return(STATUS_INSUFFICIENT_RESOURCES);
                    }

                    //
                    // We will allocate a proper guid entry for the instance 
                    // set when the data source gets linked into the main data 
                    // source list so we stash a pointer to the guid away now.
                    //
                    InstanceSet->GuidEntry = (PBGUIDENTRY)&RegGuid->Guid;

                    //
                    // Minimally initialize the InstanceSet and add it to 
                    // the DataSource's list of InstanceSets. We do this 
                    // first so that if there is any failure below and 
                    // the DataSource can'e be fully registered the instance 
                    // set and guid entry will be free when the DataSource is
                    // freed.
                    //
                    InstanceSet->DataSource = DataSource;
                    InstanceSet->Flags |= IS_NEWLY_REGISTERED;

                    Status = WmipBuildInstanceSet(RegGuid,
                                                  WmiRegInfo,
                                                  BufferSize,
                                                  InstanceSet,
                                                  RegEntry->ProviderId,
                                                  RegPath);

                    //
                    // If this is the guid that represents the binary mof data
                    // then remember the InstanceSet for  later
                    //
                    if (IsEqualGUID(&RegGuid->Guid, &WmipBinaryMofGuid))
                    {
                        BinaryMofInstanceSet = InstanceSet;
                    }


                    InsertHeadList(&DataSource->ISHead, &InstanceSet->DSISList);
  
                    if (! NT_SUCCESS(Status))
                    {
                        WmipUnreferenceDS(DataSource);
                        return(Status);
                    }
                }
            }
        }
        
        //
        // Now that the instance sets have been built successfully we 
        // can link them into the master list.
        //                        
        WmipEnterSMCritSection();
        Status = WmipLinkDataSourceToList(DataSource, (BOOLEAN)(! AppendToDS));
        WmipLeaveSMCritSection();

        if (! NT_SUCCESS(Status))
        {
            WmipUnreferenceDS(DataSource);
            return(Status);
        }
        
        RegEntry->DataSource = DataSource;
        
        //
        // We need to send out notification of new guids and mofs.
        //
        if (BinaryMofInstanceSet != NULL)
        {
            //
            // Send binary mof guid arrival notification
            //
            WmipGenerateBinaryMofNotification(BinaryMofInstanceSet,
                                      &GUID_MOF_RESOURCE_ADDED_NOTIFICATION);

        }

        //
        // Convert Registry path to a sz string so we can assign it to
        // the DS if the DS is a new one
        //
        if (RegPath != NULL)
        {
            MofRegistryPath = WmipCountedToSz(RegPath);
        } else {
            MofRegistryPath = NULL;
        }
        
        if ((AppendToDS == FALSE) && (MofRegistryPath != NULL))
        {
            DataSource->RegistryPath = MofRegistryPath;
        }
        
        if (ResourceName != NULL)
        {
            MofResourceName = WmipCountedToSz(ResourceName);        
        } else {
            MofResourceName = NULL;
        }
        
        //
        // Finally if we created a new data source we need to register
        // the mof for it. Only register those that have a RegistryPath
        // and a ResourceName
        //
        if ((MofRegistryPath != NULL) &&
            (*MofRegistryPath != 0) &&
            (MofResourceName != NULL) &&
            (*MofResourceName != 0))
        {
            //
            // If a mof is specified then add it to the list
            //
            Status2 = WmipAddMofResource(DataSource,
                                        MofRegistryPath,
                                        IsUserMode,
                                        MofResourceName, 
                                        &NewMofResource);
                                    
            if (NT_SUCCESS(Status2) && NewMofResource)
            {
                //
                // We successfully added a brand new MOF resource so
                // we need to fire an event for wbem.
                //
                WmipGenerateMofResourceNotification(MofRegistryPath,
                                                    MofResourceName,
                                      &GUID_MOF_RESOURCE_ADDED_NOTIFICATION,
                                      IsUserMode ?
                                             MOFEVENT_ACTION_IMAGE_PATH :
                                             MOFEVENT_ACTION_REGISTRY_PATH);
            }            
        }        
        
        //
        // Clean up registry path and mof resource name strings
        //
        if ((MofRegistryPath != NULL) && AppendToDS)
        {
            //
            // Only free if registry path not saved in DataSource
            //
            WmipAssert(MofRegistryPath != DataSource->RegistryPath);
            WmipFree(MofRegistryPath);
        }
        
        if (MofResourceName != NULL)
        {
            WmipFree(MofResourceName);
        }
        
        //
        // Send a notification about new/changed guids
        //
        WmipGenerateRegistrationNotification(DataSource,
                                             RegistrationAdd);
        
    } else {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    
    return(Status);
}


PBINSTANCESET WmipFindISInDSByGuid(
    PBDATASOURCE DataSource,
    LPGUID Guid
    )
/*++

Routine Description:

    This routine will find the InstanceSet in the passed DataSource for the
    guid passed.

    This routine assumes that the SM critical section is held before it is
    called.

Arguments:

    DataSource is the data source from which the guid is to be removed

    Guid has the Guid for the InstanceSet to find

Return Value:

--*/
{
    PLIST_ENTRY InstanceSetList;
    PBINSTANCESET InstanceSet;

    PAGED_CODE();
    
    InstanceSetList = DataSource->ISHead.Flink;
    while (InstanceSetList != &DataSource->ISHead)
    {
        InstanceSet = CONTAINING_RECORD(InstanceSetList,
                                        INSTANCESET,
                                        DSISList);

        if ((InstanceSet->GuidEntry != NULL) &&
             (IsEqualGUID(Guid, &InstanceSet->GuidEntry->Guid)))
        {
            WmipReferenceIS(InstanceSet);
            return(InstanceSet);
        }

        InstanceSetList = InstanceSetList->Flink;
    }
    return(NULL);
}

ULONG WmipUpdateAddGuid(
    PBDATASOURCE DataSource,
    PWMIREGGUID RegGuid,
    PWMIREGINFO WmiRegInfo,
    ULONG BufferSize,
    PBINSTANCESET *AddModInstanceSet
    )
/*++

Routine Description:

    This routine will add a new guid for the data source and send notification

    This routine assumes that the SM critical section is held before it is
    called.

Arguments:

    DataSource is the data source from which the guid is to be removed

    RegGuid has the Guid update data structure

    WmiRegInfo points at the beginning of the registration update info

Return Value:

    1 if guid was added or 0

--*/
{
    PBINSTANCESET InstanceSet;
    LPGUID Guid = &RegGuid->Guid;
    NTSTATUS Status;

    PAGED_CODE();
    
    //
    // Allocate an instance set for this new set of instances
    InstanceSet = WmipAllocInstanceSet();
    if (InstanceSet == NULL)
    {
        WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: WmipUpdateAddGuid: WmipAllocInstanceSet failed\n"));
        return(0);
    }

    //
    // We will allocate a proper guid entry for the instance set when
    // the data source gets linked into the main data source list so
    // we stash a pointer to the guid away now.
    InstanceSet->GuidEntry = (PBGUIDENTRY)Guid;

    //
    // Minimally initialize the InstanceSet and add it to the DataSource's
    // list of InstanceSets. We do this first so that if there is any
    // failure below and the DataSource can'e be fully registered the
    // instance set and guid entry will be free when the DataSource is
    // freed.
    InstanceSet->DataSource = DataSource;
    InstanceSet->Flags |= IS_NEWLY_REGISTERED;

    InsertHeadList(&DataSource->ISHead, &InstanceSet->DSISList);

    Status = WmipBuildInstanceSet(RegGuid,
                                  WmiRegInfo,
                                  BufferSize,
                                  InstanceSet,
                                  DataSource->ProviderId,
                                  DataSource->RegistryPath);

    if (! NT_SUCCESS(Status))
    {
        WmipUnreferenceIS(InstanceSet);
        return(0);
    }

    Status = WmipLinkDataSourceToList(DataSource,
                                          FALSE);

    *AddModInstanceSet = InstanceSet;

    return( NT_SUCCESS(Status) ? 1 : 0);
}

#if DBG
PWCHAR GuidToString(
    PWCHAR s,
    ULONG SizeInBytes,
    LPGUID piid
    )
{
    HRESULT hr;
    
    PAGED_CODE();
    
    hr = StringCbPrintf(s, SizeInBytes, L"%x-%x-%x-%x%x%x%x%x%x%x%x",                   
               piid->Data1, piid->Data2,
               piid->Data3,
               piid->Data4[0], piid->Data4[1],
               piid->Data4[2], piid->Data4[3],
               piid->Data4[4], piid->Data4[5],
               piid->Data4[6], piid->Data4[7]);
    WmipAssert(hr == S_OK);

    return(s);
}
#endif


BOOLEAN WmipUpdateRemoveGuid(
    PBDATASOURCE DataSource,
    PWMIREGGUID RegGuid,
    PBINSTANCESET *AddModInstanceSet
    )
/*++

Routine Description:

    This routine will remove the guid for the data source and send notification

    This routine assumes that the SM critical section is held before it is
    called.

Arguments:

    DataSource is the data source from which the guid is to be removed

    RegGuid has the Guid update data structure

Return Value:

    TRUE if guid was removed else FALSE

--*/
{
    PBINSTANCESET InstanceSet;
    LPGUID Guid = &RegGuid->Guid;
    BOOLEAN SendNotification;

    PAGED_CODE();
    
    InstanceSet = WmipFindISInDSByGuid(DataSource,
                                       Guid);
    if (InstanceSet != NULL)
    {
        WmipUnreferenceIS(InstanceSet);
        *AddModInstanceSet = InstanceSet;
        SendNotification = TRUE;
    } else {
#if DBG
        WCHAR s[256];
        WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: UpdateRemoveGuid %ws not registered by %ws\n",
                        GuidToString(s, sizeof(s), Guid), DataSource->RegistryPath));
#endif
        SendNotification = FALSE;
    }
    return(SendNotification);
}


BOOLEAN WmipIsEqualInstanceSets(
    PBINSTANCESET InstanceSet1,
    PBINSTANCESET InstanceSet2
    )
{
    ULONG i;
    ULONG Flags1, Flags2;

    PAGED_CODE();
    
    Flags1 = InstanceSet1->Flags & ~(IS_ENABLE_EVENT | IS_ENABLE_COLLECTION);
    Flags2 = InstanceSet2->Flags & ~(IS_ENABLE_EVENT | IS_ENABLE_COLLECTION);
    if (Flags1 == Flags2)
    {
        if (InstanceSet1->Flags & IS_INSTANCE_BASENAME)
        {
            if ((InstanceSet1->Count == InstanceSet2->Count) &&
                (wcscmp(InstanceSet1->IsBaseName->BaseName,
                        InstanceSet1->IsBaseName->BaseName) == 0))
            {
                return(TRUE);
            }
        } else if (InstanceSet1->Flags & IS_INSTANCE_STATICNAMES) {
            if (InstanceSet1->Count == InstanceSet2->Count)
            {
                for (i = 0; i < InstanceSet1->Count; i++)
                {
                    if (wcscmp(InstanceSet1->IsStaticNames->StaticNamePtr[i],
                               InstanceSet2->IsStaticNames->StaticNamePtr[i]) != 0)
                     {
                        return(FALSE);
                    }
                }
                return(TRUE);
            }
        } else {
            return(TRUE);
        }
    }

    return(FALSE);

}

ULONG WmipUpdateModifyGuid(
    PBDATASOURCE DataSource,
    PWMIREGGUID RegGuid,
    PWMIREGINFO WmiRegInfo,
    ULONG BufferSize,
    PBINSTANCESET *AddModInstanceSet
    )
/*++

Routine Description:

    This routine will modify an existing guid for the data source and
    send notification

    This routine assumes that the SM critical section is held before it is
    called.


    If a guid was opened when it was registered as cheap, but closed
    when the guid was registered expensive a disable collection will
    NOT be sent. Conversely if a guid was opened when it was
    registered as expensive and is closed when registered as cheap a
    disable collection may be sent.

Arguments:

    DataSource is the data source from which the guid is to be removed

    RegGuid has the Guid update data structure

    WmiRegInfo points at the beginning of the registration update info

Return Value:

    1 if guid was added or 2 if guid was modified else 0

--*/
{
    PBINSTANCESET InstanceSet;
    LPGUID Guid = &RegGuid->Guid;
    ULONG SendNotification;
    PBINSTANCESET InstanceSetNew;
    PVOID ToFree;
    NTSTATUS Status;

    PAGED_CODE();
    
    InstanceSet = WmipFindISInDSByGuid(DataSource,
                                       Guid);
    if (InstanceSet != NULL)
    {
        //
        // See if anything has changed with the instance names and if not
        // then don't bother to recreate the instance set

        InstanceSetNew = WmipAllocInstanceSet();
        if (InstanceSetNew == NULL)
        {
            WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: UpdateModifyGuid Not enough memory to alloc InstanceSet\n"));
            WmipUnreferenceIS(InstanceSet);
            return(0);
        }
    
        Status = WmipBuildInstanceSet(RegGuid,
                             WmiRegInfo,
                             BufferSize,
                             InstanceSetNew,
                             DataSource->ProviderId,
                             DataSource->RegistryPath);
                         
        if (NT_SUCCESS(Status))
        {
            if (! WmipIsEqualInstanceSets(InstanceSet,
                                          InstanceSetNew))
            {
                ToFree = NULL;
                if (InstanceSet->IsBaseName != NULL) {
                    ToFree = (PVOID)InstanceSet->IsBaseName;
                }

                RemoveEntryList(&InstanceSet->GuidISList);
                Status = WmipBuildInstanceSet(RegGuid,
                             WmiRegInfo,
                             BufferSize,
                             InstanceSet,
                             DataSource->ProviderId,
                             DataSource->RegistryPath);
                if (NT_SUCCESS(Status))
                {
                    InsertHeadList(&InstanceSet->GuidEntry->ISHead,
                               &InstanceSet->GuidISList);
                } else {
                    //
                    // It is sad, but we weren't able to rebuild the instance
                    // set so the old one is gone. This is an unlikely
                    // situation that can really only occur when the machine
                    // is out of memory.
                    //
                }

                if (ToFree != NULL)
                {
                    WmipFree(ToFree);
                }

                *AddModInstanceSet = InstanceSet;
                SendNotification = 2;
            } else {
                //
                // The InstanceSets are identical so just delete the new one
                SendNotification = 0;
            }
            
            WmipUnreferenceIS(InstanceSetNew);
            WmipUnreferenceIS(InstanceSet);
        } else {
            //
            // We could not parse the new instance set so leave the old
            // one alone
            //
            WmipUnreferenceIS(InstanceSet);
            WmipUnreferenceIS(InstanceSetNew);
            SendNotification = FALSE;
        }
    } else {
        //
        // Guid not already registered so try to add it
        SendNotification = WmipUpdateAddGuid(DataSource,
                          RegGuid,
                          WmiRegInfo,
                          BufferSize,
                          AddModInstanceSet);
    }
    return(SendNotification);
}


void WmipCachePtrs(
    LPGUID Ptr1,
    PBINSTANCESET Ptr2,
    ULONG *PtrCount,
    ULONG *PtrMax,
    PTRCACHE **PtrArray
    )
{
    PTRCACHE *NewPtrArray;
    PTRCACHE *OldPtrArray;
    PTRCACHE *ActualPtrArray;

    PAGED_CODE();
    
    if (*PtrCount == *PtrMax)
    {
        NewPtrArray = WmipAlloc((*PtrMax + PTRCACHEGROWSIZE) * sizeof(PTRCACHE));
        if (NewPtrArray != NULL)
        {
            OldPtrArray = *PtrArray;
            memcpy(NewPtrArray, OldPtrArray, *PtrMax * sizeof(PTRCACHE));
            *PtrMax += PTRCACHEGROWSIZE;
            if (*PtrArray != NULL)
            {
                WmipFree(*PtrArray);
            }
            *PtrArray = NewPtrArray;
        } else {
            WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: Couldn't alloc memory for pointer cache\n"));
            return;
        }
    }
    ActualPtrArray = *PtrArray;
    ActualPtrArray[*PtrCount].Guid = Ptr1;
    ActualPtrArray[*PtrCount].InstanceSet = Ptr2;
    (*PtrCount)++;
}



NTSTATUS WmipUpdateDataSource(
    PREGENTRY RegEntry,
    PWMIREGINFOW WmiRegInfo,
    ULONG RetSize
    )
/*++

Routine Description:

    This routine will update a data source with changes to already registered
    guids.

Arguments:

    ProviderId is the provider id of the DataSource whose guids are being
        updated.

    WmiRegInfo has the registration update information

    RetSize has the size of the registration information returned from
        kernel mode.

Return Value:


--*/
{
    PBDATASOURCE DataSource;
    PUCHAR RegInfo;
    ULONG RetSizeLeft;
    ULONG i;
    PWMIREGGUID RegGuid;
    ULONG NextWmiRegInfo;
    PTRCACHE *RemovedGuids;
    PTRCACHE *AddedGuids;
    PTRCACHE *ModifiedGuids;
    ULONG RemovedGuidCount;
    ULONG AddedGuidCount;
    ULONG ModifiedGuidCount;
    ULONG RemovedGuidMax;
    ULONG AddedGuidMax;
    ULONG ModifiedGuidMax;
    PBINSTANCESET InstanceSet;
    ULONG Action;

    PAGED_CODE();
    
    DataSource = RegEntry->DataSource;
    if (DataSource == NULL)
    {
        WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: RegEntry %p requested update but is not registered\n",
                       RegEntry));
        return(STATUS_OBJECT_NAME_NOT_FOUND);
    }

    WmipReferenceDS(DataSource);
    AddedGuidCount = 0;
    ModifiedGuidCount = 0;
    RemovedGuidCount = 0;
    AddedGuidMax = 0;
    ModifiedGuidMax = 0;
    RemovedGuidMax = 0;
    ModifiedGuids = NULL;
    AddedGuids = NULL;
    RemovedGuids = NULL;

    NextWmiRegInfo = 0;
    RetSizeLeft = RetSize;
    WmipEnterSMCritSection();
    RegInfo = (PUCHAR)WmiRegInfo;
    for (i = 0; i < WmiRegInfo->GuidCount; i++)
    {
        RegGuid = &WmiRegInfo->WmiRegGuid[i];
        if (RegGuid->Flags & WMIREG_FLAG_REMOVE_GUID)
        {
            if (WmipUpdateRemoveGuid(DataSource,
                                         RegGuid,
                                         &InstanceSet))
            {
                WmipCachePtrs(&RegGuid->Guid,
                             InstanceSet,
                             &RemovedGuidCount,
                             &RemovedGuidMax,
                             &RemovedGuids);
            }
        } else  {
            Action = WmipUpdateModifyGuid(DataSource,
                                       RegGuid,
                                       WmiRegInfo,
                                       RetSize,
                                       &InstanceSet);
            if (Action == 1)
            {
                WmipCachePtrs(&RegGuid->Guid,
                                 InstanceSet,
                                 &AddedGuidCount,
                                 &AddedGuidMax,
                                 &AddedGuids);

            } else if (Action == 2) {
                WmipCachePtrs(&RegGuid->Guid,
                                 InstanceSet,
                                 &ModifiedGuidCount,
                                 &ModifiedGuidMax,
                                 &ModifiedGuids);
            }
        }
    }
    WmipLeaveSMCritSection();

    WmipUnreferenceDS(DataSource);

    if (RemovedGuidCount > 0)
    {
        for (i = 0; i < RemovedGuidCount; i++)
        {
            if (IsEqualGUID(RemovedGuids[i].Guid,
                            &WmipBinaryMofGuid))
            {
                WmipGenerateBinaryMofNotification(RemovedGuids[i].InstanceSet,
                                     &GUID_MOF_RESOURCE_REMOVED_NOTIFICATION);
            }
                
            InstanceSet = RemovedGuids[i].InstanceSet;

            WmipDisableCollectionForRemovedGuid(RemovedGuids[i].Guid,
                                                InstanceSet);

            WmipEnterSMCritSection();
            //
            // If IS is on the GE list then remove it
            if (InstanceSet->GuidISList.Flink != NULL)
            {
                RemoveEntryList(&InstanceSet->GuidISList);
                InstanceSet->GuidEntry->ISCount--;
            }

            if (! (InstanceSet->Flags & IS_NEWLY_REGISTERED))
            {
                WmipUnreferenceGE(InstanceSet->GuidEntry);
            }

            InstanceSet->GuidEntry = NULL;

            //
            // Remove IS from the DS List
            RemoveEntryList(&InstanceSet->DSISList);
            WmipUnreferenceIS(InstanceSet);
            WmipLeaveSMCritSection();
        }

        WmipSendGuidUpdateNotifications(RegistrationDelete,
                                    RemovedGuidCount,
                                    RemovedGuids);
        WmipFree(RemovedGuids);
    }

    if (ModifiedGuidCount > 0)
    {
        for (i = 0; i < ModifiedGuidCount; i++)
        {
            if (IsEqualGUID(ModifiedGuids[i].Guid,
                            &WmipBinaryMofGuid))
            {
                WmipGenerateBinaryMofNotification(ModifiedGuids[i].InstanceSet,
                                      &GUID_MOF_RESOURCE_ADDED_NOTIFICATION);
            }
        }
        
        WmipSendGuidUpdateNotifications(RegistrationUpdate,
                                    ModifiedGuidCount,
                                    ModifiedGuids);
        WmipFree(ModifiedGuids);
    }

    if (AddedGuidCount > 0)
    {
        for (i = 0; i < AddedGuidCount; i++)
        {
            if (IsEqualGUID(AddedGuids[i].Guid,
                            &WmipBinaryMofGuid))
            {
                WmipGenerateBinaryMofNotification(AddedGuids[i].InstanceSet,
                                      &GUID_MOF_RESOURCE_ADDED_NOTIFICATION);
            }
                
            WmipEnableCollectionForNewGuid(AddedGuids[i].Guid,
                                           AddedGuids[i].InstanceSet);
        }
        WmipSendGuidUpdateNotifications(RegistrationAdd,
                                    AddedGuidCount,
                                    AddedGuids);
        WmipFree(AddedGuids);
    }
    return(STATUS_SUCCESS);
}

void WmipRemoveDataSourceByDS(
    PBDATASOURCE DataSource
    )
{    

    PAGED_CODE();
    
    WmipGenerateRegistrationNotification(DataSource,
                                         RegistrationDelete);

    WmipUnreferenceDS(DataSource);
}

NTSTATUS WmipRemoveDataSource(
    PREGENTRY RegEntry
    )
{
    PBDATASOURCE DataSource;
    NTSTATUS Status;

    PAGED_CODE();
    
    DataSource = RegEntry->DataSource;
    if (DataSource != NULL)
    {
        WmipReferenceDS(DataSource);
        WmipRemoveDataSourceByDS(DataSource);
        WmipUnreferenceDS(DataSource);
        Status = STATUS_SUCCESS;
    } else {
        WmipDebugPrintEx((DPFLTR_WMICORE_ID, DPFLTR_INFO_LEVEL,"WMI: Attempt to remove non existent data source %p\n",
                        RegEntry));
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return(Status);
}


NTSTATUS WmipEnumerateMofResources(
    PWMIMOFLIST MofList,
    ULONG BufferSize,
    ULONG *RetSize
    )
{
    PLIST_ENTRY MofResourceList;
    PMOFRESOURCE MofResource;
    ULONG MRCount;
    SIZE_T SizeNeeded, MRSize;
    PWMIMOFENTRY MofEntry;
    PWCHAR MRBuffer;
    ULONG i;
    HRESULT hr;

    PAGED_CODE();
    
    WmipEnterSMCritSection();

    MRCount = 0;
    SizeNeeded = 0;
    MofResourceList = WmipMRHeadPtr->Flink;
    while (MofResourceList != WmipMRHeadPtr)
    {
        MofResource = CONTAINING_RECORD(MofResourceList,
                                      MOFRESOURCE,
                                      MainMRList);
                                  
        MRCount++;
        SizeNeeded += (wcslen(MofResource->RegistryPath) + 
                       wcslen(MofResource->MofResourceName) + 2) * 
                           sizeof(WCHAR);
                                  
        MofResourceList = MofResourceList->Flink;
    }
    
    if (MRCount != 0)
    {
        MRSize = sizeof(WMIMOFLIST) + ((MRCount-1) * sizeof(WMIMOFENTRY));
        SizeNeeded += MRSize;
        
        if (BufferSize >= SizeNeeded)
        {
            MofList->MofListCount = MRCount;
            MofResourceList = WmipMRHeadPtr->Flink;
            i = 0;
            while (MofResourceList != WmipMRHeadPtr)
            {
                MofResource = CONTAINING_RECORD(MofResourceList,
                                      MOFRESOURCE,
                                      MainMRList);
                
                MofEntry = &MofList->MofEntry[i++];
                MofEntry->Flags = MofResource->Flags & MR_FLAG_USER_MODE ? 
                                                  WMIMOFENTRY_FLAG_USERMODE : 
                                                  0;
                                                  
                MofEntry->RegPathOffset = (ULONG) MRSize;
                MRBuffer = (PWCHAR)OffsetToPtr(MofList, MRSize);
                hr = StringCbCopy(MRBuffer,
                                  (BufferSize - MRSize),
                                  MofResource->RegistryPath);
                WmipAssert(hr == S_OK);             
                MRSize += (wcslen(MofResource->RegistryPath) + 1) * sizeof(WCHAR);
                
                MofEntry->ResourceOffset = (ULONG) MRSize;
                MRBuffer = (PWCHAR)OffsetToPtr(MofList, MRSize);
                hr = StringCbCopy(MRBuffer,
                                  (BufferSize - MRSize),
                                  MofResource->MofResourceName);
                WmipAssert(hr == S_OK);             
                MRSize += (wcslen(MofResource->MofResourceName) + 1) * sizeof(WCHAR);
                MofResourceList = MofResourceList->Flink;
            }           
        } else {
            //
            // Buffer not large enough, return size needed
            //
            MofList->MofListCount = (ULONG) SizeNeeded;
            *RetSize = sizeof(ULONG);
        }
        
    } else {
        //
        // No mof resources
        //
        MofList->MofListCount = 0;
        *RetSize = sizeof(WMIMOFLIST);
    }
    
    
    WmipLeaveSMCritSection();
    return(STATUS_SUCCESS);
}


NTSTATUS WmipInitializeDataStructs(
    void
)
/*++

Routine Description:

    This routine will do the work of initializing the WMI service

Arguments:

Return Value:

    Error status value
        
--*/
{
    ULONG Status;
    GUID InstanceInfoGuid = INSTANCE_INFO_GUID;
    GUID EnumerateGuidsGuid = ENUMERATE_GUIDS_GUID;
    PREGENTRY RegEntry;
    PDEVICE_OBJECT Callback;
    PLIST_ENTRY GuidEntryList;
    PBGUIDENTRY GuidEntry;
    BOOLEAN NewResource;

    union {
        WMIREGINFOW Info;
        UCHAR Buffer[sizeof(WMIREGINFOW) + (2 * sizeof(WMIREGGUIDW))];
    } WmiReg;

    PAGED_CODE();
    
    //
    // Initialize the various data structure lists that we maintain
    //
    WmipDSHeadPtr = &WmipDSHead;
    InitializeListHead(WmipDSHeadPtr);
    InitializeListHead(&WmipDSChunkInfo.ChunkHead);

    WmipGEHeadPtr = &WmipGEHead;
    InitializeListHead(WmipGEHeadPtr);
    InitializeListHead(&WmipGEChunkInfo.ChunkHead);

    WmipMRHeadPtr = &WmipMRHead;
    InitializeListHead(WmipMRHeadPtr);
    InitializeListHead(&WmipMRChunkInfo.ChunkHead);

    InitializeListHead(&WmipISChunkInfo.ChunkHead);

  
    //      
    // Register any internal data provider guids and mark them as such
    //
    Callback = (PDEVICE_OBJECT)(ULONG_PTR) WmipUMProviderCallback;
    
    //
    // Establish a regentry for the data provider
    //
    RegEntry = WmipAllocRegEntry(Callback,
                                 WMIREG_FLAG_CALLBACK |
                                 REGENTRY_FLAG_TRACED |
                                 REGENTRY_FLAG_NEWREGINFO | 
                                 REGENTRY_FLAG_INUSE |
                                 REGENTRY_FLAG_REG_IN_PROGRESS);
                             
    if (RegEntry != NULL)
    {
        //
        // This code assumes that no other data providers have
        // yet registered.
        //
        WmipAssert(WmipGEHeadPtr->Flink == WmipGEHeadPtr);
                
        RtlZeroMemory(&WmiReg, sizeof(WmiReg));

        WmiReg.Info.BufferSize = sizeof(WmiReg.Buffer);
        WmiReg.Info.GuidCount = 2;
        WmiReg.Info.WmiRegGuid[0].Guid = InstanceInfoGuid;
        WmiReg.Info.WmiRegGuid[1].Guid = EnumerateGuidsGuid;
        
        Status = WmipAddDataSource(RegEntry,
                                   &WmiReg.Info,
                                   WmiReg.Info.BufferSize,
                                   NULL,
                                   NULL,
                                   NULL,
                                   FALSE);
                           
        if (NT_SUCCESS(Status))
        {                          
            GuidEntryList = WmipGEHeadPtr->Flink;
            while (GuidEntryList != WmipGEHeadPtr)
            {   
                GuidEntry = CONTAINING_RECORD(GuidEntryList,
                                              GUIDENTRY,
                                             MainGEList);

                GuidEntry->Flags |= GE_FLAG_INTERNAL;
        
                GuidEntryList = GuidEntryList->Flink;       
            }
        } else {
            RegEntry->Flags |= (REGENTRY_FLAG_RUNDOWN | 
                                    REGENTRY_FLAG_NOT_ACCEPTING_IRPS);
            WmipUnreferenceRegEntry(RegEntry);
        }
        
        Status = WmipAddMofResource(RegEntry->DataSource,
                                    WMICOREIMAGEPATH,
                                    TRUE,
                                    WMICORERESOURCENAME,
                                    &NewResource);
        WmipAssert(NewResource);
    }
    
        
    return(STATUS_SUCCESS);
}

#ifdef ALLOC_DATA_PRAGMA
#pragma const_seg()
#endif

