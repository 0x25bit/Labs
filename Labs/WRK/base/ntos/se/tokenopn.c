/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    tokenopn.c

Abstract:

   This module implements the open thread and process token services.

--*/

#include "pch.h"

#pragma hdrstop


NTSTATUS
SepCreateImpersonationTokenDacl(
    IN PTOKEN Token,
    IN PACCESS_TOKEN PrimaryToken,
    OUT PACL *Acl
    );

#ifdef ALLOC_PRAGMA
NTSTATUS
SepOpenTokenOfThread(
    IN HANDLE ThreadHandle,
    IN BOOLEAN OpenAsSelf,
    OUT PACCESS_TOKEN *Token,
    OUT PETHREAD *Thread,
    OUT PBOOLEAN CopyOnOpen,
    OUT PBOOLEAN EffectiveOnly,
    OUT PSECURITY_IMPERSONATION_LEVEL ImpersonationLevel
    );
#pragma alloc_text(PAGE,SepCreateImpersonationTokenDacl)
#pragma alloc_text(PAGE,NtOpenProcessToken)
#pragma alloc_text(PAGE,NtOpenProcessTokenEx)
#pragma alloc_text(PAGE,SepOpenTokenOfThread)
#pragma alloc_text(PAGE,NtOpenThreadToken)
#pragma alloc_text(PAGE,NtOpenThreadTokenEx)
#endif



NTSTATUS
SepCreateImpersonationTokenDacl(
    __in PTOKEN Token,
    __in PACCESS_TOKEN PrimaryToken,
    __deref_out PACL *Acl
    )
/*++

Routine Description:

    This routine modifies the DACL protecting the passed token to allow
    the current user (described by the PrimaryToken parameter) full access.
    This permits callers of NtOpenThreadToken to call with OpenAsSelf==TRUE
    and succeed.

    The new DACL placed on the token is as follows:

    ACE 0 - Server gets TOKEN_ALL_ACCESS

    ACE 1 - Client gets TOKEN_ALL_ACCESS

    ACE 2 - Admins gets TOKEN_ALL_ACCESS

    ACE 3 - System gets TOKEN_ALL_ACCESS

    ACE 4 - Restricted gets TOKEN_ALL_ACCESS


Arguments:

    Token - The token whose protection is to be modified.

    PrimaryToken - Token representing the subject to be granted access.

    Acl - Returns the modified ACL, allocated out of PagedPool.


Return Value:


--*/

{
    PSID ServerUserSid;
    PSID ClientUserSid;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG AclLength;
    PACL NewDacl;
    PSECURITY_DESCRIPTOR OldDescriptor;
    BOOLEAN MemoryAllocated;
    PACL OldDacl;
    BOOLEAN DaclPresent;
    BOOLEAN DaclDefaulted;

    PAGED_CODE();

    ServerUserSid = ((PTOKEN)PrimaryToken)->UserAndGroups[0].Sid;

    ClientUserSid = Token->UserAndGroups[0].Sid;

    //
    // Compute how much space we'll need for the new DACL.
    //

    AclLength = 5 * sizeof( ACCESS_ALLOWED_ACE ) - 5 * sizeof( ULONG ) +
                SeLengthSid( ServerUserSid ) + SeLengthSid( SeLocalSystemSid ) +
                SeLengthSid( ClientUserSid ) + SeLengthSid( SeAliasAdminsSid ) +
                SeLengthSid( SeRestrictedSid ) + sizeof( ACL );

    NewDacl = ExAllocatePool( PagedPool, AclLength );

    if (NewDacl == NULL) {

        *Acl = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = RtlCreateAcl( NewDacl, AclLength, ACL_REVISION2 );
    ASSERT(NT_SUCCESS( Status ));

    Status = RtlAddAccessAllowedAce (
                 NewDacl,
                 ACL_REVISION2,
                 TOKEN_ALL_ACCESS,
                 ServerUserSid
                 );
    ASSERT( NT_SUCCESS( Status ));

    Status = RtlAddAccessAllowedAce (
                 NewDacl,
                 ACL_REVISION2,
                 TOKEN_ALL_ACCESS,
                 ClientUserSid
                 );
    ASSERT( NT_SUCCESS( Status ));

    Status = RtlAddAccessAllowedAce (
                 NewDacl,
                 ACL_REVISION2,
                 TOKEN_ALL_ACCESS,
                 SeAliasAdminsSid
                 );
    ASSERT( NT_SUCCESS( Status ));

    Status = RtlAddAccessAllowedAce (
                 NewDacl,
                 ACL_REVISION2,
                 TOKEN_ALL_ACCESS,
                 SeLocalSystemSid
                 );
    ASSERT( NT_SUCCESS( Status ));

    if(ARGUMENT_PRESENT(((PTOKEN)PrimaryToken)->RestrictedSids) ||
       ARGUMENT_PRESENT(Token->RestrictedSids)) {
        Status = RtlAddAccessAllowedAce (
                     NewDacl,
                     ACL_REVISION2,
                     TOKEN_ALL_ACCESS,
                     SeRestrictedSid
                     );
        ASSERT( NT_SUCCESS( Status ));
    }

    *Acl = NewDacl;
    return STATUS_SUCCESS;
}



NTSTATUS
NtOpenProcessToken(
    __in HANDLE ProcessHandle,
    __in ACCESS_MASK DesiredAccess,
    __out PHANDLE TokenHandle
    )

/*++

Routine Description:

    Open a token object associated with a process and return a handle
    that may be used to access that token.

Arguments:

    ProcessHandle - Specifies the process whose token is to be
        opened.

    DesiredAccess - Is an access mask indicating which access types
        are desired to the token.  These access types are reconciled
        with the Discretionary Access Control list of the token to
        determine whether the accesses will be granted or denied.

    TokenHandle - Receives the handle of the newly opened token.

Return Value:

    STATUS_SUCCESS - Indicates the operation was successful.

--*/
{
    return NtOpenProcessTokenEx (ProcessHandle,
                                 DesiredAccess,
                                 0,
                                 TokenHandle);
}

NTSTATUS
NtOpenProcessTokenEx(
    __in HANDLE ProcessHandle,
    __in ACCESS_MASK DesiredAccess,
    __in ULONG HandleAttributes,
    __out PHANDLE TokenHandle
    )

/*++

Routine Description:

    Open a token object associated with a process and return a handle
    that may be used to access that token.

Arguments:

    ProcessHandle - Specifies the process whose token is to be
        opened.

    DesiredAccess - Is an access mask indicating which access types
        are desired to the token.  These access types are reconciled
        with the Discretionary Access Control list of the token to
        determine whether the accesses will be granted or denied.

    HandleAttributes - Attributes for the created handle. Only OBJ_KERNEL_HANDLE at present.

    TokenHandle - Receives the handle of the newly opened token.

Return Value:

    STATUS_SUCCESS - Indicates the operation was successful.

--*/
{

    PVOID Token;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;

    HANDLE LocalHandle;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    //
    // Sanitize the handle attribute flags
    //
    HandleAttributes = ObSanitizeHandleAttributes (HandleAttributes, PreviousMode);

    //
    //  Probe parameters
    //

    if (PreviousMode != KernelMode) {

        try {

            ProbeForWriteHandle(TokenHandle);

        } except(EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }  // end_try

    } //end_if


    //
    // Validate access to the process and obtain a pointer to the
    // process's token.  If successful, this will cause the token's
    // reference count to be incremented.
    //

    Status = PsOpenTokenOfProcess( ProcessHandle, ((PACCESS_TOKEN *)&Token));

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    //
    //  Now try to open the token for the specified desired access
    //

    Status = ObOpenObjectByPointer(
                 (PVOID)Token,         // Object
                 HandleAttributes,     // HandleAttributes
                 NULL,                 // AccessState
                 DesiredAccess,        // DesiredAccess
                 SeTokenObjectType,   // ObjectType
                 PreviousMode,         // AccessMode
                 &LocalHandle          // Handle
                 );

    //
    //  And decrement the reference count of the token to counter
    //  the action performed by PsOpenTokenOfProcess().  If the open
    //  was successful, the handle will have caused the token's
    //  reference count to have been incremented.
    //

    ObDereferenceObject( Token );

    //
    //  Return the new handle
    //

    if (NT_SUCCESS(Status)) {

        try {

            *TokenHandle = LocalHandle;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            return GetExceptionCode();

        }
    }

    return Status;

}

NTSTATUS
SepOpenTokenOfThread(
    __in HANDLE ThreadHandle,
    __in BOOLEAN OpenAsSelf,
    __deref_out PACCESS_TOKEN *Token,
    __deref_out PETHREAD *Thread,
    __out PBOOLEAN CopyOnOpen,
    __out PBOOLEAN EffectiveOnly,
    __out PSECURITY_IMPERSONATION_LEVEL ImpersonationLevel
    )

/*++

Routine Description:

    This function does the thread specific processing of
    an NtOpenThreadToken() service.

    The service validates that the handle has appropriate access
    to reference the thread.  If so, it goes on to increment
    the reference count of the token object to prevent it from
    going away while the rest of the NtOpenThreadToken() request
    is processed.

    NOTE: If this call completes successfully, the caller is responsible
          for decrementing the reference count of the target token.
          This must be done using PsDereferenceImpersonationToken().

Arguments:

    ThreadHandle - Supplies a handle to a thread object.

    OpenAsSelf - Is a boolean value indicating whether the access should
        be made using the calling thread's current security context, which
        may be that of a client (if impersonating), or using the caller's
        process-level security context.  A value of FALSE indicates the
        caller's current context should be used un-modified.  A value of
        TRUE indicates the request should be fulfilled using the process
        level security context.

    Token - If successful, receives a pointer to the thread's token
        object.

    CopyOnOpen - The current value of the Thread->Client->CopyOnOpen field.

    EffectiveOnly - The current value of the Thread->Client->EffectiveOnly field.

    ImpersonationLevel - The current value of the Thread->Client->ImpersonationLevel
        field.

Return Value:

    STATUS_SUCCESS - Indicates the call completed successfully.

    STATUS_NO_TOKEN - Indicates the referenced thread is not currently
        impersonating a client.

    STATUS_CANT_OPEN_ANONYMOUS - Indicates the client requested anonymous
        impersonation level.  An anonymous token can not be opened.

    status may also be any value returned by an attempt the reference
    the thread object for THREAD_QUERY_INFORMATION access.

--*/

{

    NTSTATUS
        Status;

    KPROCESSOR_MODE
        PreviousMode;

    SE_IMPERSONATION_STATE
        DisabledImpersonationState;

    BOOLEAN
        RestoreImpersonationState = FALSE;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();


    //
    //  Make sure the handle grants the appropriate access to the specified
    //  thread.
    //

    Status = ObReferenceObjectByHandle(
                 ThreadHandle,
                 THREAD_QUERY_INFORMATION,
                 PsThreadType,
                 PreviousMode,
                 (PVOID *)Thread,
                 NULL
                 );

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    //
    //  Reference the impersonation token, if there is one
    //

    (*Token) = PsReferenceImpersonationToken( *Thread,
                                              CopyOnOpen,
                                              EffectiveOnly,
                                              ImpersonationLevel
                                              );




    //
    // Make sure there is a token
    //

    if ((*Token) == NULL) {
        ObDereferenceObject( *Thread );
        (*Thread) = NULL;
        return STATUS_NO_TOKEN;
    }


    //
    //  Make sure the ImpersonationLevel is high enough to allow
    //  the token to be opened.
    //

    if ((*ImpersonationLevel) <= SecurityAnonymous) {
        PsDereferenceImpersonationToken( (*Token) );
        ObDereferenceObject( *Thread );
        (*Thread) = NULL;
        (*Token) = NULL;
        return STATUS_CANT_OPEN_ANONYMOUS;
    }


    return STATUS_SUCCESS;

}


NTSTATUS
NtOpenThreadToken(
    __in HANDLE ThreadHandle,
    __in ACCESS_MASK DesiredAccess,
    __in BOOLEAN OpenAsSelf,
    __out PHANDLE TokenHandle
    )

/*++


Routine Description:

Open a token object associated with a thread and return a handle that
may be used to access that token.

Arguments:

    ThreadHandle - Specifies the thread whose token is to be opened.

    DesiredAccess - Is an access mask indicating which access types
        are desired to the token.  These access types are reconciled
        with the Discretionary Access Control list of the token to
        determine whether the accesses will be granted or denied.

    OpenAsSelf - Is a boolean value indicating whether the access should
        be made using the calling thread's current security context, which
        may be that of a client if impersonating, or using the caller's
        process-level security context.  A value of FALSE indicates the
        caller's current context should be used un-modified.  A value of
        TRUE indicates the request should be fulfilled using the process
        level security context.

        This parameter is necessary to allow a server process to open
        a client's token when the client specified IDENTIFICATION level
        impersonation.  In this case, the caller would not be able to
        open the client's token using the client's context (because you
        can't create executive level objects using IDENTIFICATION level
        impersonation).

    TokenHandle - Receives the handle of the newly opened token.

Return Value:

    STATUS_SUCCESS - Indicates the operation was successful.

    STATUS_NO_TOKEN - Indicates an attempt has been made to open a
        token associated with a thread that is not currently
        impersonating a client.

    STATUS_CANT_OPEN_ANONYMOUS - Indicates the client requested anonymous
        impersonation level.  An anonymous token can not be opened.

--*/
{
    return NtOpenThreadTokenEx (ThreadHandle,
                                DesiredAccess,
                                OpenAsSelf,
                                0,
                                TokenHandle);
}

NTSTATUS
NtOpenThreadTokenEx(
    __in HANDLE ThreadHandle,
    __in ACCESS_MASK DesiredAccess,
    __in BOOLEAN OpenAsSelf,
    __in ULONG HandleAttributes,
    __out PHANDLE TokenHandle
    )

/*++


Routine Description:

Open a token object associated with a thread and return a handle that
may be used to access that token.

Arguments:

    ThreadHandle - Specifies the thread whose token is to be opened.

    DesiredAccess - Is an access mask indicating which access types
        are desired to the token.  These access types are reconciled
        with the Discretionary Access Control list of the token to
        determine whether the accesses will be granted or denied.

    OpenAsSelf - Is a boolean value indicating whether the access should
        be made using the calling thread's current security context, which
        may be that of a client if impersonating, or using the caller's
        process-level security context.  A value of FALSE indicates the
        caller's current context should be used un-modified.  A value of
        TRUE indicates the request should be fulfilled using the process
        level security context.

        This parameter is necessary to allow a server process to open
        a client's token when the client specified IDENTIFICATION level
        impersonation.  In this case, the caller would not be able to
        open the client's token using the client's context (because you
        can't create executive level objects using IDENTIFICATION level
        impersonation).

    HandleAttributes - Attributes applied to the handle OBJ_KERNEL_HANDLE

    TokenHandle - Receives the handle of the newly opened token.

Return Value:

    STATUS_SUCCESS - Indicates the operation was successful.

    STATUS_NO_TOKEN - Indicates an attempt has been made to open a
        token associated with a thread that is not currently
        impersonating a client.

    STATUS_CANT_OPEN_ANONYMOUS - Indicates the client requested anonymous
        impersonation level.  An anonymous token can not be opened.

--*/
{

    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;

    PVOID Token;
    PTOKEN NewToken = NULL;
    BOOLEAN CopyOnOpen;
    BOOLEAN EffectiveOnly;
    SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;
    SE_IMPERSONATION_STATE DisabledImpersonationState;
    BOOLEAN RestoreImpersonationState = FALSE;

    HANDLE LocalHandle = NULL;
    SECURITY_DESCRIPTOR SecurityDescriptor;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PACL NewAcl = NULL;
    PETHREAD Thread = NULL;
    PETHREAD OriginalThread = NULL;
    PACCESS_TOKEN PrimaryToken;
    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    //
    // Sanitize the handle attribute flags
    //

    HandleAttributes = ObSanitizeHandleAttributes (HandleAttributes, PreviousMode);

    //
    //  Probe parameters
    //

    if (PreviousMode != KernelMode) {

        try {

            ProbeForWriteHandle(TokenHandle);

        } except(EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }  // end_try

    } //end_if

    //
    // Validiate access to the thread and obtain a pointer to the
    // thread's token (if there is one).  If successful, this will
    // cause the token's reference count to be incremented.
    //
    // This routine disabled impersonation as necessary to properly
    // honor the OpenAsSelf flag.
    //

    Status = SepOpenTokenOfThread( ThreadHandle,
                                  OpenAsSelf,
                                  ((PACCESS_TOKEN *)&Token),
                                  &OriginalThread,
                                  &CopyOnOpen,
                                  &EffectiveOnly,
                                  &ImpersonationLevel
                                  );

    if (!NT_SUCCESS(Status)) {
        return Status;
    }


    //
    //  The token was successfully referenced.
    //

    //
    // We need to create and/or open a token object, so disable impersonation
    // if necessary.
    //

    if (OpenAsSelf) {
         RestoreImpersonationState = PsDisableImpersonation(
                                         PsGetCurrentThread(),
                                         &DisabledImpersonationState
                                         );
    }

    //
    //  If the CopyOnOpen flag is not set, then the token can be
    //  opened directly.  Otherwise, the token must be duplicated,
    //  and a handle to the duplicate returned.
    //

    if (CopyOnOpen) {

        //
        // Create the new security descriptor for the token.
        //
        // We must obtain the correct SID to put into the Dacl.  Do this
        // by finding the process associated with the passed thread
        // and grabbing the User SID out of that process's token.
        // If we just use the current SubjectContext, we'll get the
        // SID of whoever is calling us, which isn't what we want.
        //

        Status = ObReferenceObjectByHandle(
                     ThreadHandle,
                     THREAD_ALL_ACCESS,
                     PsThreadType,
                     KernelMode,
                     (PVOID)&Thread,
                     NULL
                     );

        //
        // Verify that the handle is still pointer to the same thread
        //

        if (NT_SUCCESS(Status) && (Thread != OriginalThread)) {
            Status = STATUS_OBJECT_TYPE_MISMATCH;
        }

        if (NT_SUCCESS(Status)) {
            PEPROCESS Process = THREAD_TO_PROCESS(Thread);

            PrimaryToken = PsReferencePrimaryToken(Process);

            Status = SepCreateImpersonationTokenDacl(
                         (PTOKEN)Token,
                         PrimaryToken,
                         &NewAcl
                         );

            PsDereferencePrimaryTokenEx( Process, PrimaryToken );

            if (NT_SUCCESS( Status )) {

                if (NewAcl != NULL) {

                    //
                    // There exist tokens that either do not have security descriptors at all,
                    // or have security descriptors, but do not have DACLs.  In either case, do
                    // nothing.
                    //

                    Status = RtlCreateSecurityDescriptor ( &SecurityDescriptor, SECURITY_DESCRIPTOR_REVISION );
                    ASSERT( NT_SUCCESS( Status ));

                    Status = RtlSetDaclSecurityDescriptor (
                                 &SecurityDescriptor,
                                 TRUE,
                                 NewAcl,
                                 FALSE
                                 );

                    ASSERT( NT_SUCCESS( Status ));
                }

                InitializeObjectAttributes(
                    &ObjectAttributes,
                    NULL,
                    HandleAttributes,
                    NULL,
                    NewAcl == NULL ? NULL : &SecurityDescriptor
                    );

                //
                // Open a copy of the token
                //

                Status = SepDuplicateToken(
                             (PTOKEN)Token,        // ExistingToken
                             &ObjectAttributes,    // ObjectAttributes
                             EffectiveOnly,        // EffectiveOnly
                             TokenImpersonation,   // TokenType
                             ImpersonationLevel,   // ImpersonationLevel
                             KernelMode,           // RequestorMode must be kernel mode
                             &NewToken
                             );

                if (NT_SUCCESS( Status )) {

                    //
                    // Reference the token so it doesn't go away
                    //

                    ObReferenceObject(NewToken);

                    //
                    //  Insert the new token
                    //

                    Status = ObInsertObject( NewToken,
                                             NULL,
                                             DesiredAccess,
                                             0,
                                             (PVOID *)NULL,
                                             &LocalHandle
                                             );
                }
            }
        }


    } else {

        //
        // We do not have to modify the security on the token in the static case,
        // because in all the places in the system where impersonation takes place
        // over a secure transport (e.g., LPC), CopyOnOpen is set.  The only reason
        // we'be be here is if the impersonation is taking place because someone did
        // an NtSetInformationThread and passed in a token.
        //
        // In that case, we absolutely do not want to give the caller guaranteed
        // access, because that would allow anyone who has access to a thread to
        // impersonate any of that thread's clients for any access.
        //

        //
        //  Open the existing token
        //

        Status = ObOpenObjectByPointer(
                     (PVOID)Token,         // Object
                     HandleAttributes,     // HandleAttributes
                     NULL,                 // AccessState
                     DesiredAccess,        // DesiredAccess
                     SeTokenObjectType,   // ObjectType
                     PreviousMode,         // AccessMode
                     &LocalHandle          // Handle
                     );
    }

    if (NewAcl != NULL) {
        ExFreePool( NewAcl );
    }

    if (RestoreImpersonationState) {
        PsRestoreImpersonation(
            PsGetCurrentThread(),
            &DisabledImpersonationState
            );
    }

    //
    //  And decrement the reference count of the existing token to counter
    //  the action performed by PsOpenTokenOfThread.  If the open
    //  was successful, the handle will have caused the token's
    //  reference count to have been incremented.
    //

    ObDereferenceObject( Token );

    if (NT_SUCCESS( Status ) && CopyOnOpen) {

        //
        // Assign the newly duplicated token to the thread.
        //

        PsImpersonateClient( Thread,
                             NewToken,
                             FALSE,  // turn off CopyOnOpen flag
                             EffectiveOnly,
                             ImpersonationLevel
                             );

    }

    //
    // We've impersonated the token so let go of our reference
    //

    if (NewToken != NULL) {
        ObDereferenceObject( NewToken );
    }

    if (CopyOnOpen && (Thread != NULL)) {

        ObDereferenceObject( Thread );
    }

    if (OriginalThread != NULL) {
        ObDereferenceObject(OriginalThread);
    }

    //
    //  Return the new handle
    //

    if (NT_SUCCESS(Status)) {
        try {
            *TokenHandle = LocalHandle;
        } except(EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    return Status;
}

