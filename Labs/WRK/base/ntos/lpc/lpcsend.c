/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    lpcsend.c

Abstract:

    Local Inter-Process Communication (LPC) request system services.

--*/

#include "lpcp.h"

NTSTATUS
LpcpRequestWaitReplyPort (
    IN PVOID PortAddress,
    IN PPORT_MESSAGE RequestMessage,
    OUT PPORT_MESSAGE ReplyMessage,
    IN KPROCESSOR_MODE AccessMode
    );


#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,NtRequestPort)
#pragma alloc_text(PAGE,NtRequestWaitReplyPort)
#pragma alloc_text(PAGE,LpcRequestPort)
#pragma alloc_text(PAGE,LpcRequestWaitReplyPort)
#pragma alloc_text(PAGE,LpcpRequestWaitReplyPort)
#pragma alloc_text(PAGE,LpcRequestWaitReplyPortEx)
#endif


NTSTATUS
NtRequestPort (
    __in HANDLE PortHandle,
    __in PPORT_MESSAGE RequestMessage
    )

/*++

Routine Description:

    A client and server process send datagram messages using this procedure.

    The message pointed to by the RequestMessage parameter is placed in the
    message queue of the port connected to the communication port specified
    by the PortHandle parameter.  This service returns an error if PortHandle
    is invalid or if the MessageId field of the PortMessage structure is
    non-zero.

Arguments:

    PortHandle - Specifies the handle of the communication port to send
        the request message to.

    RequestMessage - Specifies a pointer to the request message.  The Type
        field of the message is set to LPC_DATAGRAM by the service.

Return Value:

    NTSTATUS - A status code that indicates whether or not the operation was
        successful.

--*/

{
    PETHREAD CurrentThread;
    PLPCP_PORT_OBJECT PortObject;
    PLPCP_PORT_OBJECT QueuePort;
    PORT_MESSAGE CapturedRequestMessage;
    ULONG MsgType;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;
    PLPCP_MESSAGE Msg;
    PLPCP_PORT_OBJECT ConnectionPort = NULL;

    PAGED_CODE();

    //
    //  Get previous processor mode and validate parameters
    //

    PreviousMode = KeGetPreviousMode();

    if (PreviousMode != KernelMode) {

        try {

            ProbeForReadSmallStructure( RequestMessage,
                                        sizeof( *RequestMessage ),
                                        PROBE_ALIGNMENT (PORT_MESSAGE));

            CapturedRequestMessage = *RequestMessage;
            CapturedRequestMessage.u2.s2.Type &= ~LPC_KERNELMODE_MESSAGE;

        } except( EXCEPTION_EXECUTE_HANDLER ) {

            return GetExceptionCode();
        }

        if (CapturedRequestMessage.u2.s2.Type != 0) {

            return STATUS_INVALID_PARAMETER;
        }

    } else {

        //
        //  This is a kernel mode caller
        //

        CapturedRequestMessage = *RequestMessage;

        if ((CapturedRequestMessage.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE) != 0) {

            return STATUS_INVALID_PARAMETER;
        }
    }

    //
    //  Make sure that the caller has given us some data to send
    //

    if (CapturedRequestMessage.u2.s2.DataInfoOffset != 0) {

        return STATUS_INVALID_PARAMETER;
    }

    //
    //  Make sure DataLength is valid with respect to header size and total length
    //

    if ((((CLONG)CapturedRequestMessage.u1.s1.DataLength) + sizeof( PORT_MESSAGE )) >
        ((CLONG)CapturedRequestMessage.u1.s1.TotalLength)) {

        return STATUS_INVALID_PARAMETER;
    }

    //
    //  Reference the communication port object by handle.  Return status if
    //  unsuccessful.
    //

    Status = LpcpReferencePortObject( PortHandle,
                                      0,
                                      PreviousMode,
                                      &PortObject );

    if (!NT_SUCCESS( Status )) {

        return Status;
    }

    //
    //  Validate the message length
    //

    if (((ULONG)CapturedRequestMessage.u1.s1.TotalLength > PortObject->MaxMessageLength) ||
        ((ULONG)CapturedRequestMessage.u1.s1.TotalLength <= (ULONG)CapturedRequestMessage.u1.s1.DataLength)) {

        ObDereferenceObject( PortObject );

        return STATUS_PORT_MESSAGE_TOO_LONG;
    }

    //
    //  Determine which port to queue the message to and get client
    //  port context if client sending to server.  Also validate
    //  length of message being sent.
    //

    //
    //  Allocate and initialize the LPC message to send off
    //

    Msg = (PLPCP_MESSAGE)LpcpAllocateFromPortZone( CapturedRequestMessage.u1.s1.TotalLength );

    if (Msg == NULL) {

        ObDereferenceObject( PortObject );

        return STATUS_NO_MEMORY;
    }

    Msg->RepliedToThread = NULL;
    Msg->PortContext = NULL;
    MsgType = CapturedRequestMessage.u2.s2.Type | LPC_DATAGRAM;

    CurrentThread = PsGetCurrentThread();

    if (PreviousMode != KernelMode) {

        try {

            LpcpMoveMessage( &Msg->Request,
                            &CapturedRequestMessage,
                            (RequestMessage + 1),
                            MsgType,
                            &CurrentThread->Cid );

        } except( EXCEPTION_EXECUTE_HANDLER ) {

            Status = GetExceptionCode();

            LpcpFreeToPortZone( Msg, 0 );

            ObDereferenceObject( PortObject );

            return Status;
        }

    }
    else {

        LpcpMoveMessage( &Msg->Request,
                         &CapturedRequestMessage,
                         (RequestMessage + 1),
                         MsgType,
                         &CurrentThread->Cid );
    }

    //
    //  Acquire the global Lpc mutex that guards the LpcReplyMessage
    //  field of the thread and the request message queue.  Stamp the
    //  request message with a serial number, insert the message at
    //  the tail of the request message queue and remember the address
    //  of the message in the LpcReplyMessage field for the current thread.
    //

    LpcpAcquireLpcpLockByThread(CurrentThread);

    //
    //  Based on what type of port the caller gave us we'll need to get
    //  the port to actually queue the message off to.
    //

    if ((PortObject->Flags & PORT_TYPE) != SERVER_CONNECTION_PORT) {

        //
        //  The caller didn't give us a connection port so find the
        //  connection port for this port.  If it is null then we'll
        //  fall through without sending a message
        //

        QueuePort = PortObject->ConnectedPort;

        //
        //  Check if the queue port is in process of going away
        //

        if ( QueuePort != NULL) {

            //
            //  If the port is a client communication port then give the
            //  message the proper port context
            //

            if ((PortObject->Flags & PORT_TYPE) == CLIENT_COMMUNICATION_PORT) {

                Msg->PortContext = QueuePort->PortContext;
                ConnectionPort = QueuePort = PortObject->ConnectionPort;

                if (ConnectionPort == NULL) {

                    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );
                    
                    ObDereferenceObject( PortObject );

                    return STATUS_PORT_DISCONNECTED;
                }

            //
            //  In the case we don't have a CLIENT_COMMUNICATION_PORT nor 
            //  SERVER_COMMUNICATION_PORT we'll use the connection port
            //  to queue messages. 
            //    

            } else if ((PortObject->Flags & PORT_TYPE) != SERVER_COMMUNICATION_PORT) {

                ConnectionPort = QueuePort = PortObject->ConnectionPort;
                
                if (ConnectionPort == NULL) {

                    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );
                    
                    ObDereferenceObject( PortObject );
                    
                    return STATUS_PORT_DISCONNECTED;
                }
            }

            if (ConnectionPort) {

                ObReferenceObject( ConnectionPort );
            }
        }

    } else {

        //
        //  The caller supplied a server connection port so that is the port
        //  we queue off to
        //

        QueuePort = PortObject;
    }

    //
    //  At this point we have an LPC message ready to send and if queue port is
    //  not null then we have a port to actually send the message off to
    //

    if (QueuePort != NULL) {

        //
        //  Reference the QueuePort to prevent this port evaporating under us
        //  Test if the QueuePort isn't in the process of going away 
        //  (i.e. we need to have at least 2 references for this object when 
        //  ObReferenceObject returns). Note the LPC lock is still held.
        //

        if ( ObReferenceObjectSafe( QueuePort ) ) {

            //
            //  Finish filling in the message and then insert it in the queue
            //

            Msg->Request.MessageId = LpcpGenerateMessageId();
            Msg->Request.CallbackId = 0;
            Msg->SenderPort = PortObject;

            CurrentThread->LpcReplyMessageId = 0;

            InsertTailList( &QueuePort->MsgQueue.ReceiveHead, &Msg->Entry );

            LpcpTrace(( "%s Send DataGram (%s) Msg %lx [%08x %08x %08x %08x] to Port %lx (%s)\n",
                        PsGetCurrentProcess()->ImageFileName,
                        LpcpMessageTypeName[ Msg->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE ],
                        Msg,
                        *((PULONG)(Msg+1)+0),
                        *((PULONG)(Msg+1)+1),
                        *((PULONG)(Msg+1)+2),
                        *((PULONG)(Msg+1)+3),
                        QueuePort,
                        LpcpGetCreatorName( QueuePort )));

            //
            //  Release the mutex, increment the request message queue
            //  semaphore by one for the newly inserted request message
            //  then exit the critical region.
            //
            //  Disable APCs to prevent this thread from being suspended
            //  before being able to release the semaphore.
            //

            KeEnterCriticalRegionThread(&CurrentThread->Tcb);

            LpcpReleaseLpcpLock();

            KeReleaseSemaphore( QueuePort->MsgQueue.Semaphore,
                                LPC_RELEASE_WAIT_INCREMENT,
                                1L,
                                FALSE );

            //
            //  If this is a waitable port then we'll need to set the event for
            //  anyone that was waiting on the port
            //

            if ( QueuePort->Flags & PORT_WAITABLE ) {

                KeSetEvent( &QueuePort->WaitEvent,
                            LPC_RELEASE_WAIT_INCREMENT,
                            FALSE );
            }

            //
            //  Exit the critical region and release the port object
            //

            KeLeaveCriticalRegionThread(&CurrentThread->Tcb);

            if (ConnectionPort) {

                ObDereferenceObject( ConnectionPort );
            }
            
            ObDereferenceObject( QueuePort );
            ObDereferenceObject( PortObject );

            //
            //  And return to our caller.  This is the only successful way out
            //  of this routine
            //

            return Status;
        }
    }

    //
    //  At this point we have a message but not a valid port to queue it off
    //  to so we'll free up the port object and release the unused message.
    //

    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

    ObDereferenceObject( PortObject );
    
    if (ConnectionPort) {

        ObDereferenceObject( ConnectionPort );
    }

    //
    //  And return the error status to our caller
    //

    return STATUS_PORT_DISCONNECTED;
}


NTSTATUS
NtRequestWaitReplyPort (
    __in HANDLE PortHandle,
    __in PPORT_MESSAGE RequestMessage,
    __out PPORT_MESSAGE ReplyMessage
    )

/*++

Routine Description:

    A client and server process can send a request and wait for a reply using
    the NtRequestWaitReplyPort service.

    If the Type field of the RequestMessage structure is euqal to LPC_REQUEST,
    then this is identified as a callback request.  The ClientId and MessageId
    fields are used to identify the thread that is waiting for a reply.  This
    thread is unblocked and the current thread that called this service then
    blocks waiting for a reply.

    The Type field of the message is set to LPC_REQUEST by the service.
    Otherwise the Type field of the message must be zero and it will be set to
    LPC_REQUEST by the service.  The message pointed to by the RequestMessage
    parameter is placed in the message queue of the port connected to the
    communication port specified by the PortHandle parameter.  This service
    returns an error if PortHandle is invalid.  The calling thread then blocks
    waiting for a reply.

    The reply message is stored in the location pointed to by the ReplyMessage
    parameter.  The ClientId, MessageId and message type fields will be filled
    in by the service.

Arguments:

    PortHandle - Specifies the handle of the communication port to send the
        request message to.

    RequestMessage - Specifies a pointer to a request message to send.

    ReplyMessage - Specifies the address of a variable that will receive the
        reply message.  This parameter may point to the same buffer as the
        RequestMessage parameter.

Return Value:

    NTSTATUS - A status code that indicates whether or not the operation was
        successful.

--*/

{
    PLPCP_PORT_OBJECT PortObject;
    PLPCP_PORT_OBJECT QueuePort;
    PLPCP_PORT_OBJECT RundownPort;
    PORT_MESSAGE CapturedRequestMessage;
    ULONG MsgType;
    PKSEMAPHORE ReleaseSemaphore;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;
    PLPCP_MESSAGE Msg;
    PETHREAD CurrentThread;
    PETHREAD WakeupThread;
    BOOLEAN CallbackRequest;
    PORT_DATA_INFORMATION CapturedDataInfo;
    PLPCP_PORT_OBJECT ConnectionPort = NULL;
    LOGICAL NoImpersonate;

    PAGED_CODE();

    //
    //  We cannot wait for a reply if the current thread is exiting
    //

    CurrentThread = PsGetCurrentThread();

    if (CurrentThread->LpcExitThreadCalled) {

        return STATUS_THREAD_IS_TERMINATING;
    }

    //
    //  Get previous processor mode and probe output arguments if necessary.
    //

    PreviousMode = KeGetPreviousMode();

    if (PreviousMode != KernelMode) {

        try {

            ProbeForReadSmallStructure( RequestMessage,
                                        sizeof( *RequestMessage ),
                                        PROBE_ALIGNMENT (PORT_MESSAGE));

            CapturedRequestMessage = *RequestMessage;
            CapturedRequestMessage.u2.s2.Type &= ~LPC_KERNELMODE_MESSAGE;

            ProbeForWriteSmallStructure( ReplyMessage,
                                         sizeof( *ReplyMessage ),
                                         PROBE_ALIGNMENT (PORT_MESSAGE));

            //
            //  Make sure that if this message has a data info offset that
            //  the port data information actually fits in the message.
            //
            //  We first check that the DataInfoOffset doesn't put us beyond
            //  the end of the message.
            //
            //  Then we capture the data info record and compute a pointer to
            //  the first unused data entry based on the supplied count.  If
            //  the start of the message plus its total length doesn't come
            //  up to the first unused data entry then the last valid data
            //  entry doesn't fit in the message buffer.  Also if the data
            //  entry pointer that we compute is less than the data info
            //  pointer then we must have wrapped.
            //

            if (CapturedRequestMessage.u2.s2.DataInfoOffset != 0) {

                PPORT_DATA_INFORMATION DataInfo;
                PPORT_DATA_ENTRY DataEntry;

                if (((ULONG)CapturedRequestMessage.u2.s2.DataInfoOffset) > (CapturedRequestMessage.u1.s1.TotalLength - sizeof(PORT_DATA_INFORMATION))) {

                    return STATUS_INVALID_PARAMETER;
                }

                if ((ULONG)CapturedRequestMessage.u2.s2.DataInfoOffset < sizeof(PORT_MESSAGE)) {

                    return STATUS_INVALID_PARAMETER;
                }

                DataInfo = (PPORT_DATA_INFORMATION)(((PUCHAR)RequestMessage) + CapturedRequestMessage.u2.s2.DataInfoOffset);

                ProbeForReadSmallStructure( DataInfo,
                                            sizeof( *DataInfo ),
                                            PROBE_ALIGNMENT (PORT_DATA_INFORMATION));

                CapturedDataInfo = *DataInfo;

                if (CapturedDataInfo.CountDataEntries > ((CapturedRequestMessage.u1.s1.TotalLength - CapturedRequestMessage.u2.s2.DataInfoOffset) / sizeof(PORT_DATA_ENTRY))) {

                    return STATUS_INVALID_PARAMETER;
                }

                DataEntry = &(DataInfo->DataEntries[CapturedDataInfo.CountDataEntries]);

                if ( ((PUCHAR)DataEntry < (PUCHAR)DataInfo)
                        ||
                     ((((PUCHAR)RequestMessage) + CapturedRequestMessage.u1.s1.TotalLength) < (PUCHAR)DataEntry)) {

                    return STATUS_INVALID_PARAMETER;
                }
            }

        } except( EXCEPTION_EXECUTE_HANDLER ) {

            return GetExceptionCode();
        }

    } else {

        CapturedRequestMessage = *RequestMessage;

        if (CapturedRequestMessage.u2.s2.DataInfoOffset != 0) {

            PPORT_DATA_INFORMATION DataInfo;

            if (((ULONG)CapturedRequestMessage.u2.s2.DataInfoOffset) > (CapturedRequestMessage.u1.s1.TotalLength - sizeof(PORT_DATA_INFORMATION))) {

                return STATUS_INVALID_PARAMETER;
            }

            if ((ULONG)CapturedRequestMessage.u2.s2.DataInfoOffset < sizeof(PORT_MESSAGE)) {

                return STATUS_INVALID_PARAMETER;
            }
            
            DataInfo = (PPORT_DATA_INFORMATION)(((PUCHAR)RequestMessage) + CapturedRequestMessage.u2.s2.DataInfoOffset);

            CapturedDataInfo = *DataInfo;
        }
    }

    //
    //  Capture the NoImpersonateFlag and clear the bit if necessary
    //

    if (CapturedRequestMessage.u2.s2.Type & LPC_NO_IMPERSONATE) {

        NoImpersonate = TRUE;
        CapturedRequestMessage.u2.s2.Type &= ~LPC_NO_IMPERSONATE;

    } else {

        NoImpersonate = FALSE;
    }
    
    //
    //  If the message type is an lpc request then say we need a callback.
    //  Otherwise if it not an lpc request and it is not a kernel mode message
    //  then it is an illegal parameter.  A third case is if the type is
    //  a kernel mode message in which case we make it look like an lpc request
    //  but without the callback.
    //

    if ((CapturedRequestMessage.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE) == LPC_REQUEST) {

        CallbackRequest = TRUE;

    } else if ((CapturedRequestMessage.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE) != 0) {

        return STATUS_INVALID_PARAMETER;

    } else {

        CapturedRequestMessage.u2.s2.Type |= LPC_REQUEST;
        CallbackRequest = FALSE;
    }

    //
    //  Make sure DataLength is valid with respect to header size and total length
    //

    if ((((CLONG)CapturedRequestMessage.u1.s1.DataLength) + sizeof( PORT_MESSAGE )) >
        ((CLONG)CapturedRequestMessage.u1.s1.TotalLength)) {

        return STATUS_INVALID_PARAMETER;
    }

    //
    //  Reference the communication port object by handle.  Return status if
    //  unsuccessful.
    //

    Status = LpcpReferencePortObject( PortHandle,
                                      0,
                                      PreviousMode,
                                      &PortObject );

    if (!NT_SUCCESS( Status )) {

        return Status;
    }

    //
    //  Validate the message length
    //

    if (((ULONG)CapturedRequestMessage.u1.s1.TotalLength > PortObject->MaxMessageLength) ||
        ((ULONG)CapturedRequestMessage.u1.s1.TotalLength <= (ULONG)CapturedRequestMessage.u1.s1.DataLength)) {

        ObDereferenceObject( PortObject );

        return STATUS_PORT_MESSAGE_TOO_LONG;
    }

    //
    //  Determine which port to queue the message to and get client
    //  port context if client sending to server.  Also validate
    //  length of message being sent.
    //

    //
    //  Allocate and initialize the LPC message to send off
    //


    Msg = (PLPCP_MESSAGE)LpcpAllocateFromPortZone( CapturedRequestMessage.u1.s1.TotalLength );

    if (Msg == NULL) {

        ObDereferenceObject( PortObject );

        return STATUS_NO_MEMORY;
    }

    MsgType = CapturedRequestMessage.u2.s2.Type;

    //
    //  Check if we need to do a callback
    //

    if (CallbackRequest) {

        //
        //  Check for a valid request message id
        //

        if (CapturedRequestMessage.MessageId == 0) {

            LpcpFreeToPortZone( Msg, 0 );

            ObDereferenceObject( PortObject );

            return STATUS_INVALID_PARAMETER;
        }

        //
        //  Translate the ClientId from the request into a
        //  thread pointer.  This is a referenced pointer to keep the thread
        //  from evaporating out from under us.
        //

        Status = PsLookupProcessThreadByCid( &CapturedRequestMessage.ClientId,
                                             NULL,
                                             &WakeupThread );

        if (!NT_SUCCESS( Status )) {

            LpcpFreeToPortZone( Msg, 0 );

            ObDereferenceObject( PortObject );

            return Status;
        }

        //
        //  Acquire the mutex that guards the LpcReplyMessage field of
        //  the thread and get the pointer to the message that the thread
        //  is waiting for a reply to.
        //

        LpcpAcquireLpcpLockByThread(CurrentThread);

        //
        //  See if the thread is waiting for a reply to the message
        //  specified on this call.  If not then a bogus message has been
        //  specified, so release the mutex, dereference the thread
        //  and return failure.
        //

        if ((WakeupThread->LpcReplyMessageId != CapturedRequestMessage.MessageId)

                ||

            ((LpcpGetThreadMessage(WakeupThread) != NULL) &&
             (LpcpGetThreadMessage(WakeupThread)->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE) != LPC_REQUEST)
                
                ||

            (!LpcpValidateClientPort(WakeupThread, PortObject, LPCP_VALIDATE_REASON_REPLY)) ) {

            LpcpPrint(( "%s Attempted CallBack Request to Thread %lx (%s)\n",
                        PsGetCurrentProcess()->ImageFileName,
                        WakeupThread,
                        THREAD_TO_PROCESS( WakeupThread )->ImageFileName ));

            LpcpPrint(( "failed.  MessageId == %u  Client Id: %x.%x\n",
                        CapturedRequestMessage.MessageId,
                        CapturedRequestMessage.ClientId.UniqueProcess,
                        CapturedRequestMessage.ClientId.UniqueThread ));

            LpcpPrint(( "         Thread MessageId == %u  Client Id: %x.%x\n",
                        WakeupThread->LpcReplyMessageId,
                        WakeupThread->Cid.UniqueProcess,
                        WakeupThread->Cid.UniqueThread ));

#if DBG
            if (LpcpStopOnReplyMismatch) {

                DbgBreakPoint();
            }
#endif

            LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

            ObDereferenceObject( WakeupThread );
            ObDereferenceObject( PortObject );

            return STATUS_REPLY_MESSAGE_MISMATCH;
        }

        //
        //  Copy over the text of the message
        //

        try {

            LpcpMoveMessage( &Msg->Request,
                             &CapturedRequestMessage,
                             (RequestMessage + 1),
                             MsgType,
                             &CurrentThread->Cid );

            if (CapturedRequestMessage.u2.s2.DataInfoOffset != 0) {

                PPORT_DATA_INFORMATION DataInfo;

                DataInfo = (PPORT_DATA_INFORMATION)(((PUCHAR)&Msg->Request) + CapturedRequestMessage.u2.s2.DataInfoOffset);

                if ( DataInfo->CountDataEntries != CapturedDataInfo.CountDataEntries ) {
                    
                    Status = STATUS_INVALID_PARAMETER;
                }
            }
        } except( EXCEPTION_EXECUTE_HANDLER ) {

            Status = GetExceptionCode();
        }

        if (!NT_SUCCESS( Status )) {

            LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

            ObDereferenceObject( WakeupThread );
            ObDereferenceObject( PortObject );

            return Status;
        }

        //
        //  Under the protect of the global lock we'll get everything
        //  ready for the callback
        //

        QueuePort = NULL;
        Msg->PortContext = NULL;

        if ((PortObject->Flags & PORT_TYPE) == SERVER_CONNECTION_PORT) {

            RundownPort = PortObject;

        } else {

            RundownPort = PortObject->ConnectedPort;

            if (RundownPort == NULL) {

                LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                ObDereferenceObject( WakeupThread );
                ObDereferenceObject( PortObject );

                return STATUS_PORT_DISCONNECTED;
            }

            if ((PortObject->Flags & PORT_TYPE) == CLIENT_COMMUNICATION_PORT) {

                Msg->PortContext = RundownPort->PortContext;
            }
        }

        Msg->Request.CallbackId = LpcpGenerateCallbackId();

        LpcpTrace(( "%s CallBack Request (%s) Msg %lx (%u.%u) [%08x %08x %08x %08x] to Thread %lx (%s)\n",
                    PsGetCurrentProcess()->ImageFileName,
                    LpcpMessageTypeName[ Msg->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE ],
                    Msg,
                    Msg->Request.MessageId,
                    Msg->Request.CallbackId,
                    *((PULONG)(Msg+1)+0),
                    *((PULONG)(Msg+1)+1),
                    *((PULONG)(Msg+1)+2),
                    *((PULONG)(Msg+1)+3),
                    WakeupThread,
                    THREAD_TO_PROCESS( WakeupThread )->ImageFileName ));

        //
        //  Add an extra reference so LpcExitThread does not evaporate
        //  the pointer before we get to the wait below
        //

        ObReferenceObject( WakeupThread );

        Msg->RepliedToThread = WakeupThread;

        WakeupThread->LpcReplyMessageId = 0;
        WakeupThread->LpcReplyMessage = (PVOID)Msg;

        //
        //  Remove the thread from the reply rundown list as we are sending a callback
        //

        if (!IsListEmpty( &WakeupThread->LpcReplyChain )) {

            RemoveEntryList( &WakeupThread->LpcReplyChain );

            InitializeListHead( &WakeupThread->LpcReplyChain );
        }

        CurrentThread->LpcReplyMessageId = Msg->Request.MessageId;
        CurrentThread->LpcReplyMessage = NULL;

        InsertTailList( &RundownPort->LpcReplyChainHead, &CurrentThread->LpcReplyChain );

        LpcpSetPortToThread( CurrentThread, PortObject );
        
        if (NoImpersonate) {

            LpcpSetThreadAttributes(CurrentThread, LPCP_NO_IMPERSONATION);
        }

        KeEnterCriticalRegionThread (&CurrentThread->Tcb);
        LpcpReleaseLpcpLock();

        //
        //  Wake up the thread that is waiting for an answer to its request
        //  inside of NtRequestWaitReplyPort or NtReplyWaitReplyPort
        //

        ReleaseSemaphore = &WakeupThread->LpcReplySemaphore;

    } else {

        //
        //  A callback is not required, so continue setting up the
        //  lpc message
        //

        try {

            LpcpMoveMessage( &Msg->Request,
                             &CapturedRequestMessage,
                             (RequestMessage + 1),
                             MsgType,
                             &CurrentThread->Cid );

            if (CapturedRequestMessage.u2.s2.DataInfoOffset != 0) {

                PPORT_DATA_INFORMATION DataInfo;

                DataInfo = (PPORT_DATA_INFORMATION)(((PUCHAR)&Msg->Request) + CapturedRequestMessage.u2.s2.DataInfoOffset);

                if ( DataInfo->CountDataEntries != CapturedDataInfo.CountDataEntries ) {

                    LpcpFreeToPortZone( Msg, 0 );

                    ObDereferenceObject( PortObject );

                    return STATUS_INVALID_PARAMETER;
                }
            }
        } except( EXCEPTION_EXECUTE_HANDLER ) {

            LpcpFreeToPortZone( Msg, 0 );

            ObDereferenceObject( PortObject );

            return GetExceptionCode();
        }

        //
        //  Acquire the global Lpc mutex that guards the LpcReplyMessage
        //  field of the thread and the request message queue.  Stamp the
        //  request message with a serial number, insert the message at
        //  the tail of the request message queue and remember the address
        //  of the message in the LpcReplyMessage field for the current thread.
        //

        LpcpAcquireLpcpLockByThread(CurrentThread);

        Msg->PortContext = NULL;

        if ((PortObject->Flags & PORT_TYPE) != SERVER_CONNECTION_PORT) {

            QueuePort = PortObject->ConnectedPort;

            if (QueuePort == NULL) {

                LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                ObDereferenceObject( PortObject );

                return STATUS_PORT_DISCONNECTED;
            }

            RundownPort = QueuePort;

            if ((PortObject->Flags & PORT_TYPE) == CLIENT_COMMUNICATION_PORT) {

                Msg->PortContext = QueuePort->PortContext;
                ConnectionPort = QueuePort = PortObject->ConnectionPort;
                
                if (ConnectionPort == NULL) {

                    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                    ObDereferenceObject( PortObject );

                    return STATUS_PORT_DISCONNECTED;
                }

            } else if ((PortObject->Flags & PORT_TYPE) != SERVER_COMMUNICATION_PORT) {

                ConnectionPort = QueuePort = PortObject->ConnectionPort;
                
                if (ConnectionPort == NULL) {

                    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                    ObDereferenceObject( PortObject );

                    return STATUS_PORT_DISCONNECTED;
                }
            }

            if (ConnectionPort) {

                ObReferenceObject( ConnectionPort );
            }

        } else {

            QueuePort = PortObject;
            RundownPort = PortObject;
        }

        //
        //  Stamp the request message with a serial number, insert the message
        //  at the tail of the request message queue
        //

        Msg->RepliedToThread = NULL;
        Msg->Request.MessageId = LpcpGenerateMessageId();
        Msg->Request.CallbackId = 0;
        Msg->SenderPort = PortObject;

        CurrentThread->LpcReplyMessageId = Msg->Request.MessageId;
        CurrentThread->LpcReplyMessage = NULL;
        
        InsertTailList( &QueuePort->MsgQueue.ReceiveHead, &Msg->Entry );
        InsertTailList( &RundownPort->LpcReplyChainHead, &CurrentThread->LpcReplyChain );
        
        LpcpSetPortToThread(CurrentThread, PortObject);

        if (NoImpersonate) {

            LpcpSetThreadAttributes(CurrentThread, LPCP_NO_IMPERSONATION);
        }

        LpcpTrace(( "%s Send Request (%s) Msg %lx (%u) [%08x %08x %08x %08x] to Port %lx (%s)\n",
                    PsGetCurrentProcess()->ImageFileName,
                    LpcpMessageTypeName[ Msg->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE ],
                    Msg,
                    Msg->Request.MessageId,
                    *((PULONG)(Msg+1)+0),
                    *((PULONG)(Msg+1)+1),
                    *((PULONG)(Msg+1)+2),
                    *((PULONG)(Msg+1)+3),
                    QueuePort,
                    LpcpGetCreatorName( QueuePort )));

        KeEnterCriticalRegionThread (&CurrentThread->Tcb);
        LpcpReleaseLpcpLock();

        //
        //  Increment the request message queue semaphore by one for
        //  the newly inserted request message.
        //

        ReleaseSemaphore = QueuePort->MsgQueue.Semaphore;

        //
        //  If port is waitable then set the event that someone could
        //  be waiting on
        //

        if ( QueuePort->Flags & PORT_WAITABLE ) {

            KeSetEvent( &QueuePort->WaitEvent,
                        LPC_RELEASE_WAIT_INCREMENT,
                        FALSE );
        }
    }

    //
    //  At this point we've enqueued our request and if necessary
    //  set ourselves up for the callback or reply.
    //
    //  So now wake up the other end
    //

    Status = KeReleaseSemaphore( ReleaseSemaphore,
                                 1,
                                 1,
                                 FALSE );

    KeLeaveCriticalRegionThread (&CurrentThread->Tcb);
    if (CallbackRequest) {

        ObDereferenceObject( WakeupThread );
    }

    //
    //  And wait for a reply
    //

    Status = KeWaitForSingleObject( &CurrentThread->LpcReplySemaphore,
                                    WrLpcReply,
                                    PreviousMode,
                                    FALSE,
                                    NULL );

    if (Status == STATUS_USER_APC) {

        //
        //  if the semaphore is signaled, then clear it
        //

        if (KeReadStateSemaphore( &CurrentThread->LpcReplySemaphore )) {

            KeWaitForSingleObject( &CurrentThread->LpcReplySemaphore,
                                   WrExecutive,
                                   KernelMode,
                                   FALSE,
                                   NULL );

            Status = STATUS_SUCCESS;
        }
    }

    //
    //  Acquire the LPC mutex.  Remove the reply message from the current thread
    //

    LpcpAcquireLpcpLockByThread(CurrentThread);

    Msg = LpcpGetThreadMessage(CurrentThread);

    CurrentThread->LpcReplyMessage = NULL;
    CurrentThread->LpcReplyMessageId = 0;

    //
    //  Remove the thread from the reply rundown list in case we did not wakeup due to
    //  a reply
    //

    if (!IsListEmpty( &CurrentThread->LpcReplyChain )) {

        RemoveEntryList( &CurrentThread->LpcReplyChain );

        InitializeListHead( &CurrentThread->LpcReplyChain );
    }

#if DBG
    if (Status == STATUS_SUCCESS && Msg != NULL) {

        LpcpTrace(( "%s Got Reply Msg %lx (%u) [%08x %08x %08x %08x] for Thread %lx (%s)\n",
                    PsGetCurrentProcess()->ImageFileName,
                    Msg,
                    Msg->Request.MessageId,
                    *((PULONG)(Msg+1)+0),
                    *((PULONG)(Msg+1)+1),
                    *((PULONG)(Msg+1)+2),
                    *((PULONG)(Msg+1)+3),
                    CurrentThread,
                    THREAD_TO_PROCESS( CurrentThread )->ImageFileName ));

        if (!IsListEmpty( &Msg->Entry )) {

            LpcpTrace(( "Reply Msg %lx has non-empty list entry\n", Msg ));
        }
    }
#endif

    LpcpReleaseLpcpLock();

    //
    //  If the wait succeeded, copy the reply to the reply buffer.
    //

    if (Status == STATUS_SUCCESS) {

        if (Msg != NULL) {

            try {

                LpcpMoveMessage( ReplyMessage,
                                 &Msg->Request,
                                 (&Msg->Request) + 1,
                                 0,
                                 NULL );

            } except( EXCEPTION_EXECUTE_HANDLER ) {

                Status = GetExceptionCode();
            }

            //
            //  Acquire the LPC mutex and decrement the reference count for the
            //  message.  If the reference count goes to zero the message will be
            //  deleted.
            //

            if (((Msg->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE) == LPC_REQUEST) &&
                (Msg->Request.u2.s2.DataInfoOffset != 0)) {

                LpcpSaveDataInfoMessage( PortObject, Msg, 0 );

            } else {

                LpcpFreeToPortZone( Msg, 0 );
            }

        } else {

            Status = STATUS_LPC_REPLY_LOST;
        }

    } else {

        //
        //  Wait failed, free the message if any.
        //

        LpcpTrace(( "%s NtRequestWaitReply wait failed - Status == %lx\n",
                    PsGetCurrentProcess()->ImageFileName,
                    Status ));

        if (Msg != NULL) {

            LpcpFreeToPortZone( Msg, 0);
        }
    }

    ObDereferenceObject( PortObject );
    
    if (ConnectionPort) {

        ObDereferenceObject( ConnectionPort );
    }

    //
    //  And return to our caller
    //

    return Status;
}


NTSTATUS
LpcRequestPort (
    __in PVOID PortAddress,
    __in PPORT_MESSAGE RequestMessage
    )

/*++

Routine Description:

    This procedure is similar to NtRequestPort but without the Handle based
    interface.

Arguments:

    PortAddress - Supplies a pointer to the communication port to send
        the request message to.

    RequestMessage - Specifies a pointer to the request message.  The Type
        field of the message is set to LPC_DATAGRAM by the service.

Return Value:

    NTSTATUS - A status code that indicates whether or not the operation was
        successful.

--*/

{
    PETHREAD CurrentThread;
    PLPCP_PORT_OBJECT PortObject = (PLPCP_PORT_OBJECT)PortAddress;
    PLPCP_PORT_OBJECT QueuePort;
    ULONG MsgType;
    PLPCP_MESSAGE Msg;
    KPROCESSOR_MODE PreviousMode;
    PLPCP_PORT_OBJECT ConnectionPort = NULL;

    PAGED_CODE();

    //
    //  Get previous processor mode and validate parameters
    //

    PreviousMode = KeGetPreviousMode();

    if (RequestMessage->u2.s2.Type != 0) {

        MsgType = RequestMessage->u2.s2.Type & ~LPC_KERNELMODE_MESSAGE;

        if ((MsgType < LPC_DATAGRAM) ||
            (MsgType > LPC_CLIENT_DIED)) {

            return STATUS_INVALID_PARAMETER;
        }

        //
        //  If previous mode is kernel, allow the LPC_KERNELMODE_MESSAGE
        //  bit to be passed in message type field.
        //

        if ((PreviousMode == KernelMode) &&
            (RequestMessage->u2.s2.Type & LPC_KERNELMODE_MESSAGE)) {

            MsgType |= LPC_KERNELMODE_MESSAGE;
        }

    } else {

        MsgType = LPC_DATAGRAM;
    }

    if (RequestMessage->u2.s2.DataInfoOffset != 0) {

        return STATUS_INVALID_PARAMETER;
    }

    //
    //  Validate the message length
    //

    if (((ULONG)RequestMessage->u1.s1.TotalLength > PortObject->MaxMessageLength) ||
        ((ULONG)RequestMessage->u1.s1.TotalLength <= (ULONG)RequestMessage->u1.s1.DataLength)) {

        return STATUS_PORT_MESSAGE_TOO_LONG;
    }

    //
    //  Allocate a message block
    //


    Msg = (PLPCP_MESSAGE)LpcpAllocateFromPortZone( RequestMessage->u1.s1.TotalLength );

    if (Msg == NULL) {

        return STATUS_NO_MEMORY;
    }

    //
    //  Fill in the message block.
    //

    Msg->RepliedToThread = NULL;
    Msg->PortContext = NULL;

    CurrentThread = PsGetCurrentThread();

    LpcpMoveMessage( &Msg->Request,
                     RequestMessage,
                     (RequestMessage + 1),
                     MsgType,
                     &CurrentThread->Cid );

    //
    //  Acquire the global Lpc mutex that guards the LpcReplyMessage
    //  field of the thread and the request message queue.  Stamp the
    //  request message with a serial number, insert the message at
    //  the tail of the request message queue
    //

    LpcpAcquireLpcpLockByThread(CurrentThread);

    if ((PortObject->Flags & PORT_TYPE) != SERVER_CONNECTION_PORT) {

        QueuePort = PortObject->ConnectedPort;

        if (QueuePort != NULL) {

            if ((PortObject->Flags & PORT_TYPE) == CLIENT_COMMUNICATION_PORT) {

                Msg->PortContext = QueuePort->PortContext;
                ConnectionPort = QueuePort = PortObject->ConnectionPort;

                if (ConnectionPort == NULL) {

                    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                    return STATUS_PORT_DISCONNECTED;
                }

            //
            //  In the case we don't have a CLIENT_COMMUNICATION_PORT nor 
            //  SERVER_COMMUNICATION_PORT we'll use the connection port
            //  to queue messages. 
            //    

            } else if ((PortObject->Flags & PORT_TYPE) != SERVER_COMMUNICATION_PORT) {

                ConnectionPort = QueuePort = PortObject->ConnectionPort;
                
                if (ConnectionPort == NULL) {

                    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                    return STATUS_PORT_DISCONNECTED;
                }
            }

            if (ConnectionPort) {

                ObReferenceObject( ConnectionPort );
            }
        }

    } else {

        QueuePort = PortObject;
    }

    //
    //  At this point we have an LPC message ready to send and if queue port is
    //  not null then we have a port to actually send the message off to
    //

    if (QueuePort != NULL) {

        Msg->Request.MessageId = LpcpGenerateMessageId();
        Msg->Request.CallbackId = 0;
        Msg->SenderPort = PortObject;

        CurrentThread->LpcReplyMessageId = 0;

        InsertTailList( &QueuePort->MsgQueue.ReceiveHead, &Msg->Entry );

        LpcpTrace(( "%s Send DataGram (%s) Msg %lx [%08x %08x %08x %08x] to Port %lx (%s)\n",
                    PsGetCurrentProcess()->ImageFileName,
                    LpcpMessageTypeName[ Msg->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE ],
                    Msg,
                    *((PULONG)(Msg+1)+0),
                    *((PULONG)(Msg+1)+1),
                    *((PULONG)(Msg+1)+2),
                    *((PULONG)(Msg+1)+3),
                    QueuePort,
                    LpcpGetCreatorName( QueuePort )));

        //
        //  Release the mutex, increment the request message queue
        //  semaphore by one for the newly inserted request message,
        //  then exit the critical region.
        //
        //  Disable APCs to prevent this thread from being suspended
        //  before being able to release the semaphore.
        //

        KeEnterCriticalRegionThread(&CurrentThread->Tcb);

        LpcpReleaseLpcpLock();

        KeReleaseSemaphore( QueuePort->MsgQueue.Semaphore,
                            LPC_RELEASE_WAIT_INCREMENT,
                            1L,
                            FALSE );


        if ( QueuePort->Flags & PORT_WAITABLE ) {

            KeSetEvent( &QueuePort->WaitEvent,
                        LPC_RELEASE_WAIT_INCREMENT,
                        FALSE );
        }

        KeLeaveCriticalRegionThread(&CurrentThread->Tcb);

        if (ConnectionPort) {

            ObDereferenceObject( ConnectionPort );
        }

        return STATUS_SUCCESS;

    }

    //
    //  At this point we have a message but not a valid port to queue it off
    //  to so we'll release the unused message and release our mutex.
    //

    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

    if (ConnectionPort) {

        ObDereferenceObject( ConnectionPort );
    }

    //
    //  And return the error status to our caller
    //

    return STATUS_PORT_DISCONNECTED;
}


NTSTATUS
LpcpRequestWaitReplyPort (
    IN PVOID PortAddress,
    IN PPORT_MESSAGE RequestMessage,
    OUT PPORT_MESSAGE ReplyMessage,
    IN KPROCESSOR_MODE AccessMode
    )

/*++

Routine Description:

    This procedure is similar to NtRequestWaitReplyPort but without the
    handle based interface

Arguments:

    PortAddress - Supplies the communication port object to send the
        request message to.

    RequestMessage - Specifies a pointer to a request message to send.

    ReplyMessage - Specifies the address of a variable that will receive the
        reply message.  This parameter may point to the same buffer as the
        RequestMessage parameter.

Return Value:

    NTSTATUS - A status code that indicates whether or not the operation was
        successful.

--*/

{
    PLPCP_PORT_OBJECT PortObject = (PLPCP_PORT_OBJECT)PortAddress;
    PLPCP_PORT_OBJECT QueuePort;
    PLPCP_PORT_OBJECT RundownPort;
    PKSEMAPHORE ReleaseSemaphore;
    NTSTATUS Status;
    ULONG MsgType;
    PLPCP_MESSAGE Msg;
    PETHREAD CurrentThread;
    PETHREAD WakeupThread;
    BOOLEAN CallbackRequest;
    KPROCESSOR_MODE PreviousMode;
    PLPCP_PORT_OBJECT ConnectionPort = NULL;

    PAGED_CODE();

    CurrentThread = PsGetCurrentThread();

    if (CurrentThread->LpcExitThreadCalled) {

        return STATUS_THREAD_IS_TERMINATING;
    }

    //
    //  Get previous processor mode and validate parameters
    //

    PreviousMode = KeGetPreviousMode();
    MsgType = RequestMessage->u2.s2.Type & ~LPC_KERNELMODE_MESSAGE;
    CallbackRequest = FALSE;

    switch (MsgType) {

    case 0:

        MsgType = LPC_REQUEST;
        break;

    case LPC_REQUEST:

        CallbackRequest = TRUE;
        break;

    case LPC_CLIENT_DIED:
    case LPC_PORT_CLOSED:
    case LPC_EXCEPTION:
    case LPC_DEBUG_EVENT:
    case LPC_ERROR_EVENT:

        break;

    default :

        return STATUS_INVALID_PARAMETER;
    }

    //
    //  Allow the LPC_KERNELMODE_MESSAGE
    //  bit to be passed in message type field. Don't check the previous mode !!!
    //

    if ( RequestMessage->u2.s2.Type & LPC_KERNELMODE_MESSAGE) {

        MsgType |= LPC_KERNELMODE_MESSAGE;
    }

    RequestMessage->u2.s2.Type = (CSHORT)MsgType;

    //
    //  Validate the message length
    //

    if (((ULONG)RequestMessage->u1.s1.TotalLength > PortObject->MaxMessageLength) ||
        ((ULONG)RequestMessage->u1.s1.TotalLength <= (ULONG)RequestMessage->u1.s1.DataLength)) {

        return STATUS_PORT_MESSAGE_TOO_LONG;
    }

    //
    //  Determine which port to queue the message to and get client
    //  port context if client sending to server.  Also validate
    //  length of message being sent.
    //


    Msg = (PLPCP_MESSAGE)LpcpAllocateFromPortZone( RequestMessage->u1.s1.TotalLength );


    if (Msg == NULL) {

        return STATUS_NO_MEMORY;
    }

    if (CallbackRequest) {

        //
        //  Check for a valid request message id
        //

        if (RequestMessage->MessageId == 0) {

            LpcpFreeToPortZone( Msg, 0 );

            return STATUS_INVALID_PARAMETER;
        }

        //
        //  Translate the ClientId from the request into a
        //  thread pointer.  This is a referenced pointer to keep the thread
        //  from evaporating out from under us.
        //

        Status = PsLookupProcessThreadByCid( &RequestMessage->ClientId,
                                             NULL,
                                             &WakeupThread );

        if (!NT_SUCCESS( Status )) {

            LpcpFreeToPortZone( Msg, 0 );

            return Status;
        }

        //
        //  Acquire the mutex that guards the LpcReplyMessage field of
        //  the thread and get the pointer to the message that the thread
        //  is waiting for a reply to.
        //

        LpcpAcquireLpcpLockByThread(CurrentThread);

        //
        //  See if the thread is waiting for a reply to the message
        //  specified on this call.  If not then a bogus message
        //  has been specified, so release the mutex, dereference the thread
        //  and return failure.
        //

        if ((WakeupThread->LpcReplyMessageId != RequestMessage->MessageId)

                ||

            ((LpcpGetThreadMessage(WakeupThread) != NULL) &&
             (LpcpGetThreadMessage(WakeupThread)->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE) != LPC_REQUEST)

                ||

            (!LpcpValidateClientPort(WakeupThread, PortObject, LPCP_VALIDATE_REASON_REPLY)) ) {

            LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

            ObDereferenceObject( WakeupThread );

            return STATUS_REPLY_MESSAGE_MISMATCH;
        }

        QueuePort = NULL;
        Msg->PortContext = NULL;

        if ((PortObject->Flags & PORT_TYPE) == SERVER_CONNECTION_PORT) {

            RundownPort = PortObject;

        } else {

            RundownPort = PortObject->ConnectedPort;

            if (RundownPort == NULL) {

                LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                ObDereferenceObject( WakeupThread );

                return STATUS_PORT_DISCONNECTED;
            }

            if ((PortObject->Flags & PORT_TYPE) == CLIENT_COMMUNICATION_PORT) {

                Msg->PortContext = RundownPort->PortContext;
            }
        }
        
        //
        //  Allocate and initialize a request message
        //

        LpcpMoveMessage( &Msg->Request,
                         RequestMessage,
                         (RequestMessage + 1),
                         0,
                         &CurrentThread->Cid );

        Msg->Request.CallbackId = LpcpGenerateCallbackId();

        LpcpTrace(( "%s CallBack Request (%s) Msg %lx (%u.%u) [%08x %08x %08x %08x] to Thread %lx (%s)\n",
                    PsGetCurrentProcess()->ImageFileName,
                    LpcpMessageTypeName[ Msg->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE ],
                    Msg,
                    Msg->Request.MessageId,
                    Msg->Request.CallbackId,
                    *((PULONG)(Msg+1)+0),
                    *((PULONG)(Msg+1)+1),
                    *((PULONG)(Msg+1)+2),
                    *((PULONG)(Msg+1)+3),
                    WakeupThread,
                    THREAD_TO_PROCESS( WakeupThread )->ImageFileName ));

        //
        //  Add an extra reference so LpcExitThread does not evaporate
        //  the pointer before we get to the wait below
        //

        ObReferenceObject( WakeupThread );

        Msg->RepliedToThread = WakeupThread;

        WakeupThread->LpcReplyMessageId = 0;
        WakeupThread->LpcReplyMessage = (PVOID)Msg;

        //
        //  Remove the thread from the reply rundown list as we are sending a callback
        //

        if (!IsListEmpty( &WakeupThread->LpcReplyChain )) {

            RemoveEntryList( &WakeupThread->LpcReplyChain );

            InitializeListHead( &WakeupThread->LpcReplyChain );
        }

        CurrentThread->LpcReplyMessageId = Msg->Request.MessageId;
        CurrentThread->LpcReplyMessage = NULL;
        
        InsertTailList( &RundownPort->LpcReplyChainHead, &CurrentThread->LpcReplyChain );
        
        LpcpSetPortToThread(CurrentThread, PortObject);

        KeEnterCriticalRegionThread (&CurrentThread->Tcb);
        LpcpReleaseLpcpLock();

        //
        //  Wake up the thread that is waiting for an answer to its request
        //  inside of NtRequestWaitReplyPort or NtReplyWaitReplyPort
        //

        ReleaseSemaphore = &WakeupThread->LpcReplySemaphore;

    } else {

        //
        //  There is no callback requested
        //

        LpcpMoveMessage( &Msg->Request,
                         RequestMessage,
                         (RequestMessage + 1),
                         0,
                         &CurrentThread->Cid );

        //
        //  Acquire the global Lpc mutex that guards the LpcReplyMessage
        //  field of the thread and the request message queue.  Stamp the
        //  request message with a serial number, insert the message at
        //  the tail of the request message queue and remember the address
        //  of the message in the LpcReplyMessage field for the current thread.
        //

        LpcpAcquireLpcpLockByThread(CurrentThread);

        if ((CurrentThread->LpcReplyMessage != NULL)
                ||
            (CurrentThread->LpcReplyMessageId != 0)
                ||
            (CurrentThread->KeyedEventInUse)) {
            
            LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

            return STATUS_UNSUCCESSFUL;
        }

        Msg->PortContext = NULL;

        if ((PortObject->Flags & PORT_TYPE) != SERVER_CONNECTION_PORT) {

            QueuePort = PortObject->ConnectedPort;

            if (QueuePort == NULL) {

                LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                return STATUS_PORT_DISCONNECTED;
            }

            RundownPort = QueuePort;

            if ((PortObject->Flags & PORT_TYPE) == CLIENT_COMMUNICATION_PORT) {

                Msg->PortContext = QueuePort->PortContext;
                ConnectionPort = QueuePort = PortObject->ConnectionPort;

                if (ConnectionPort == NULL) {

                    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                    return STATUS_PORT_DISCONNECTED;
                }

            } else if ((PortObject->Flags & PORT_TYPE) != SERVER_COMMUNICATION_PORT) {

                ConnectionPort = QueuePort = PortObject->ConnectionPort;
                
                if (ConnectionPort == NULL) {

                    LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                    return STATUS_PORT_DISCONNECTED;
                }
            }

            if (ConnectionPort) {

                ObReferenceObject( ConnectionPort );
            }

        } else {

            if ((PortObject->Flags & PORT_NAME_DELETED) != 0) {
                LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

                return STATUS_PORT_DISCONNECTED;
            }

            QueuePort = PortObject;
            RundownPort = PortObject;
        }

        Msg->RepliedToThread = NULL;
        Msg->Request.MessageId = LpcpGenerateMessageId();
        Msg->Request.CallbackId = 0;
        Msg->SenderPort = PortObject;

        CurrentThread->LpcReplyMessageId = Msg->Request.MessageId;
        CurrentThread->LpcReplyMessage = NULL;

        InsertTailList( &QueuePort->MsgQueue.ReceiveHead, &Msg->Entry );
        InsertTailList( &RundownPort->LpcReplyChainHead, &CurrentThread->LpcReplyChain );
        
        LpcpSetPortToThread(CurrentThread, PortObject);

        LpcpTrace(( "%s Send Request (%s) Msg %lx (%u) [%08x %08x %08x %08x] to Port %lx (%s)\n",
                    PsGetCurrentProcess()->ImageFileName,
                    LpcpMessageTypeName[ Msg->Request.u2.s2.Type & ~LPC_KERNELMODE_MESSAGE ],
                    Msg,
                    Msg->Request.MessageId,
                    *((PULONG)(Msg+1)+0),
                    *((PULONG)(Msg+1)+1),
                    *((PULONG)(Msg+1)+2),
                    *((PULONG)(Msg+1)+3),
                    QueuePort,
                    LpcpGetCreatorName( QueuePort )));

        KeEnterCriticalRegionThread (&CurrentThread->Tcb);
        LpcpReleaseLpcpLock();

        //
        //  Increment the request message queue semaphore by one for
        //  the newly inserted request message.  Release the spin
        //  lock, while remaining at the dispatcher IRQL.  Then wait for the
        //  reply to this request by waiting on the LpcReplySemaphore
        //  for the current thread.
        //

        ReleaseSemaphore = QueuePort->MsgQueue.Semaphore;

        if ( QueuePort->Flags & PORT_WAITABLE ) {

            KeSetEvent( &QueuePort->WaitEvent,
                        LPC_RELEASE_WAIT_INCREMENT,
                        FALSE );
        }
    }

    //
    //  At this point we've enqueued our request and if necessary
    //  set ourselves up for the callback or reply.
    //
    //  So now wake up the other end
    //

    Status = KeReleaseSemaphore( ReleaseSemaphore,
                                 1,
                                 1,
                                 FALSE );
    KeLeaveCriticalRegionThread (&CurrentThread->Tcb);

    if (CallbackRequest) {

        ObDereferenceObject( WakeupThread );
    }

    //
    //  And wait for a reply
    //

    Status = KeWaitForSingleObject( &CurrentThread->LpcReplySemaphore,
                                    WrLpcReply,
                                    AccessMode,
                                    FALSE,
                                    NULL );

    if (Status == STATUS_USER_APC) {

        //
        //  if the semaphore is signaled, then clear it
        //

        if (KeReadStateSemaphore( &CurrentThread->LpcReplySemaphore )) {

            KeWaitForSingleObject( &CurrentThread->LpcReplySemaphore,
                                   WrExecutive,
                                   KernelMode,
                                   FALSE,
                                   NULL );

            Status = STATUS_SUCCESS;
        }
    }

    //
    //  Acquire the LPC mutex.  Remove the reply message from the current thread
    //

    LpcpAcquireLpcpLockByThread(CurrentThread);

    Msg = LpcpGetThreadMessage(CurrentThread);

    CurrentThread->LpcReplyMessage = NULL;
    CurrentThread->LpcReplyMessageId = 0;

    //
    //  Remove the thread from the reply rundown list in case we did not wakeup due to
    //  a reply
    //

    if (!IsListEmpty( &CurrentThread->LpcReplyChain )) {

        RemoveEntryList( &CurrentThread->LpcReplyChain );

        InitializeListHead( &CurrentThread->LpcReplyChain );
    }

#if DBG
    if (Msg != NULL) {

        LpcpTrace(( "%s Got Reply Msg %lx (%u) [%08x %08x %08x %08x] for Thread %lx (%s)\n",
                    PsGetCurrentProcess()->ImageFileName,
                    Msg,
                    Msg->Request.MessageId,
                    *((PULONG)(Msg+1)+0),
                    *((PULONG)(Msg+1)+1),
                    *((PULONG)(Msg+1)+2),
                    *((PULONG)(Msg+1)+3),
                    CurrentThread,
                    THREAD_TO_PROCESS( CurrentThread )->ImageFileName ));
    }
#endif

    LpcpReleaseLpcpLock();

    //
    //  If the wait succeeded, copy the reply to the reply buffer.
    //

    if (Status == STATUS_SUCCESS) {

        if (Msg != NULL) {

            LpcpMoveMessage( ReplyMessage,
                             &Msg->Request,
                             (&Msg->Request) + 1,
                             0,
                             NULL );

            //
            //  Acquire the LPC mutex and decrement the reference count for the
            //  message.  If the reference count goes to zero the message will be
            //  deleted.
            //

            LpcpAcquireLpcpLockByThread(CurrentThread);

            if (Msg->RepliedToThread != NULL) {

                ObDereferenceObject( Msg->RepliedToThread );

                Msg->RepliedToThread = NULL;
            }

            LpcpFreeToPortZone( Msg, LPCP_MUTEX_OWNED | LPCP_MUTEX_RELEASE_ON_RETURN );

        } else {

            Status = STATUS_LPC_REPLY_LOST;
        }

    } else {

        //
        //  Wait failed, free the message if any.
        //

        if (Msg != NULL) {

            LpcpFreeToPortZone( Msg, 0 );
        }
    }

    if (ConnectionPort) {

        ObDereferenceObject( ConnectionPort );
    }
    
    //
    //  And return to our caller
    //

    return Status;
}


NTSTATUS
LpcRequestWaitReplyPort (
    __in PVOID PortAddress,
    __in PPORT_MESSAGE RequestMessage,
    __out PPORT_MESSAGE ReplyMessage
    )
{
    return LpcpRequestWaitReplyPort( PortAddress,
                                     RequestMessage,
                                     ReplyMessage,
                                     KernelMode
                                   );
}


NTSTATUS
LpcRequestWaitReplyPortEx (
    IN PVOID PortAddress,
    IN PPORT_MESSAGE RequestMessage,
    OUT PPORT_MESSAGE ReplyMessage
    )
{
    return LpcpRequestWaitReplyPort( PortAddress,
                                     RequestMessage,
                                     ReplyMessage,
                                     KeGetPreviousMode()
                                   );
}

