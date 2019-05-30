/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    lpcpriv.c

Abstract:

    Local Inter-Process Communication privileged procedures that implement
    client impersonation.

--*/

#include "lpcp.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,LpcpFreePortClientSecurity)
#pragma alloc_text(PAGE,NtImpersonateClientOfPort)
#endif


NTSTATUS
NtImpersonateClientOfPort (
    __in HANDLE PortHandle,
    __in PPORT_MESSAGE Message
    )

/*++

Routine Description:

    This procedure is used by the server thread to temporarily acquire the
    identifier set of a client thread.

    This service establishes an impersonation token for the calling thread.
    The impersonation token corresponds to the context provided by the port
    client.  The client must currently be waiting for a reply to the
    specified message.

    This service returns an error status code if the client thread is not
    waiting for a reply to the message.  The security quality of service
    parameters specified by the client upon connection dictate what use the
    server will have of the client's security context.

    For complicated or extended impersonation needs, the server may open a
    copy of the client's token (using NtOpenThreadToken()).  This must be
    done while impersonating the client.

Arguments:

    PortHandle - Specifies the handle of the communication port that the
        message was received from.

    Message - Specifies an address of a message that was received from the
        client that is to be impersonated.  The ClientId field of the message
        identifies the client thread that is to be impersonated.  The client
        thread must be waiting for a reply to the message in order to
        impersonate the client.

Return Value:

    NTSTATUS - Status code that indicates whether or not the operation was
    successful.

--*/

{
    PLPCP_PORT_OBJECT PortObject;
    PLPCP_PORT_OBJECT ConnectedPort;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;
    PETHREAD ClientThread;
    CLIENT_ID CapturedClientId;
    ULONG CapturedMessageId;
    SECURITY_CLIENT_CONTEXT DynamicSecurity;

    PAGED_CODE();

    //
    //  Get previous processor mode and probe output arguments if necessary.
    //

    PreviousMode = KeGetPreviousMode();

    if (PreviousMode != KernelMode) {

        try {

            ProbeForReadSmallStructure( Message, sizeof( PORT_MESSAGE ), sizeof( ULONG ));

            CapturedClientId = Message->ClientId;
            CapturedMessageId = Message->MessageId;

        } except( EXCEPTION_EXECUTE_HANDLER ) {

            return( GetExceptionCode() );
        }

    } else {

        CapturedClientId = Message->ClientId;
        CapturedMessageId = Message->MessageId;
    }

    //
    //  Reference the communication port object by handle.  Return status if
    //  unsuccessful.
    //

    Status = LpcpReferencePortObject( PortHandle, 0,
                                      PreviousMode, &PortObject );
    if (!NT_SUCCESS( Status )) {

        return( Status );
    }

    //
    //  It is an error to try this on any port other than a server
    //  communication port
    //

    if ((PortObject->Flags & PORT_TYPE) != SERVER_COMMUNICATION_PORT) {

        ObDereferenceObject( PortObject );

        return( STATUS_INVALID_PORT_HANDLE );
    }

    //
    //  Translate the ClientId from the connection request into a
    //  thread pointer.  This is a referenced pointer to keep the thread
    //  from evaporating out from under us.
    //

    Status = PsLookupProcessThreadByCid( &CapturedClientId,
                                         NULL,
                                         &ClientThread );

    if (!NT_SUCCESS( Status )) {

        ObDereferenceObject( PortObject );

        return( Status );
    }

    //
    //  Acquire the mutex that guards the LpcReplyMessage field of
    //  the thread and get the pointer to the message that the thread
    //  is waiting for a reply to.
    //

    LpcpAcquireLpcpLock();

    //
    //  The connected port can be in a state with 0 references in a deletion process.
    //  We need to test this case while referencing it.
    //

    ConnectedPort = PortObject->ConnectedPort;
    
    if ( ( ConnectedPort == NULL ) || 
         ( !ObReferenceObjectSafe( ConnectedPort ) ) ) {

        //
        //  The port is being deleted. Quit this function with 
        //  appropriate return status. 
        //  We don't need to dereference the connected port because 
        //  it is anyway about to be deleted
        //

        LpcpReleaseLpcpLock();

        ObDereferenceObject( PortObject );
        ObDereferenceObject( ClientThread );

        return( STATUS_PORT_DISCONNECTED );
    }

    //
    //  See if the thread is waiting for a reply to the message
    //  specified on this call, if the user gave us a bad
    //  message id.  If not then a bogus message
    //  has been specified, so return failure.
    //

    //
    //  The W2k fix searched the client thread in the rundown queue, to make sure
    //  we are not impersonating a port from a different connection. Ones we added the port
    //  to the thread structure to fix other security issues for reply os accessing data
    //  we can use that easy test for the impersonation too, w/o searching the rundown queue
    //

    if ((ClientThread->LpcReplyMessageId != CapturedMessageId) 
            ||
        (CapturedMessageId == 0) 
            ||
        (!LpcpValidateClientPort( ClientThread, 
                                  PortObject, 
                                  LPCP_VALIDATE_REASON_IMPERSONATION)) ) {

        LpcpReleaseLpcpLock();

        ObDereferenceObject( PortObject );
        ObDereferenceObject( ClientThread );
        ObDereferenceObject( ConnectedPort );

        return (STATUS_REPLY_MESSAGE_MISMATCH);
    }

    //
    //  Test whether the client allows impersonation for this message or not.
    //

    if (LpcpGetThreadAttributes(ClientThread) & LPCP_NO_IMPERSONATION) {
        
        LpcpReleaseLpcpLock();

        ObDereferenceObject( PortObject );
        ObDereferenceObject( ClientThread );
        ObDereferenceObject( ConnectedPort );

        return (STATUS_ACCESS_DENIED);
    }

    LpcpReleaseLpcpLock();

    //
    //  If the client requested dynamic security tracking, then the client
    //  security needs to be referenced.  Otherwise, (static case)
    //  it is already in the client's port.
    //

    if (ConnectedPort->Flags & PORT_DYNAMIC_SECURITY) {

        //
        //  Impersonate the client with information from the queued message
        //

        Status = LpcpGetDynamicClientSecurity( ClientThread,
                                               ConnectedPort,
                                               &DynamicSecurity );

        if (!NT_SUCCESS( Status )) {

            ObDereferenceObject( PortObject );
            ObDereferenceObject( ClientThread );
            ObDereferenceObject( ConnectedPort );

            return( Status );
        }

        Status = SeImpersonateClientEx( &DynamicSecurity, NULL );

        LpcpFreeDynamicClientSecurity( &DynamicSecurity );

    } else {

        //
        //  Impersonate the client with information from the client's port
        //

        Status = SeImpersonateClientEx( &ConnectedPort->StaticSecurity, NULL );

    }

    ObDereferenceObject( PortObject );
    ObDereferenceObject( ClientThread );
    ObDereferenceObject( ConnectedPort );

    //
    //  And return to our caller
    //

    return Status;
}


VOID
LpcpFreePortClientSecurity (
    IN PLPCP_PORT_OBJECT Port
    )

/*++

Routine Description:

    This routine cleans up the captured security context for a client port.
    The cleanup is typically done when we are deleting a port

Arguments:

    Port - Supplies the client port being deleted

Return Value:

    None.

--*/

{
    //
    //  We only do this action if supplied with a client communication port
    //

    if ((Port->Flags & PORT_TYPE) == CLIENT_COMMUNICATION_PORT) {

        //
        //  We only do this action if the port has static security tracking,
        //  and we have a captured client token.  The action is to simply
        //  delete the client token.
        //

        if (!(Port->Flags & PORT_DYNAMIC_SECURITY)) {

            if ( Port->StaticSecurity.ClientToken ) {

                SeDeleteClientSecurity( &(Port)->StaticSecurity );
            }
        }
    }

    //
    //  And return to our caller
    //

    return;
}

