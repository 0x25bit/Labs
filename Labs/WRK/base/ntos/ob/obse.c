/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    obse.c

Abstract:

    Object Security API calls

--*/

#include "obp.h"

#if defined(ALLOC_PRAGMA)

#pragma alloc_text(PAGE,NtSetSecurityObject)
#pragma alloc_text(PAGE,NtQuerySecurityObject)
#pragma alloc_text(PAGE,ObAssignObjectSecurityDescriptor)
#pragma alloc_text(PAGE,ObAssignSecurity)
#pragma alloc_text(PAGE,ObCheckCreateObjectAccess)
#pragma alloc_text(PAGE,ObCheckObjectAccess)
#pragma alloc_text(PAGE,ObpCheckObjectReference)
#pragma alloc_text(PAGE,ObpCheckTraverseAccess)
#pragma alloc_text(PAGE,ObGetObjectSecurity)
#pragma alloc_text(PAGE,ObSetSecurityDescriptorInfo)
#pragma alloc_text(PAGE,ObQuerySecurityDescriptorInfo)
#pragma alloc_text(PAGE,ObReleaseObjectSecurity)
#pragma alloc_text(PAGE,ObValidateSecurityQuota)
#pragma alloc_text(PAGE,ObpValidateAccessMask)
#pragma alloc_text(PAGE,ObSetSecurityObjectByPointer)

#endif

ULONG ObpDefaultSecurityDescriptorLength = 256;

NTSTATUS
NtSetSecurityObject (
    __in HANDLE Handle,
    __in SECURITY_INFORMATION SecurityInformation,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor
    )

/*++

Routine Description:

    This routine is used to invoke an object's security routine.  It
    is used to set the object's security state.

Arguments:

    Handle - Supplies the handle for the object being modified

    SecurityInformation - Indicates the type of information we are
        interested in setting. e.g., owner, group, dacl, or sacl.

    SecurityDescriptor - Supplies the security descriptor for the
        object being modified.

Return Value:

    An appropriate NTSTATUS value

--*/

{
    NTSTATUS Status;
    PVOID Object;
    ACCESS_MASK DesiredAccess;
    OBJECT_HANDLE_INFORMATION HandleInformation;
    KPROCESSOR_MODE RequestorMode;
    SECURITY_DESCRIPTOR_RELATIVE *CapturedDescriptor;

    PAGED_CODE();

    //
    //  Make sure the passed security descriptor is really there.
    //  SeCaptureSecurityDescriptor doesn't mind being passed a NULL
    //  SecurityDescriptor, and will just return NULL back.
    //

    if (!ARGUMENT_PRESENT( SecurityDescriptor )) {

        return( STATUS_ACCESS_VIOLATION );
    }

    //
    //  Establish the accesses needed to the object based upon the
    //  security information being modified.
    //

    SeSetSecurityAccessMask( SecurityInformation, &DesiredAccess );

    Status = ObReferenceObjectByHandle( Handle,
                                        DesiredAccess,
                                        NULL,
                                        RequestorMode = KeGetPreviousMode(),
                                        &Object,
                                        &HandleInformation );

    if (NT_SUCCESS( Status )) {

        //
        //  Probe and capture the input security descriptor, and return
        //  right away if it is ill-formed.
        //
        //  Because the security descriptor is always captured the returned
        //  security descriptor is in self-relative format.
        //

        Status = SeCaptureSecurityDescriptor( SecurityDescriptor,
                                              RequestorMode,
                                              PagedPool,
                                              TRUE,
                                              (PSECURITY_DESCRIPTOR *)&CapturedDescriptor );

        if (NT_SUCCESS( Status )) {

            //
            //  Now check for a valid combination of what the user wants to set
            //  and what was supplied in the input security descriptor.  If the
            //  caller wants to set the owner then the owner field of the
            //  security descriptor better not be null, likewise for the group
            //  setting.  If anything is missing we'll return and error.
            //

            ASSERT(CapturedDescriptor->Control & SE_SELF_RELATIVE);

            if (((SecurityInformation & OWNER_SECURITY_INFORMATION) &&
                (CapturedDescriptor->Owner == 0))

                ||

                ((SecurityInformation & GROUP_SECURITY_INFORMATION) &&
                (CapturedDescriptor->Group == 0))) {

                SeReleaseSecurityDescriptor( (PSECURITY_DESCRIPTOR)CapturedDescriptor,
                                             RequestorMode,
                                             TRUE );

                ObDereferenceObject( Object );

                return( STATUS_INVALID_SECURITY_DESCR );
            }

            Status = ObSetSecurityObjectByPointer( Object,
                                                   SecurityInformation,
                                                   CapturedDescriptor );

            SeReleaseSecurityDescriptor( (PSECURITY_DESCRIPTOR)CapturedDescriptor,
                                         RequestorMode,
                                         TRUE );
        }

        ObDereferenceObject( Object );

    }

    return( Status );
}


NTSTATUS
ObSetSecurityObjectByPointer (
    __in PVOID Object,
    __in SECURITY_INFORMATION SecurityInformation,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor
    )

/*++

Routine Description:

    This routine is used to invoke an object's security routine.  It
    is used to set the object's security state.

    This routine is accessible only to the kernel and assumes that all
    necessary validation of parameters has been done by the caller.

Arguments:

    Object - Supplies the pointer for the object being modified

    SecurityInformation - Indicates the type of information we are
        interested in setting. e.g., owner, group, dacl, or sacl.

    SecurityDescriptor - Supplies the security descriptor for the
        object being modified.

Return Value:

    An appropriate NTSTATUS value

--*/

{
    NTSTATUS Status;
    POBJECT_HEADER ObjectHeader;
    POBJECT_TYPE ObjectType;

    PAGED_CODE();

//    DbgPrint("ObSetSecurityObjectByPointer called for object %#08lx with info "
//             "%x and descriptor %#08lx\n",
//             Object, SecurityInformation, SecurityDescriptor);

    //
    //  Map the object body to an object header and the corresponding
    //  object type
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );
    ObjectType = ObjectHeader->Type;

    //
    //  Make sure the passed security descriptor is really there.
    //

    ASSERT(ARGUMENT_PRESENT( SecurityDescriptor ));

    //
    //  Now invoke the security procedure call back to set the security
    //  descriptor for the object
    //

    Status = (ObjectType->TypeInfo.SecurityProcedure)
                ( Object,
                  SetSecurityDescriptor,
                  &SecurityInformation,
                  SecurityDescriptor,
                  NULL,
                  &ObjectHeader->SecurityDescriptor,
                  ObjectType->TypeInfo.PoolType,
                  &ObjectType->TypeInfo.GenericMapping );


//    DbgPrint("ObSetSecurityObjectByPointer: object security routine returned "
//             "%#08lx\n", Status);

    return( Status );
}

NTSTATUS
NtQuerySecurityObject (
    __in HANDLE Handle,
    __in SECURITY_INFORMATION SecurityInformation,
    __out_bcount_opt(Length) PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in ULONG Length,
    __out PULONG LengthNeeded
    )

/*++

Routine Description:

    This routine is used to query the security descriptor for an
    object.

Arguments:

    Handle - Supplies the handle for the object being investigated

    SecurityInformation - Indicates the type of information we are
        interested in getting. e.g., owner, group, dacl, or sacl.

    SecurityDescriptor - Supplies a pointer to where the information
        should be returned

    Length - Supplies the size, in bytes, of the output buffer

    LengthNeeded - Receives the length, in bytes, needed to store
        the output security descriptor

Return Value:

    An appropriate NTSTATUS value

--*/

{
    NTSTATUS Status;
    PVOID Object;
    ACCESS_MASK DesiredAccess;
    OBJECT_HANDLE_INFORMATION HandleInformation;
    KPROCESSOR_MODE RequestorMode;
    POBJECT_HEADER ObjectHeader;
    POBJECT_TYPE ObjectType;

    PAGED_CODE();

    //
    //  Probe output parameters
    //

    RequestorMode = KeGetPreviousMode();

    if (RequestorMode != KernelMode) {

        try {

            ProbeForWriteUlong( LengthNeeded );

            ProbeForWrite( SecurityDescriptor, Length, sizeof(ULONG) );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            return GetExceptionCode();
        }
    }

    //
    //  Establish the accesses needed to the object based upon the
    //  security information being queried
    //

    SeQuerySecurityAccessMask( SecurityInformation, &DesiredAccess );

    Status = ObReferenceObjectByHandle( Handle,
                                        DesiredAccess,
                                        NULL,
                                        RequestorMode,
                                        &Object,
                                        &HandleInformation );

    if (!NT_SUCCESS( Status )) {

        return( Status );
    }

    //
    //  Map the object body to an object header and the corresponding
    //  object type
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );
    ObjectType = ObjectHeader->Type;

    //
    //  Invoke the object type's security callback routine to query
    //  the object.  This routine is assumed to have a try-except around
    //  the setting of the output security descriptor
    //

    Status = (ObjectType->TypeInfo.SecurityProcedure)( Object,
                                                       QuerySecurityDescriptor,
                                                       &SecurityInformation,
                                                       SecurityDescriptor,
                                                       &Length,
                                                       &ObjectHeader->SecurityDescriptor,
                                                       ObjectType->TypeInfo.PoolType,
                                                       &ObjectType->TypeInfo.GenericMapping );

    //
    //  Indicate the length needed for the security descriptor.  This
    //  will be set even if the callback failed so the caller will know
    //  the number of bytes necessary
    //

    try {

        *LengthNeeded = Length;

    } except(EXCEPTION_EXECUTE_HANDLER) {

        ObDereferenceObject( Object );

        return(GetExceptionCode());
    }

    //
    //  And return to our caller
    //

    ObDereferenceObject( Object );

    return( Status );
}


BOOLEAN
ObCheckObjectAccess (
    __in PVOID Object,
    __inout PACCESS_STATE AccessState,
    __in BOOLEAN TypeMutexLocked,
    __in KPROCESSOR_MODE AccessMode,
    __out PNTSTATUS AccessStatus
    )

/*++

Routine Description:

    This routine performs access validation on the passed object.  The
    remaining desired access mask is extracted from the AccessState
    parameter and passes to the appropriate security routine to perform the
    access check.

    If the access attempt is successful, SeAccessCheck returns a mask
    containing the granted accesses.  The bits in this mask are turned
    on in the PreviouslyGrantedAccess field of the AccessState, and
    are turned off in the RemainingDesiredAccess field.

Arguments:

    Object - The object being examined.

    AccessState - The ACCESS_STATE structure containing accumulated
        information about the current attempt to gain access to the object.

    TypeMutexLocked - Indicates whether the type mutex for this object's
        type is locked.  The type mutex is used to protect the object's
        security descriptor from being modified while it is being accessed.

    AccessMode - The previous processor mode.

    AccessStatus - Pointer to a variable to return the status code of the
        access attempt.  In the case of failure this status code must be
        propagated back to the user.


Return Value:

    BOOLEAN - TRUE if access is allowed and FALSE otherwise

--*/

{
    ACCESS_MASK GrantedAccess = 0;
    BOOLEAN AccessAllowed;
    BOOLEAN MemoryAllocated;
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR SecurityDescriptor = NULL;
    POBJECT_HEADER ObjectHeader;
    POBJECT_TYPE ObjectType;
    PPRIVILEGE_SET Privileges = NULL;

    PAGED_CODE();

    UNREFERENCED_PARAMETER (TypeMutexLocked);

    //
    //  Map the object body to an object header and the
    //  corresponding object type
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );
    ObjectType = ObjectHeader->Type;

    //
    //  Obtain the object's security descriptor
    //

    Status = ObGetObjectSecurity( Object,
                                  &SecurityDescriptor,
                                  &MemoryAllocated );

    //
    //  If we failed in getting the security descriptor then
    //  put the object type lock back where it was and return
    //  the error back to our caller
    //

    if (!NT_SUCCESS( Status )) {

        *AccessStatus = Status;

        return( FALSE );

    } else {

        //
        //  Otherwise we've been successful at getting the
        //  object's security descriptor, but now make sure
        //  it is not null.

        if (SecurityDescriptor == NULL) {

            *AccessStatus = Status;

            return(TRUE);
        }
    }

    //
    //  We have a non-null security descriptor so now
    //  lock the caller's tokens until after auditing has been
    //  performed.
    //

    SeLockSubjectContext( &AccessState->SubjectSecurityContext );

    //
    //  Do the access check, and if we have some privileges then
    //  put those in the access state too.
    //

    AccessAllowed = SeAccessCheck( SecurityDescriptor,
                                   &AccessState->SubjectSecurityContext,
                                   TRUE,                        // Tokens are locked
                                   AccessState->RemainingDesiredAccess,
                                   AccessState->PreviouslyGrantedAccess,
                                   &Privileges,
                                   &ObjectType->TypeInfo.GenericMapping,
                                   AccessMode,
                                   &GrantedAccess,
                                   AccessStatus );

    if (Privileges != NULL) {

        Status = SeAppendPrivileges( AccessState,
                                     Privileges );

        SeFreePrivileges( Privileges );
    }

    //
    //  If we were granted access then set that fact into
    //  what we've been granted and remove it from what remains
    //  to be granted.
    //

    if (AccessAllowed) {

        AccessState->PreviouslyGrantedAccess |= GrantedAccess;
        AccessState->RemainingDesiredAccess &= ~(GrantedAccess | MAXIMUM_ALLOWED);
    }

    //
    //  Audit the attempt to open the object, audit
    //  the creation of its handle later.
    //

    if ( SecurityDescriptor != NULL ) {

        SeOpenObjectAuditAlarm( &ObjectType->Name,
                                Object,
                                NULL,                    // AbsoluteObjectName
                                SecurityDescriptor,
                                AccessState,
                                FALSE,                   // ObjectCreated (FALSE, only open here)
                                AccessAllowed,
                                AccessMode,
                                &AccessState->GenerateOnClose );
    }

    SeUnlockSubjectContext( &AccessState->SubjectSecurityContext );

    //
    //  Free the security descriptor before returning to
    //  our caller
    //

    ObReleaseObjectSecurity( SecurityDescriptor,
                             MemoryAllocated );

    return( AccessAllowed );
}


BOOLEAN
ObpCheckObjectReference (
    IN PVOID Object,
    IN OUT PACCESS_STATE AccessState,
    IN BOOLEAN TypeMutexLocked,
    IN KPROCESSOR_MODE AccessMode,
    OUT PNTSTATUS AccessStatus
    )

/*++

Routine Description:

    The routine performs access validation on the passed object.  The
    remaining desired access mask is extracted from the AccessState
    parameter and passes to the appropriate security routine to
    perform the access check.

    If the access attempt is successful, SeAccessCheck returns a mask
    containing the granted accesses.  The bits in this mask are turned
    on in the PreviouslyGrantedAccess field of the AccessState, and
    are turned off in the RemainingDesiredAccess field.

    This routine differs from ObpCheckObjectAccess in that it calls
    a different audit routine.

Arguments:

    Object - The object being examined.

    AccessState - The ACCESS_STATE structure containing accumulated
        information about the current attempt to gain access to the object.

    TypeMutexLocked - Indicates whether the type mutex for this object's
        type is locked.  The type mutex is used to protect the object's
        security descriptor from being modified while it is being accessed.

    AccessMode - The previous processor mode.

    AccessStatus - Pointer to a variable to return the status code of the
        access attempt.  In the case of failure this status code must be
        propagated back to the user.


Return Value:

    BOOLEAN - TRUE if access is allowed and FALSE otherwise

--*/

{
    BOOLEAN AccessAllowed;
    ACCESS_MASK GrantedAccess = 0;
    BOOLEAN MemoryAllocated;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    NTSTATUS Status;
    POBJECT_HEADER ObjectHeader;
    POBJECT_TYPE ObjectType;
    PPRIVILEGE_SET Privileges = NULL;

    PAGED_CODE();

    UNREFERENCED_PARAMETER (TypeMutexLocked);

    //
    //  Map the object body to an object header and the
    //  corresponding object type
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );
    ObjectType = ObjectHeader->Type;

    //
    //  Obtain the object's security descriptor
    //

    Status = ObGetObjectSecurity( Object,
                                  &SecurityDescriptor,
                                  &MemoryAllocated );

    //
    //  If we failed in getting the security descriptor then
    //  put the object type lock back where it was and return
    //  the error back to our caller
    //

    if (!NT_SUCCESS( Status )) {

        *AccessStatus = Status;

        return( FALSE );
    }

    //
    //  Lock the caller's tokens until after auditing has been
    //  performed.
    //

    SeLockSubjectContext( &AccessState->SubjectSecurityContext );

    //
    //  Do the access check, and if we have some privileges then
    //  put those in the access state too.
    //

    AccessAllowed = SeAccessCheck( SecurityDescriptor,
                                   &AccessState->SubjectSecurityContext,
                                   TRUE,               // Tokens are locked
                                   AccessState->RemainingDesiredAccess,
                                   AccessState->PreviouslyGrantedAccess,
                                   &Privileges,
                                   &ObjectType->TypeInfo.GenericMapping,
                                   AccessMode,
                                   &GrantedAccess,
                                   AccessStatus );

    if (AccessAllowed) {

        AccessState->PreviouslyGrantedAccess |= GrantedAccess;
        AccessState->RemainingDesiredAccess &= ~GrantedAccess;
    }

    //
    //  If we have a security descriptor then call the security routine
    //  to audit this reference and then unlock the caller's token
    //

    if ( SecurityDescriptor != NULL ) {

        SeObjectReferenceAuditAlarm( &AccessState->OperationID,
                                     Object,
                                     SecurityDescriptor,
                                     &AccessState->SubjectSecurityContext,
                                     AccessState->RemainingDesiredAccess | AccessState->PreviouslyGrantedAccess,
                                     ((PAUX_ACCESS_DATA)(AccessState->AuxData))->PrivilegesUsed,
                                     AccessAllowed,
                                     AccessMode );
    }

    SeUnlockSubjectContext( &AccessState->SubjectSecurityContext );

    //
    //  Finally free the security descriptor
    //  and return to our caller
    //

    ObReleaseObjectSecurity( SecurityDescriptor,
                             MemoryAllocated );

    return( AccessAllowed );
}


BOOLEAN
ObpCheckTraverseAccess (
    IN PVOID DirectoryObject,
    IN ACCESS_MASK TraverseAccess,
    IN PACCESS_STATE AccessState,
    IN BOOLEAN TypeMutexLocked,
    IN KPROCESSOR_MODE PreviousMode,
    OUT PNTSTATUS AccessStatus
    )

/*++

Routine Description:

    This routine checks for traverse access to the given directory object.

    Note that the contents of the AccessState structure are not
    modified, since it is assumed that this access check is incidental
    to another access operation.

Arguments:

    DirectoryObject - The object body of the object being examined.

    TraverseAccess - The desired access to the object, most likely DIRECTORY
        TRAVERSE access.

    AccessState - Checks for traverse access will typically be incidental
        to some other access attempt.  Information on the current state of
        that access attempt is required so that the constituent access
        attempts may be associated with each other in the audit log.

    TypeMutexLocked - Indicates whether the type mutex for this object's
        type is locked.  The type mutex is used to protect the object's
        security descriptor from being modified while it is being accessed.

    PreviousMode - The previous processor mode.

    AccessStatus - Pointer to a variable to return the status code of the
        access attempt.  In the case of failure this status code must be
        propagated back to the user.

Return Value:

    BOOLEAN - TRUE if access is allowed and FALSE otherwise.  AccessStatus
    contains the status code to be passed back to the caller.  It is not
    correct to simply pass back STATUS_ACCESS_DENIED, since this will have
    to change with the advent of mandatory access control.

--*/

{
    BOOLEAN AccessAllowed;
    ACCESS_MASK GrantedAccess = 0;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    BOOLEAN MemoryAllocated;
    NTSTATUS Status;
    POBJECT_HEADER ObjectHeader;
    POBJECT_TYPE ObjectType;
    PPRIVILEGE_SET Privileges = NULL;

    PAGED_CODE();

    UNREFERENCED_PARAMETER (TypeMutexLocked);

    //
    //  Map the object body to an object header and corresponding
    //  object type
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( DirectoryObject );
    ObjectType = ObjectHeader->Type;

    //
    //  Obtain the object's security descriptor and make it was
    //  successful
    //

    Status = ObGetObjectSecurity( DirectoryObject,
                                  &SecurityDescriptor,
                                  &MemoryAllocated );

    if (!NT_SUCCESS( Status )) {

        *AccessStatus = Status;

        return( FALSE );
    }

    if (!SeFastTraverseCheck( SecurityDescriptor,
                              AccessState,
                              DIRECTORY_TRAVERSE,
                              PreviousMode )) {

        //
        //  SeFastTraverseCheck could be modified to tell us that
        //  no one has any access to this directory.  However,
        //  we're going to have to fail this entire call if
        //  that is the case, so we really don't need to worry
        //  all that much about making it blindingly fast.
        //
        //  The world does not have traverse access and we have
        //  the client's access state so lock down the client's
        //  token and then do the access check, appending privileges
        //  if present.  The access check will give the answer
        //  we return back to our caller
        //

        SeLockSubjectContext( &AccessState->SubjectSecurityContext );

        AccessAllowed = SeAccessCheck( SecurityDescriptor,
                                       &AccessState->SubjectSecurityContext,
                                       TRUE,             // Tokens are locked
                                       TraverseAccess,
                                       0,
                                       &Privileges,
                                       &ObjectType->TypeInfo.GenericMapping,
                                       PreviousMode,
                                       &GrantedAccess,
                                       AccessStatus );

        if (Privileges != NULL) {

            Status = SeAppendPrivileges( AccessState,
                                         Privileges );

            SeFreePrivileges( Privileges );
        }

        //
        //  If the client's token is locked then now we can unlock it
        //

        SeUnlockSubjectContext( &AccessState->SubjectSecurityContext );

    } else {

        //
        //  At this point the world has traverse access
        //

        AccessAllowed = TRUE;
    }

    //
    //  Finally free the security descriptor
    //  and then return to our caller
    //

    ObReleaseObjectSecurity( SecurityDescriptor,
                             MemoryAllocated );

    return( AccessAllowed );
}


BOOLEAN
ObCheckCreateObjectAccess (
    __in PVOID DirectoryObject,
    __in ACCESS_MASK CreateAccess,
    __in PACCESS_STATE AccessState,
    __in PUNICODE_STRING ComponentName,
    __in BOOLEAN TypeMutexLocked,
    __in KPROCESSOR_MODE PreviousMode,
    __out PNTSTATUS AccessStatus
    )

/*++

Routine Description:

    This routine checks to see if we are allowed to create an object in the
    given directory, and performs auditing as appropriate.

Arguments:

    DirectoryObject - The directory object being examined.

    CreateAccess - The access mask corresponding to create access for
        this directory type.

    AccessState - Checks for traverse access will typically be incidental
        to some other access attempt.  Information on the current state of
        that access attempt is required so that the constituent access
        attempts may be associated with each other in the audit log.

    ComponentName - Pointer to a Unicode string containing the name of
        the object being created.

    TypeMutexLocked - Indicates whether the type mutex for this object's
        type is locked.  The type mutex is used to protect the object's
        security descriptor from being modified while it is being accessed.

    PreviousMode - The previous processor mode.

    AccessStatus - Pointer to a variable to return the status code of the
        access attempt.  In the case of failure this status code must be
        propagated back to the user.

Return Value:

    BOOLEAN - TRUE if access is allowed and FALSE otherwise.  AccessStatus
    contains the status code to be passed back to the caller.  It is not
    correct to simply pass back STATUS_ACCESS_DENIED, since this will have
    to change with the advent of mandatory access control.

--*/

{
    BOOLEAN AccessAllowed;
    ACCESS_MASK GrantedAccess = 0;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    BOOLEAN MemoryAllocated;
    NTSTATUS Status;
    POBJECT_HEADER ObjectHeader;
    POBJECT_TYPE ObjectType;
    PPRIVILEGE_SET Privileges = NULL;

    PAGED_CODE();

    UNREFERENCED_PARAMETER (ComponentName);
    UNREFERENCED_PARAMETER (TypeMutexLocked);

    //
    //  Map the object body to its object header and corresponding
    //  object type
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( DirectoryObject );
    ObjectType = ObjectHeader->Type;

    //
    //  Obtain the object's security descriptor and make it was
    //  successful
    //

    Status = ObGetObjectSecurity( DirectoryObject,
                                  &SecurityDescriptor,
                                  &MemoryAllocated );

    if (!NT_SUCCESS( Status )) {

        *AccessStatus = Status;

        return( FALSE );
    }

    //
    //  lock the caller's tokens until after auditing has been
    //  performed.
    //

    SeLockSubjectContext( &AccessState->SubjectSecurityContext );

    //
    //  if we have a security descriptor then do an access
    //  check to see if access is allowed and set in the
    //  privileges if necessary
    //

    if (SecurityDescriptor != NULL) {

        AccessAllowed = SeAccessCheck( SecurityDescriptor,
                                       &AccessState->SubjectSecurityContext,
                                       TRUE,            // Tokens are locked
                                       CreateAccess,
                                       0,
                                       &Privileges,
                                       &ObjectType->TypeInfo.GenericMapping,
                                       PreviousMode,
                                       &GrantedAccess,
                                       AccessStatus );

        if (Privileges != NULL) {

            Status = SeAppendPrivileges( AccessState,
                                         Privileges );

            SeFreePrivileges( Privileges );
        }

        //
        //  This is wrong, but leave for reference.
        //
        //  if (AccessAllowed) {
        //
        //      AccessState->PreviouslyGrantedAccess |= GrantedAccess;
        //      AccessState->RemainingDesiredAccess &= ~GrantedAccess;
        //  }
        //

    } else {

        //
        //  At this point there is not a security descriptor
        //  so we'll assume access is allowed
        //

        AccessAllowed = TRUE;
    }

    //
    //  Free the caller's token and if the caller didn't have the
    //  object type locked we need to free it.
    //

    SeUnlockSubjectContext( &AccessState->SubjectSecurityContext );

    //
    //  Finally free the security descriptor
    //  and return to our caller
    //

    ObReleaseObjectSecurity( SecurityDescriptor,
                             MemoryAllocated );

    return( AccessAllowed );
}


NTSTATUS
ObAssignObjectSecurityDescriptor (
    IN PVOID Object,
    IN PSECURITY_DESCRIPTOR SecurityDescriptor OPTIONAL,
    IN POOL_TYPE PoolType // This field is currently ignored.
    )

/*++

Routine Description:

    Takes a pointer to an object and sets the SecurityDescriptor field
    in the object's header.

Arguments:

    Object - Supplies a pointer to the object

    SecurityDescriptor - Supplies a pointer to the security descriptor
        to be assigned to the object.  This pointer may be null if there
        is no security on the object.

    PoolType - Supplies the type of pool memory used to allocate the
        security descriptor.  This field is currently ignored.

Return Value:

    An appropriate NTSTATUS value.

--*/

{
    NTSTATUS Status;
    PSECURITY_DESCRIPTOR OutputSecurityDescriptor;
    POBJECT_HEADER ObjectHeader;

    PAGED_CODE();

    UNREFERENCED_PARAMETER (PoolType);

    //
    //  If the security descriptor isn't supplied then we set the
    //  object header's security descriptor to null and return
    //  to our caller
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );

    if (!ARGUMENT_PRESENT(SecurityDescriptor)) {

        ExFastRefInitialize ((PEX_FAST_REF) &ObjectHeader->SecurityDescriptor, NULL);

        return( STATUS_SUCCESS );
    }

    //
    //  Log the new security descriptor into our security database and
    //  get back the real security descriptor to use
    //

    Status = ObLogSecurityDescriptor( SecurityDescriptor,
                                      &OutputSecurityDescriptor,
                                      ExFastRefGetAdditionalReferenceCount () + 1 );

    //
    //  If we've been successful so far then set the object's
    //  security descriptor to the newly allocated one.
    //

    if (NT_SUCCESS(Status)) {

        ExFreePool (SecurityDescriptor);

        ASSERT (OutputSecurityDescriptor);
        __assume (OutputSecurityDescriptor);
        //
        // Initialize a fast reference structure with zero additional references
        //
        ExFastRefInitialize ((PEX_FAST_REF) &ObjectHeader->SecurityDescriptor, OutputSecurityDescriptor);
    }

    //
    //  And return to our caller
    //

    return( Status );
}



NTSTATUS
ObGetObjectSecurity (
    __in PVOID Object,
    __out PSECURITY_DESCRIPTOR *SecurityDescriptor,
    __out PBOOLEAN MemoryAllocated
    )

/*++

Routine Description:

    Given an object, this routine will find its security descriptor.
    It will do this by calling the object's security method.

    It is possible for an object not to have a security descriptor
    at all.  Unnamed objects such as events that can only be referenced
    by a handle are an example of an object that does not have a
    security descriptor.

Arguments:

    Object - Supplies the object body being queried.

    SecurityDescriptor - Returns a pointer to the object's security
        descriptor.

    MemoryAllocated - indicates whether we had to allocate pool
        memory to hold the security descriptor or not.  This should
        be passed back into ObReleaseObjectSecurity.

Return Value:

    STATUS_SUCCESS - The operation was successful.  Note that the
        operation may be successful and still return a NULL security
        descriptor.

    STATUS_INSUFFICIENT_RESOURCES - Insufficient memory was available
        to satisfy the request.

--*/

{
    SECURITY_INFORMATION SecurityInformation;
    ULONG Length = ObpDefaultSecurityDescriptorLength;
    NTSTATUS Status;
    POBJECT_TYPE ObjectType;
    POBJECT_HEADER ObjectHeader;
#if DBG
    KIRQL SaveIrql;
#endif

    PAGED_CODE();

    //
    //  Map the object body to its object header and corresponding
    //  object type
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );
    ObjectType = ObjectHeader->Type;

    //
    //  If the object is one that uses the default object method,
    //  its security descriptor is contained in ob's security
    //  descriptor cache.
    //
    //  Reference it so that it doesn't go away out from under us.
    //

    if (ObpCentralizedSecurity(ObjectType))  {

        *SecurityDescriptor = ObpReferenceSecurityDescriptor( ObjectHeader );

        *MemoryAllocated = FALSE;

        return( STATUS_SUCCESS );
    }

    //
    //  Request a complete security descriptor
    //

    SecurityInformation = OWNER_SECURITY_INFORMATION |
                          GROUP_SECURITY_INFORMATION |
                          DACL_SECURITY_INFORMATION  |
                          SACL_SECURITY_INFORMATION;

    //
    //  We don't know exactly how large is the SD, but we try with the largest
    //  size we get so far. In general the SD will be released after
    //  the access is checked. It shouldn't be then a problem of an extra pool usage
    //  because this oversizing
    //

    *SecurityDescriptor = ExAllocatePoolWithTag( PagedPool, Length, 'qSbO' );

    if (*SecurityDescriptor == NULL) {

        return( STATUS_INSUFFICIENT_RESOURCES );
    }

    *MemoryAllocated = TRUE;

    //
    //  The security method will return an absolute format
    //  security descriptor that just happens to be in a self
    //  contained buffer (not to be confused with a self-relative
    //  security descriptor).
    //

    ObpBeginTypeSpecificCallOut( SaveIrql );

    Status = (*ObjectType->TypeInfo.SecurityProcedure)( Object,
                                                        QuerySecurityDescriptor,
                                                        &SecurityInformation,
                                                        *SecurityDescriptor,
                                                        &Length,
                                                        &ObjectHeader->SecurityDescriptor,
                                                        ObjectType->TypeInfo.PoolType,
                                                        &ObjectType->TypeInfo.GenericMapping );

    ObpEndTypeSpecificCallOut( SaveIrql, "Security", ObjectType, Object );

    if (Status == STATUS_BUFFER_TOO_SMALL) {

        //
        //  The SD is larger than we tried first time. We need to allocate an other
        //  buffer and try again with this size
        //

        ExFreePool( *SecurityDescriptor );
        *MemoryAllocated = FALSE;

        //
        //  Save the new largest size
        //

        ObpDefaultSecurityDescriptorLength = Length;

//        DbgPrint( "ObpDefaultSecurityDescriptorLength increased to %ld\n",
//                  ObpDefaultSecurityDescriptorLength);

        //
        //  Now that we know how large the security descriptor is we
        //  can allocate space for it
        //

        *SecurityDescriptor = ExAllocatePoolWithTag( PagedPool, Length, 'qSbO' );

        if (*SecurityDescriptor == NULL) {

            return( STATUS_INSUFFICIENT_RESOURCES );
        }

        *MemoryAllocated = TRUE;

        //
        //  The security method will return an absolute format
        //  security descriptor that just happens to be in a self
        //  contained buffer (not to be confused with a self-relative
        //  security descriptor).
        //

        ObpBeginTypeSpecificCallOut( SaveIrql );

        Status = (*ObjectType->TypeInfo.SecurityProcedure)( Object,
                                                            QuerySecurityDescriptor,
                                                            &SecurityInformation,
                                                            *SecurityDescriptor,
                                                            &Length,
                                                            &ObjectHeader->SecurityDescriptor,
                                                            ObjectType->TypeInfo.PoolType,
                                                            &ObjectType->TypeInfo.GenericMapping );

        ObpEndTypeSpecificCallOut( SaveIrql, "Security", ObjectType, Object );
    }

    if (!NT_SUCCESS( Status )) {

        ExFreePool( *SecurityDescriptor );

        *MemoryAllocated = FALSE;
    }

    return( Status );
}


VOID
ObReleaseObjectSecurity (
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in BOOLEAN MemoryAllocated
    )

/*++

Routine Description:

    This function will free up any memory associated with a queried
    security descriptor.  This undoes the function ObGetObjectSecurity

Arguments:

    SecurityDescriptor - Supplies a pointer to the security descriptor
        to be freed.

    MemoryAllocated - Supplies whether or not we should free the
        memory pointed to by SecurityDescriptor.

Return Value:

    None.

--*/

{
    PAGED_CODE();

    //
    //  Check if there is a security descriptor to actually free
    //

    if ( SecurityDescriptor != NULL ) {

        //
        //  If ObGetObjectSecurity allocated memory then we
        //  need to free it. Otherwise what the earlier routine did
        //  was reference the object to keep the security descriptor
        //  to keep it from going away
        //

        if (MemoryAllocated) {

            ExFreePool( SecurityDescriptor );

        } else {

            ObDereferenceSecurityDescriptor( SecurityDescriptor, 1);
        }
    }
}


NTSTATUS
ObValidateSecurityQuota (
    IN PVOID Object,
    IN ULONG NewSize
    )

/*++

Routine Description:

    This routine will check to see if the new security information
    is larger than is allowed by the object's pre-allocated quota.

Arguments:

    Object - Supplies a pointer to the object whose information is to be
        modified.

    NewSize - Supplies the size of the proposed new security
        information.

Return Value:

    STATUS_SUCCESS - New size is within allotted quota.

    STATUS_QUOTA_EXCEEDED - The desired adjustment would have exceeded
        the permitted security quota for this object.

--*/

{
    POBJECT_HEADER ObjectHeader;
    POBJECT_HEADER_QUOTA_INFO QuotaInfo;

    PAGED_CODE();

    //
    //  Map the object body to its object header and corresponding
    //  quota information block
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );

    //
    // If we never charged quota originaly then don't worry about it now.
    //
    if (ObjectHeader->QuotaBlockCharged == (PEPROCESS_QUOTA_BLOCK) 1) {
        return( STATUS_SUCCESS );
    }

    QuotaInfo = OBJECT_HEADER_TO_QUOTA_INFO( ObjectHeader );

    //
    //  If there isn't any quota info and the new size is greater
    //  then the default security quota then if the object uses
    //  the default value then we've exceeded quota otherwise
    //  let the caller get the quota
    //

    if ((QuotaInfo == NULL) && (NewSize > SE_DEFAULT_SECURITY_QUOTA)) {


        if (!(ObjectHeader->Flags & OB_FLAG_DEFAULT_SECURITY_QUOTA)) {

            return( STATUS_SUCCESS );
        }

        return( STATUS_QUOTA_EXCEEDED );

    //
    //  If the quota is not null and the new size is greater than the
    //  allowed quota charge then if the charge is zero we grant the
    //  request otherwise we've exceeded quota.
    //

    } else if ((QuotaInfo != NULL) && (NewSize > QuotaInfo->SecurityDescriptorCharge)) {

        if (QuotaInfo->SecurityDescriptorCharge == 0) {

            //
            //  Should really charge quota here.
            //

            //  QuotaInfo->SecurityDescriptorCharge = SeComputeSecurityQuota( NewSize );

            return( STATUS_SUCCESS );
        }

        return( STATUS_QUOTA_EXCEEDED );

    //
    //  Otherwise we have two cases.  (1) there isn't any quota info but
    //  the size is within limits or (2) there is a quota info block and
    //  the size is within the specified security descriptor charge so
    //  return success to our caller
    //

    } else {

        return( STATUS_SUCCESS );
    }
}


NTSTATUS
ObAssignSecurity (
    __in PACCESS_STATE AccessState,
    __in_opt PSECURITY_DESCRIPTOR ParentDescriptor,
    __in PVOID Object,
    __in POBJECT_TYPE ObjectType
    )

/*++

Routine Description:

    This routine will assign a security descriptor to a newly created object.
    It assumes that the AccessState parameter contains a captured security
    descriptor.

Arguments:

     AccessState - The AccessState containing the security information
        for this object creation.

     ParentDescriptor - The security descriptor from the parent object, if
        available.

     Object - A pointer to the object being created.

     ObjectType - Supplies the type of object being created.

Return Value:

    STATUS_SUCCESS - indicates the operation was successful.

    STATUS_INVALID_OWNER - The owner SID provided as the owner of the
        target security descriptor is not one the caller is authorized
        to assign as the owner of an object.

    STATUS_PRIVILEGE_NOT_HELD - The caller does not have the privilege
        necessary to explicitly assign the specified system ACL.
        SeSecurityPrivilege privilege is needed to explicitly assign
        system ACLs to objects.

--*/

{
    PSECURITY_DESCRIPTOR NewDescriptor = NULL;
    NTSTATUS Status;
#if DBG
    KIRQL SaveIrql;
#endif

    PAGED_CODE();

    //
    //  SeAssignSecurity will construct the final version
    //  of the security  descriptor
    //

    Status = SeAssignSecurity( ParentDescriptor,
                               AccessState->SecurityDescriptor,
                               &NewDescriptor,
                               (BOOLEAN)(ObjectType == ObpDirectoryObjectType),
                               &AccessState->SubjectSecurityContext,
                               &ObjectType->TypeInfo.GenericMapping,
                               PagedPool );

    if (!NT_SUCCESS( Status )) {

        return( Status );
    }

    ObpBeginTypeSpecificCallOut( SaveIrql );

    //
    //  Now invoke the security method callback to finish
    //  the assignment.
    //

    Status = (*ObjectType->TypeInfo.SecurityProcedure)( Object,
                                                        AssignSecurityDescriptor,
                                                        NULL,
                                                        NewDescriptor,
                                                        NULL,
                                                        NULL,
                                                        PagedPool,
                                                        &ObjectType->TypeInfo.GenericMapping );

    ObpEndTypeSpecificCallOut( SaveIrql, "Security", ObjectType, Object );

    if (!NT_SUCCESS( Status )) {

        //
        // The attempt to assign the security descriptor to the object
        // failed.  Free the space used by the new security descriptor.
        //

        SeDeassignSecurity( &NewDescriptor );
    }

    //
    //  And return to our caller
    //

    return( Status );
}



NTSTATUS
ObQuerySecurityDescriptorInfo(
    IN PVOID Object,
    IN PSECURITY_INFORMATION SecurityInformation,
    OUT PSECURITY_DESCRIPTOR SecurityDescriptor,
    IN OUT PULONG Length,
    IN PSECURITY_DESCRIPTOR *ObjectsSecurityDescriptor
    )
/*++

Routine Description:

    This routine will extract the desired information from the
    passed security descriptor and return the information in
    the passed buffer as a security descriptor in self-relative
    format.

    This routine assumes that all parameters are captured and
    safe to reference.

Arguments:

    Object - Object that is having its security queried

    SecurityInformation - Specifies what information is being queried.

    SecurityDescriptor - Supplies the buffer to output the requested
        information into.

        This buffer has been probed only to the size indicated by
        the Length parameter.  Since it still points into user space,
        it must always be accessed in a try clause.

    Length - Supplies the address of a variable containing the length of
        the security descriptor buffer.  Upon return this variable will
        contain the length needed to store the requested information.

    ObjectsSecurityDescriptor - Supplies the address of a pointer to
        the objects security descriptor.  The passed security descriptor
        must be in self-relative format.

Return Value:

    NTSTATUS - STATUS_SUCCESS if successful and an appropriate error value
        otherwise

--*/
{
    NTSTATUS Status;
    POBJECT_HEADER ObjectHeader;
    PSECURITY_DESCRIPTOR ReferencedSecurityDescriptor;

    PAGED_CODE();

    UNREFERENCED_PARAMETER (ObjectsSecurityDescriptor);

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );

    //
    // Reference the security descriptor
    //
    ReferencedSecurityDescriptor = ObpReferenceSecurityDescriptor( ObjectHeader );

    Status = SeQuerySecurityDescriptorInfo( SecurityInformation,
                                            SecurityDescriptor,
                                            Length,
                                            &ReferencedSecurityDescriptor
                                            );

    if (ReferencedSecurityDescriptor != NULL) {
        ObDereferenceSecurityDescriptor ( ReferencedSecurityDescriptor, 1 );
    }

    return( Status );
}



NTSTATUS
ObSetSecurityDescriptorInfo (
    __in PVOID Object,
    __in PSECURITY_INFORMATION SecurityInformation,
    __inout PSECURITY_DESCRIPTOR SecurityDescriptor,
    __inout PSECURITY_DESCRIPTOR *ObjectsSecurityDescriptor,
    __in POOL_TYPE PoolType,
    __in PGENERIC_MAPPING GenericMapping
    )

/*++

Routine Description:

    Sets the security descriptor on an already secure object.

Arguments:

    Object - Pointer to the object being modified.

    SecurityInformation - Describes which information in the SecurityDescriptor parameter
        is relevant.

    SecurityDescriptor - Provides the new security information.

    ObjectsSecurityDescriptor - Provides/returns the object's security descriptor.

    PoolType - The pool the ObjectSecurityDescriptor is allocated from.

    GenericMapping - Supplies the generic mapping for the object.

Return Value:

    An appropriate status value

--*/

{
    PSECURITY_DESCRIPTOR OldDescriptor;
    PSECURITY_DESCRIPTOR NewDescriptor;
    PSECURITY_DESCRIPTOR CachedDescriptor;
    NTSTATUS Status;
    POBJECT_HEADER ObjectHeader;
    EX_FAST_REF OldRef;

    PAGED_CODE();

    //
    //  Check the rest of our input and call the default set security
    //  method.  Also make sure no one is modifying the security descriptor
    //  while we're looking at it.
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER( Object );

    //
    // In order to preserve some protected fields in the SD (like the SACL) we need to make sure that only one
    // thread updates it at any one time. If we didn't do this another modification could wipe out a SACL
    // an administrator was adding.
    //
    while (1) {

        //
        // Reference the security descriptor
        //

        OldDescriptor = ObpReferenceSecurityDescriptor( ObjectHeader );
        NewDescriptor = OldDescriptor;

        Status = SeSetSecurityDescriptorInfo( Object,
                                              SecurityInformation,
                                              SecurityDescriptor,
                                              &NewDescriptor,
                                              PoolType,
                                              GenericMapping );
        //
        //  If we successfully set the new security descriptor then we
        //  need to log it in our database and get yet another pointer
        //  to the finally security descriptor
        //
        if ( NT_SUCCESS( Status )) {
            Status = ObLogSecurityDescriptor( NewDescriptor,
                                              &CachedDescriptor,
                                              ExFastRefGetAdditionalReferenceCount () + 1 );
            ExFreePool( NewDescriptor );
            if ( NT_SUCCESS( Status )) {
                //
                // Now we need to see if anyone else update this security descriptor inside the
                // gap where we didn't hold the lock. If they did then we just try it all again.
                //
                OldRef = ExFastRefCompareSwapObject ((PEX_FAST_REF)ObjectsSecurityDescriptor,
                                                     CachedDescriptor,
                                                     OldDescriptor);
                if (ExFastRefEqualObjects (OldRef, OldDescriptor)) {
                    //
                    // The swap occured ok. We must now flush any slow refers out of the slow ref path before
                    // dereferencing the object. We do this by obtaining and dropping the object lock.
                    //
                    ObpLockObject( ObjectHeader );
                    ObpUnlockObject( ObjectHeader );
                    //
                    // If there was an original object then we need to work out how many
                    // cached references there were (if any) and return them.
                    //
                    ObDereferenceSecurityDescriptor( OldDescriptor, ExFastRefGetUnusedReferences (OldRef) + 2 );
                    break;
                } else {
                    ObDereferenceSecurityDescriptor( OldDescriptor, 1 );
                    ObDereferenceSecurityDescriptor( CachedDescriptor, ExFastRefGetAdditionalReferenceCount () + 1);
                }

            } else {

                //
                //  Dereference old SecurityDescriptor
                //

                ObDereferenceSecurityDescriptor( OldDescriptor, 1 );
                break;
            }
        } else {

            //
            //  Dereference old SecurityDescriptor
            //
            if (OldDescriptor != NULL) {
                ObDereferenceSecurityDescriptor( OldDescriptor, 1 );
            }
            break;
        }
    }

    //
    //  And return to our caller
    //

    return( Status );
}


NTSTATUS
ObpValidateAccessMask (
    PACCESS_STATE AccessState
    )

/*++

Routine Description:

    Checks the desired access mask of a passed object against the
    passed security descriptor.

Arguments:

    AccessState - A pointer to the AccessState for the pending operation.

Return Value:

    Only returns STATUS_SUCCESS

--*/

{
    SECURITY_DESCRIPTOR *SecurityDescriptor = AccessState->SecurityDescriptor;

    PAGED_CODE();

    //
    //  First make sure the access state has a security descriptor.  If there
    //  is one and it has a system acl and the previously granted access did
    //  not include system security then add the fact that we want system
    //  security to the remaining desired access state.
    //

    if (SecurityDescriptor != NULL) {

        if ( SecurityDescriptor->Control & SE_SACL_PRESENT ) {

            if ( !(AccessState->PreviouslyGrantedAccess & ACCESS_SYSTEM_SECURITY)) {

                AccessState->RemainingDesiredAccess |= ACCESS_SYSTEM_SECURITY;
            }
        }
    }

    return( STATUS_SUCCESS );
}

