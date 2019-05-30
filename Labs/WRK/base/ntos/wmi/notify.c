/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    notify.c

Abstract:

    Manages KM to UM notification queue

--*/

#include "wmikmp.h"


void WmipInitializeNotifications(
    void
    );

void WmipEventNotification(
    IN PVOID Context
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT,WmipInitializeNotifications)
#pragma alloc_text(PAGE,WmipEventNotification)
#endif

WORK_QUEUE_ITEM WmipEventWorkQueueItem;
LIST_ENTRY WmipNPEvent = {&WmipNPEvent, &WmipNPEvent};
KSPIN_LOCK WmipNPNotificationSpinlock;
LONG WmipEventWorkItems;
#if DBG
ULONG WmipNPAllocFail;
#endif

void WmipInitializeNotifications(
    void
    )
{
    PAGED_CODE();
    
    ExInitializeWorkItem( &WmipEventWorkQueueItem,
                          WmipEventNotification,
                          NULL );

    KeInitializeSpinLock(&WmipNPNotificationSpinlock);

}

void WmipEventNotification(
    IN PVOID Context
    )
/*++

Routine Description:

    Work item routine to call WmipNotifyUserMode on behalf of an event fired
    by a driver

Arguments:

    Context is not used

Return Value:


--*/
{
    PWNODE_HEADER WnodeEventItem;
    PLIST_ENTRY NotificationPacketList;
	PEVENTWORKCONTEXT EventContext;

    PAGED_CODE();
    
    UNREFERENCED_PARAMETER (Context);

    do
    {
        NotificationPacketList = ExInterlockedRemoveHeadList(
            &WmipNPEvent,
            &WmipNPNotificationSpinlock);

        WmipAssert(NotificationPacketList != NULL);

		EventContext = (PEVENTWORKCONTEXT)
                         CONTAINING_RECORD(NotificationPacketList,
                         EVENTWORKCONTEXT,
                         ListEntry);
		
        WnodeEventItem = EventContext->Wnode;

        //
        // Restore the Wnode->Version from ->ClientContext
        //
        WnodeEventItem->Version = WnodeEventItem->ClientContext;
        WnodeEventItem->ClientContext = 0;
        WnodeEventItem->Linkage = 0;

        WmipProcessEvent(WnodeEventItem,
                         FALSE,
                         TRUE);

        if (EventContext->RegEntry != NULL)
        {
            //
            // Unref for the ref count taken in IoWMIWriteEvent
            //
            WmipUnreferenceRegEntry(EventContext->RegEntry);
        }

		ExFreePool(EventContext);
    } while (InterlockedDecrement(&WmipEventWorkItems));

}

