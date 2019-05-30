/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    obinsert.c

Abstract:

    Object instantiation API

--*/

#include "obp.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,ObInsertObject)
#endif


NTSTATUS
ObInsertObject (
    __in PVOID Object,
    __in_opt PACCESS_STATE AccessState,
    __in_opt ACCESS_MASK DesiredAccess,
    __in ULONG ObjectPointerBias,
    __out_opt PVOID *NewObject,
    __out_opt PHANDLE Handle
    )

/*++

Routine Description:

    This routine inserts an object into the current processes handle table.

    The Object header includes a pointer to a SecurityDescriptor passed in
    an object creation call.  This SecurityDescriptor is not assumed to have
    been captured.  This routine is responsible for making an appropriate
    SecurityDescriptor and removing the reference in the object header.

Arguments:

    Object - Supplies a pointer to the new object body

    AccessState - Optionally supplies the access state for the new
        handle

    DesiredAccess - Optionally supplies the desired access we want for the
        new handle

    ObjectPointerBias - Supplies a bias to apply for the pointer count for the
        object

    NewObject - Optionally receives the pointer to the new object that we've
        created a handle for

    Handle - Receives the new handle, If NULL then no handle is created.
             Objects that don't have handles created must be unnamed and
             have an object bias of zero.

Return Value:

    An appropriate NTSTATUS value.

--*/

{
    POBJECT_CREATE_INFORMATION ObjectCreateInfo;
    POBJECT_HEADER ObjectHeader;
    PUNICODE_STRING ObjectName;
    POBJECT_TYPE ObjectType;
    POBJECT_HEADER_NAME_INFO NameInfo;
    PSECURITY_DESCRIPTOR ParentDescriptor = NULL;
    PVOID InsertObject;
    HANDLE NewHandle;
    OB_OPEN_REASON OpenReason;
    NTSTATUS Status = STATUS_SUCCESS;
    ACCESS_STATE LocalAccessState;
    AUX_ACCESS_DATA AuxData;
    BOOLEAN SecurityDescriptorAllocated;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS ReturnStatus;
    PVOID DirObject = NULL;
    OBP_LOOKUP_CONTEXT LookupContext;

    PAGED_CODE();

    ObpValidateIrql("ObInsertObject");

    //
    //  Get the address of the object header, the object create information,
    //  the object type, and the address of the object name descriptor, if
    //  specified.
    //

    ObjectHeader = OBJECT_TO_OBJECT_HEADER(Object);

#if DBG

    if ((ObjectHeader->Flags & OB_FLAG_NEW_OBJECT) == 0) {

        KdPrint(("OB: Attempting to insert existing object %08x\n", Object));
        KdBreakPoint();

        ObDereferenceObject(Object);

        return STATUS_INVALID_PARAMETER;
    }

#endif

    ObjectCreateInfo = ObjectHeader->ObjectCreateInfo;

    ObjectType = ObjectHeader->Type;

    NameInfo = ObpReferenceNameInfo( ObjectHeader );

    ObjectName = NULL;

    if ((NameInfo != NULL) && (NameInfo->Name.Buffer != NULL)) {

        ObjectName = &NameInfo->Name;
    }

    ASSERT (ARGUMENT_PRESENT (Handle) || (ObjectPointerBias == 0 && ObjectName == NULL &&
                                          ObjectType->TypeInfo.SecurityRequired && NewObject == NULL));

    //
    //  If security checks are not required and an object name is not
    //  specified, insert an unnamed object, biasing the count
    //  by one, dereference the bias, and return to our caller
    //

    PreviousMode = KeGetPreviousMode();

    if (!ObjectType->TypeInfo.SecurityRequired && (ObjectName == NULL)) {

        ObjectHeader->ObjectCreateInfo = NULL;

        *Handle = NULL;

        Status = ObpCreateUnnamedHandle( Object,
                                         DesiredAccess,
                                         1 + ObjectPointerBias,
                                         ObjectCreateInfo->Attributes,
                                         PreviousMode,
                                         NewObject,
                                         Handle );
        //
        //  Free the object creation information and dereference the object.
        //

        ObpFreeObjectCreateInformation(ObjectCreateInfo);

        ObpDereferenceNameInfo( NameInfo );
        ObDereferenceObject(Object);

        return Status;
    }

    //
    //  The object is either named or requires full security checks.  If the
    //  caller hasn't specified an access state then dummy up a local one
    //  using the requested desired access
    //

    if (!ARGUMENT_PRESENT(AccessState)) {

        AccessState = &LocalAccessState;

        Status = SeCreateAccessState( &LocalAccessState,
                                      &AuxData,
                                      DesiredAccess,
                                      &ObjectType->TypeInfo.GenericMapping );

        if (!NT_SUCCESS(Status)) {

            ObpDereferenceNameInfo( NameInfo );
            ObDereferenceObject(Object);

            return Status;
        }
    }

    AccessState->SecurityDescriptor = ObjectCreateInfo->SecurityDescriptor;

    //
    //  Check the desired access mask against the security descriptor
    //

    Status = ObpValidateAccessMask( AccessState );

    if (!NT_SUCCESS( Status )) {

        if (AccessState == &LocalAccessState) {

            SeDeleteAccessState( AccessState );
        }

        ObpDereferenceNameInfo( NameInfo );
        ObDereferenceObject(Object);

        if (AccessState == &LocalAccessState) {

            SeDeleteAccessState( AccessState );
        }

        return( Status );
    }

    //
    //  Set some local state variables
    //

    ObpInitializeLookupContext(&LookupContext);

    InsertObject = Object;
    OpenReason = ObCreateHandle;

    //
    //  Check if we have an object name.  If so then
    //  lookup the name
    //

    if (ObjectName != NULL) {

        Status = ObpLookupObjectName( ObjectCreateInfo->RootDirectory,
                                      ObjectName,
                                      ObjectCreateInfo->Attributes,
                                      ObjectType,
                                      (KPROCESSOR_MODE)(ObjectHeader->Flags & OB_FLAG_KERNEL_OBJECT
                                                            ? KernelMode : UserMode),
                                      ObjectCreateInfo->ParseContext,
                                      ObjectCreateInfo->SecurityQos,
                                      Object,
                                      AccessState,
                                      &LookupContext,
                                      &InsertObject );

        //
        //  We found the name and it is not the object we have as our input.
        //  So we cannot insert the object again so we'll return an
        //  appropriate status
        //

        if (NT_SUCCESS(Status) &&
            (InsertObject != NULL) &&
            (InsertObject != Object)) {

            OpenReason = ObOpenHandle;

            if (ObjectCreateInfo->Attributes & OBJ_OPENIF) {

                if (ObjectType != OBJECT_TO_OBJECT_HEADER(InsertObject)->Type) {

                    Status = STATUS_OBJECT_TYPE_MISMATCH;

                } else {

                    Status = STATUS_OBJECT_NAME_EXISTS;     // Warning only
                }

            } else {

                if (OBJECT_TO_OBJECT_HEADER(InsertObject)->Type == ObpSymbolicLinkObjectType) {

                     ObDereferenceObject(InsertObject);
                }
                
                Status = STATUS_OBJECT_NAME_COLLISION;
            }
        }

        //
        //  We did not find the name so we'll cleanup after ourselves
        //  and return to our caller
        //

        if (!NT_SUCCESS( Status )) {

            ObpReleaseLookupContext( &LookupContext );

            ObpDereferenceNameInfo( NameInfo );
            ObDereferenceObject( Object );

            //
            //  Free security information if we allocated it
            //

            if (AccessState == &LocalAccessState) {

                SeDeleteAccessState( AccessState );
            }

            return( Status );

        } else {

            //
            //  Otherwise we did locate the object name
            //
            //  If we just created a named symbolic link then call out to
            //  handle any Dos Device name semantics.
            //

            if (ObjectType == ObpSymbolicLinkObjectType) {

                ObpCreateSymbolicLinkName( (POBJECT_SYMBOLIC_LINK)InsertObject );
            }
        }
    }

    //
    //  If we are creating a new object, then we need assign security
    //  to it.  A pointer to the captured caller-proposed security
    //  descriptor is contained in the AccessState structure.  The
    //  SecurityDescriptor field in the object header must point to
    //  the final security descriptor, or to NULL if no security is
    //  to be assigned to the object.
    //

    if (InsertObject == Object) {

        //
        //  Only the following objects have security descriptors:
        //
        //       - Named Objects
        //       - Unnamed objects whose object-type information explicitly
        //         indicates a security descriptor is required.
        //

        if ((ObjectName != NULL) || ObjectType->TypeInfo.SecurityRequired) {

            //
            //  Get the parent's descriptor, if there is one...
            //

            if ((NameInfo != NULL) && (NameInfo->Directory != NULL)) {

                //
                //  This will allocate a block of memory and copy
                //  the parent's security descriptor into it, and
                //  return the pointer to the block.
                //
                //  Call ObReleaseObjectSecurity to free up this
                //  memory.
                //

                ObGetObjectSecurity( NameInfo->Directory,
                                     &ParentDescriptor,
                                     &SecurityDescriptorAllocated );
            }
            else {
                SecurityDescriptorAllocated = FALSE;
            }

            //
            //  Take the captured security descriptor in the AccessState,
            //  put it into the proper format, and call the object's
            //  security method to assign the new security descriptor to
            //  the new object.
            //

            Status = ObAssignSecurity( AccessState,
                                       ParentDescriptor,
                                       Object,
                                       ObjectType );

            if (ParentDescriptor != NULL) {

                ObReleaseObjectSecurity( ParentDescriptor,
                                         SecurityDescriptorAllocated );

            } else if (NT_SUCCESS( Status )) {

                SeReleaseSecurityDescriptor( ObjectCreateInfo->SecurityDescriptor,
                                             ObjectCreateInfo->ProbeMode,
                                             TRUE );

                ObjectCreateInfo->SecurityDescriptor = NULL;
                AccessState->SecurityDescriptor = NULL;
            }
        }

        if (!NT_SUCCESS( Status )) {

            //
            //  The attempt to assign the security descriptor to
            //  the object failed.
            //
            
            if (LookupContext.DirectoryLocked) {
                
                //
                //  If ObpLookupObjectName already inserted the 
                //  object into the directory we have to backup this
                //

                //
                //  Capture the object Directory 
                //

                DirObject = NameInfo->Directory;

                ObpDeleteDirectoryEntry( &LookupContext ); 
            }

            ObpReleaseLookupContext( &LookupContext );

            //
            //  If ObpLookupObjectName inserted the object into the directory
            //  it added a reference to the object and to its directory
            //  object. We should remove the extra-references
            //

            if (DirObject) {

                ObDereferenceObject( Object );
                ObDereferenceObject( DirObject );
            }

            //
            //  The first backout logic used ObpDeleteNameCheck
            //  which is wrong because the security descriptor for
            //  the object is not initialized. Actually  ObpDeleteNameCheck
            //  had no effect because the object was removed before from 
            //  the directory
            //

            ObpDereferenceNameInfo( NameInfo );
            ObDereferenceObject( Object );

            //
            //  Free security information if we allocated it
            //

            if (AccessState == &LocalAccessState) {

                SeDeleteAccessState( AccessState );
            }

            return( Status );
        }
    }

    ReturnStatus = Status;

    ObjectHeader->ObjectCreateInfo = NULL;

    //
    //  Create a named handle for the object with a pointer bias
    //  This call also will unlock the directory lock is necessary
    //  on return
    //

    if (ARGUMENT_PRESENT (Handle)) {

        Status = ObpCreateHandle( OpenReason,
                                  InsertObject,
                                  NULL,
                                  AccessState,
                                  1 + ObjectPointerBias,
                                  ObjectCreateInfo->Attributes,
                                  &LookupContext,
                                  PreviousMode,
                                  NewObject,
                                  &NewHandle );

        //
        //  If the insertion failed, the following dereference will cause
        //  the newly created object to be deallocated.
        //

        if (!NT_SUCCESS( Status )) {

            //
            //  Make the name reference go away if an error.
            //

            if (ObjectName != NULL) {

                ObpDeleteNameCheck( Object );
            }

            *Handle = NULL;

            ReturnStatus = Status;

        } else {
            *Handle = NewHandle;
        }

        ObpDereferenceNameInfo( NameInfo );

        ObDereferenceObject( Object );


    } else {

        BOOLEAN IsNewObject;

        //
        //  Charge the user quota for the object.
        //

        ObpLockObject( ObjectHeader );

        ReturnStatus = ObpChargeQuotaForObject( ObjectHeader, ObjectType, &IsNewObject );

        ObpUnlockObject( ObjectHeader );

        if (!NT_SUCCESS (ReturnStatus)) {
            ObDereferenceObject( Object );
        }

        //
        //  N.B. An object cannot be named if no Handle parameter is specified.
        //  The calls to ObpDereferenceNameInfo and ObpReleaseLookupContext are 
        //  not necessary in this path then.
        //
    }

    ObpFreeObjectCreateInformation( ObjectCreateInfo );

    //
    //  Free security information if we allocated it
    //

    if (AccessState == &LocalAccessState) {

        SeDeleteAccessState( AccessState );
    }

    return( ReturnStatus );
}

