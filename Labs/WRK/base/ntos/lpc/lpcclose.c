/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    lpcclose.c

Abstract:

    Local Inter-Process Communication close procedures that are called when
    a connection port or a communications port is closed.

--*/

#include "lpcp.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,LpcpClosePort)
#pragma alloc_text(PAGE,LpcpDeletePort)
#pragma alloc_text(PAGE,LpcExitThread)
#endif


VOID
LpcpClosePort (
    IN PEPROCESS Process OPTIONAL,
    IN PVOID Object,
    IN ACCESS_MASK GrantedAccess,
    IN ULONG_PTR ProcessHandleCount,
    IN ULONG_PTR SystemHandleCount
    )

/*++

Routine Description:

    This routine is the callback used for closing a port object.

Arguments:

    Process - Supplies an optional pointer to the process whose port is being
        closed

    Object - Supplies a pointer to the port object being closed

    GrantedAccess - Supplies the access granted to the handle closing port
        object

    ProcessHandleCount - Supplies the number of process handles remaining to
        the object

    SystemHandleCount - Supplies the number of system handles remaining to
        the object

Return Value:

    None.

--*/

{
    //
    //  Translate the object to what it really is, an LPCP port object
    //

    PLPCP_PORT_OBJECT Port = Object;

    UNREFERENCED_PARAMETER (Process);
    UNREFERENCED_PARAMETER (GrantedAccess);
    UNREFERENCED_PARAMETER (ProcessHandleCount);

    //
    //  We only have work to do if the object is a server communication port
    //

    if ( (Port->Flags & PORT_TYPE) == SERVER_CONNECTION_PORT ) {

        //
        //  If this is a server communication port without any system handles
        //  then we can completely destroy the communication queue for the
        //  port
        //

        if ( SystemHandleCount == 0 ) {

            LpcpDestroyPortQueue( Port, TRUE );

        //
        //  If there is only one system handle left then we'll reset the
        //  communication queue for the port
        //

        } else if ( SystemHandleCount == 1 ) {

            LpcpDestroyPortQueue( Port, FALSE );
        }

        //
        //  Otherwise we do nothing
        //
    }

    return;
}


VOID
LpcpDeletePort (
    IN PVOID Object
    )

/*++

Routine Description:

    This routine is the callback used for deleting a port object.

Arguments:

    Object - Supplies a pointer to the port object being deleted

Return Value:

    None.

--*/

{
    PETHREAD CurrentThread;
    PLPCP_PORT_OBJECT Port = Object;
    PLPCP_PORT_OBJECT ConnectionPort;
    LPC_CLIENT_DIED_MSG ClientPortClosedDatagram;
    PLPCP_MESSAGE Msg;
    PLIST_ENTRY Head, Next;
    HANDLE CurrentProcessId;
    NTSTATUS Status;
    LARGE_INTEGER RetryInterval = {(ULONG)(-10 * 1000 * 100), -1}; // 100 milliseconds

    PAGED_CODE();

    CurrentThread = PsGetCurrentThread ();

    //
    //  If the port is a server communication port then make sure that if
    //  there is a dangling client thread that we get rid of it.  This
    //  handles the case of someone calling NtAcceptConnectPort and not
    //  calling NtCompleteConnectPort
    //

    if ((Port->Flags & PORT_TYPE) == SERVER_COMMUNICATION_PORT) {

        PETHREAD ClientThread;

        LpcpAcquireLpcpLockByThread(CurrentThread);

        if ((ClientThread = Port->ClientThread) != NULL) {

            Port->ClientThread = NULL;

            LpcpReleaseLpcpLock();

            ObDereferenceObject( ClientThread );

        } else {

            LpcpReleaseLpcpLock();
        }
    }

    //
    //  Send an LPC_PORT_CLOSED datagram to whoever is connected
    //  to this port so they know they are no longer connected.
    //

    if ((Port->Flags & PORT_TYPE) == CLIENT_COMMUNICATION_PORT) {

        ClientPortClosedDatagram.PortMsg.u1.s1.TotalLength = sizeof( ClientPortClosedDatagram );
        ClientPortClosedDatagram.PortMsg.u1.s1.DataLength = sizeof( ClientPortClosedDatagram.CreateTime );

        ClientPortClosedDatagram.PortMsg.u2.s2.Type = LPC_PORT_CLOSED;
        ClientPortClosedDatagram.PortMsg.u2.s2.DataInfoOffset = 0;

        ClientPortClosedDatagram.CreateTime = PsGetCurrentProcess()->CreateTime;

        Status = LpcRequestPort( Port, (PPORT_MESSAGE)&ClientPortClosedDatagram );

        while (Status == STATUS_NO_MEMORY) {

            KeDelayExecutionThread(KernelMode, FALSE, &RetryInterval);

            Status = LpcRequestPort( Port, (PPORT_MESSAGE)&ClientPortClosedDatagram );
        }
    }

    //
    //  If connected, disconnect the port, and then scan the message queue
    //  for this port and dereference any messages in the queue.
    //

    LpcpDestroyPortQueue( Port, TRUE );

    //
    //  If we had mapped sections into the server or client communication ports,
    //  we need to unmap them in the context of that process.
    //

    if ( (Port->ClientSectionBase != NULL) ||
         (Port->ServerSectionBase != NULL) ) {

        //
        //  If the client has a port memory view, then unmap it
        //

        if (Port->ClientSectionBase != NULL) {

            MmUnmapViewOfSection( Port->MappingProcess,
                                  Port->ClientSectionBase );

        }

        //
        //  If the server has a port memory view, then unmap it
        //

        if (Port->ServerSectionBase != NULL) {

            MmUnmapViewOfSection( Port->MappingProcess,
                                  Port->ServerSectionBase  );

        }

        //
        //  Removing the reference added while mapping the section
        //

        ObDereferenceObject( Port->MappingProcess );

        Port->MappingProcess = NULL;
    }

    //
    //  Dereference the pointer to the connection port if it is not
    //  this port.
    //

    LpcpAcquireLpcpLockByThread(CurrentThread);

    ConnectionPort = Port->ConnectionPort;

    if (ConnectionPort) {

        CurrentProcessId = CurrentThread->Cid.UniqueProcess;

        Head = &ConnectionPort->LpcDataInfoChainHead;
        Next = Head->Flink;

        while (Next != Head) {

            Msg = CONTAINING_RECORD( Next, LPCP_MESSAGE, Entry );
            Next = Next->Flink;
            
            if (Port == ConnectionPort) {

                //
                //  If the Connection port is going away free all queued messages
                //

                RemoveEntryList( &Msg->Entry );
                InitializeListHead( &Msg->Entry );

                LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED );

                //
                //  In LpcpFreeToPortZone the LPC lock is released and reacquired.
                //  Another thread might free the LPC message captured above
                //  in Next. We need to restart the search at the list head.
                //

                Next = Head->Flink;

            } else if ((Msg->Request.ClientId.UniqueProcess == CurrentProcessId)
                    &&
                ((Msg->SenderPort == Port) 
                        || 
                 (Msg->SenderPort == Port->ConnectedPort) 
                        || 
                 (Msg->SenderPort == ConnectionPort))) {

                //
                //  Test whether the message come from the same port and process
                //

                LpcpTrace(( "%s Freeing DataInfo Message %lx (%u.%u)  Port: %lx\n",
                            PsGetCurrentProcess()->ImageFileName,
                            Msg,
                            Msg->Request.MessageId,
                            Msg->Request.CallbackId,
                            ConnectionPort ));

                RemoveEntryList( &Msg->Entry );
                InitializeListHead( &Msg->Entry );

                LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED );

                //
                //  In LpcpFreeToPortZone the LPC lock is released and reacquired.
                //  Another thread might free the LPC message captured above
                //  in Next. We need to restart the search at the list head.
                //

                Next = Head->Flink;
            }
        }

        LpcpReleaseLpcpLock();

        if (ConnectionPort != Port) {

            ObDereferenceObject( ConnectionPort );
        }

    } else {

        LpcpReleaseLpcpLock();
    }

    if (((Port->Flags & PORT_TYPE) == SERVER_CONNECTION_PORT) &&
        (ConnectionPort->ServerProcess != NULL)) {

        ObDereferenceObject( ConnectionPort->ServerProcess );

        ConnectionPort->ServerProcess = NULL;
    }

    //
    //  Free any static client security context
    //

    LpcpFreePortClientSecurity( Port );

    //
    //  And return to our caller
    //

    return;
}


VOID
LpcExitThread (
    PETHREAD Thread
    )

/*++

Routine Description:

    This routine is called whenever a thread is exiting and need to cleanup the
    lpc port for the thread.

Arguments:

    Thread - Supplies the thread being terminated

Return Value:

    None.

--*/

{
    PLPCP_MESSAGE Msg;

    //
    //  Acquire the mutex that protects the LpcReplyMessage field of
    //  the thread.  Zero the field so nobody else tries to process it
    //  when we release the lock.
    //

    ASSERT (Thread == PsGetCurrentThread());

    LpcpAcquireLpcpLockByThread(Thread);

    if (!IsListEmpty( &Thread->LpcReplyChain )) {

        RemoveEntryList( &Thread->LpcReplyChain );
    }

    //
    //  Indicate that this thread is exiting
    //

    Thread->LpcExitThreadCalled = TRUE;
    Thread->LpcReplyMessageId = 0;

    //
    //  If we need to reply to a message then if the thread that we need to reply
    //  to is still around we want to dereference the thread and free the message
    //

    Msg = LpcpGetThreadMessage(Thread);

    if (Msg != NULL) {

        Thread->LpcReplyMessage = NULL;

        if (Msg->RepliedToThread != NULL) {

            ObDereferenceObject( Msg->RepliedToThread );

            Msg->RepliedToThread = NULL;
        }

        LpcpTrace(( "Cleanup Msg %lx (%d) for Thread %lx allocated\n", Msg, IsListEmpty( &Msg->Entry ), Thread ));

        LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );
    }
    else {

        //
        //  Free the global lpc mutex.
        //

        LpcpReleaseLpcpLock();
    }

    //
    //  And return to our caller
    //

    return;
}

