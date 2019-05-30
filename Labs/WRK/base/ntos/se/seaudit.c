/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    seaudit.c

Abstract:

    This Module implements the audit and alarm procedures.

--*/

#include "pch.h"

#pragma hdrstop

VOID
SepProbeAndCaptureString_U (
    IN PUNICODE_STRING SourceString,
    OUT PUNICODE_STRING *DestString
    );

VOID
SepFreeCapturedString(
    IN PUNICODE_STRING CapturedString
    );

VOID
SepAuditTypeList (
    IN PIOBJECT_TYPE_LIST ObjectTypeList,
    IN ULONG ObjectTypeListLength,
    IN PNTSTATUS AccessStatus,
    IN ULONG StartIndex,
    OUT PBOOLEAN GenerateSuccessAudit,
    OUT PBOOLEAN GenerateFailureAudit
    );

VOID
SepExamineSaclEx(
    IN PACL Sacl,
    IN PACCESS_TOKEN Token,
    IN ACCESS_MASK DesiredAccess,
    IN PIOBJECT_TYPE_LIST ObjectTypeList OPTIONAL,
    IN ULONG ObjectTypeListLength,
    IN BOOLEAN ReturnResultList,
    IN PNTSTATUS AccessStatus,
    IN PACCESS_MASK GrantedAccess,
    IN PSID PrincipalSelfSid,
    OUT PBOOLEAN GenerateSuccessAudit,
    OUT PBOOLEAN GenerateFailureAudit
    );

NTSTATUS
SepAccessCheckAndAuditAlarm (
    IN PUNICODE_STRING SubsystemName,
    IN PVOID HandleId,
    IN PHANDLE ClientToken OPTIONAL,
    IN PUNICODE_STRING ObjectTypeName,
    IN PUNICODE_STRING ObjectName,
    IN PSECURITY_DESCRIPTOR SecurityDescriptor,
    IN PSID PrincipalSelfSid,
    IN ACCESS_MASK DesiredAccess,
    IN AUDIT_EVENT_TYPE AuditType,
    IN ULONG Flags,
    IN POBJECT_TYPE_LIST ObjectTypeList OPTIONAL,
    IN ULONG ObjectTypeListLength,
    IN PGENERIC_MAPPING GenericMapping,
    OUT PACCESS_MASK GrantedAccess,
    OUT PNTSTATUS AccessStatus,
    OUT PBOOLEAN GenerateOnClose,
    IN BOOLEAN ReturnResultList
    );

#ifdef ALLOC_PRAGMA
VOID
SepSetAuditInfoForObjectType(
    IN  UCHAR AceFlags,
    IN  ACCESS_MASK AccessMask,
    IN  ACCESS_MASK DesiredAccess,
    IN  PIOBJECT_TYPE_LIST ObjectTypeList,
    IN  ULONG ObjectTypeListLength,
    IN  BOOLEAN ReturnResultList,
    IN  ULONG ObjectTypeIndex,
    IN  PNTSTATUS AccessStatus,
    IN  PACCESS_MASK GrantedAccess,
    OUT PBOOLEAN GenerateSuccessAudit,
    OUT PBOOLEAN GenerateFailureAudit
    );
#pragma alloc_text(PAGE,SepSinglePrivilegeCheck)
#pragma alloc_text(PAGE,SeCheckAuditPrivilege)
#pragma alloc_text(PAGE,SepProbeAndCaptureString_U)
#pragma alloc_text(PAGE,SepFreeCapturedString)
#pragma alloc_text(PAGE,NtPrivilegeObjectAuditAlarm)
#pragma alloc_text(PAGE,SePrivilegeObjectAuditAlarm)
#pragma alloc_text(PAGE,NtPrivilegedServiceAuditAlarm)
#pragma alloc_text(PAGE,SePrivilegedServiceAuditAlarm)
#pragma alloc_text(PAGE,SepAccessCheckAndAuditAlarm)
#pragma alloc_text(PAGE,NtAccessCheckAndAuditAlarm)
#pragma alloc_text(PAGE,NtAccessCheckByTypeAndAuditAlarm)
#pragma alloc_text(PAGE,NtAccessCheckByTypeResultListAndAuditAlarm)
#pragma alloc_text(PAGE,NtAccessCheckByTypeResultListAndAuditAlarmByHandle)
#pragma alloc_text(PAGE,NtOpenObjectAuditAlarm)
#pragma alloc_text(PAGE,NtCloseObjectAuditAlarm)
#pragma alloc_text(PAGE,NtDeleteObjectAuditAlarm)
#pragma alloc_text(PAGE,SeOpenObjectAuditAlarm)
#pragma alloc_text(PAGE,SeOpenObjectForDeleteAuditAlarm)
#pragma alloc_text(PAGE,SeObjectReferenceAuditAlarm)
#pragma alloc_text(PAGE,SeAuditHandleCreation)
#pragma alloc_text(PAGE,SeCloseObjectAuditAlarm)
#pragma alloc_text(PAGE,SeDeleteObjectAuditAlarm)
#pragma alloc_text(PAGE,SepExamineSacl)
#pragma alloc_text(PAGE,SepAuditTypeList)
#pragma alloc_text(PAGE,SepSetAuditInfoForObjectType)
#pragma alloc_text(PAGE,SepExamineSaclEx)
#pragma alloc_text(INIT,SepInitializePrivilegeFilter)
#pragma alloc_text(PAGE,SepFilterPrivilegeAudits)
#pragma alloc_text(PAGE,SeAuditingFileOrGlobalEvents)
#pragma alloc_text(PAGE,SeAuditingFileEvents)
#pragma alloc_text(PAGE,SeAuditingFileEventsWithContext)
#pragma alloc_text(PAGE,SeAuditingHardLinkEvents)
#pragma alloc_text(PAGE,SeAuditingHardLinkEventsWithContext)
#endif


//
//  Private useful routines
//

//
// This routine is to be called to do simple checks of single privileges
// against the passed token.
//
// DO NOT CALL THIS TO CHECK FOR SeTcbPrivilege SINCE THAT MUST
// BE CHECKED AGAINST THE PRIMARY TOKEN ONLY!
//

BOOLEAN
SepSinglePrivilegeCheck (
   LUID DesiredPrivilege,
   IN PACCESS_TOKEN Token,
   IN KPROCESSOR_MODE PreviousMode
   )

/*++

Routine Description:

    Determines if the passed token has the passed privilege.

Arguments:

    DesiredPrivilege - The privilege to be tested for.

    Token - The token being examined.

    PreviousMode - The previous processor mode.

Return Value:

    Returns TRUE of the subject has the passed privilege, FALSE otherwise.

--*/

{

   LUID_AND_ATTRIBUTES Privilege;
   BOOLEAN Result;

   PAGED_CODE();
   
   //
   // Don't let anyone call this to test for SeTcbPrivilege
   //

   ASSERT(!((DesiredPrivilege.LowPart == SeTcbPrivilege.LowPart) &&
            (DesiredPrivilege.HighPart == SeTcbPrivilege.HighPart)));

   Privilege.Luid = DesiredPrivilege;
   Privilege.Attributes = 0;

   Result = SepPrivilegeCheck(
               Token,
               &Privilege,
               1,
               PRIVILEGE_SET_ALL_NECESSARY,
               PreviousMode
               );

   return(Result);
}


BOOLEAN
SeCheckAuditPrivilege (
   __in PSECURITY_SUBJECT_CONTEXT SubjectSecurityContext,
   __in KPROCESSOR_MODE PreviousMode
   )
/*++

Routine Description:

    This routine specifically searches the primary token (rather than
    the effective token) of the calling process for SeAuditPrivilege.
    In order to do this it must call the underlying worker
    SepPrivilegeCheck directly, to ensure that the correct token is
    searched

Arguments:

    SubjectSecurityContext - The subject being examined.

    PreviousMode - The previous processor mode.

Return Value:

    Returns TRUE if the subject has SeAuditPrivilege, FALSE otherwise.

--*/
{

    PRIVILEGE_SET RequiredPrivileges;
    BOOLEAN AccessGranted;

    PAGED_CODE();

    RequiredPrivileges.PrivilegeCount = 1;
    RequiredPrivileges.Control = PRIVILEGE_SET_ALL_NECESSARY;
    RequiredPrivileges.Privilege[0].Luid = SeAuditPrivilege;
    RequiredPrivileges.Privilege[0].Attributes = 0;

    AccessGranted = SepPrivilegeCheck(
                        SubjectSecurityContext->PrimaryToken,     // token
                        RequiredPrivileges.Privilege,             // privilege set
                        RequiredPrivileges.PrivilegeCount,        // privilege count
                        PRIVILEGE_SET_ALL_NECESSARY,              // privilege control
                        PreviousMode                              // previous mode
                        );

    if ( PreviousMode != KernelMode ) {

        SePrivilegedServiceAuditAlarm (
            NULL,
            SubjectSecurityContext,
            &RequiredPrivileges,
            AccessGranted
            );
    }

    return( AccessGranted );
}


VOID
SepProbeAndCaptureString_U (
    IN PUNICODE_STRING SourceString,
    OUT PUNICODE_STRING *DestString
    )
/*++

Routine Description:

    Helper routine to probe and capture a Unicode string argument.

    This routine may fail due to lack of memory, in which case,
    it will return a NULL pointer in the output parameter.

Arguments:

    SourceString - Pointer to a Unicode string to be captured.

    DestString - Returns a pointer to a captured Unicode string.  This
        will be one contiguous structure, and thus may be freed by
        a single call to ExFreePool().

Return Value:

    None.

--*/
{

    UNICODE_STRING InputString;
    ULONG Length;
    NTSTATUS Status;

    PAGED_CODE();

    //
    // Initialize the object name descriptor and capture the specified name
    // string.
    //

    *DestString = NULL;

    Status = STATUS_SUCCESS;
    try {

        //
        // Probe and capture the name string descriptor and probe the
        // name string, if necessary.
        //

        ProbeAndReadUnicodeStringEx(&InputString, SourceString);
        ProbeForRead(InputString.Buffer,
                     InputString.Length,
                     sizeof(WCHAR));



        //
        // If the length of the string is not an even multiple of the
        // size of a UNICODE character or cannot be zero terminated,
        // then return an error.
        //

        Length = InputString.Length;
        if (((Length & (sizeof(WCHAR) - 1)) != 0) ||
            (Length == (MAXUSHORT - sizeof(WCHAR) + 1))) {
            Status = STATUS_INVALID_PARAMETER;

        } else {

            //
            // Allocate a buffer for the specified name string.
            //

            *DestString = ExAllocatePoolWithTag(
                            PagedPool,
                            InputString.Length + sizeof(UNICODE_STRING),
                            'sUeS');

            if (*DestString == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;

            } else {
                (*DestString)->Length = InputString.Length;
                (*DestString)->MaximumLength = InputString.Length;
                (*DestString)->Buffer = (PWSTR) ((*DestString) + 1);

                if (InputString.Length != 0) {

                    RtlCopyMemory(
                        (*DestString)->Buffer,
                        InputString.Buffer,
                        InputString.Length);
                }

            }
        }

    } except(ExSystemExceptionFilter()) {
        Status = GetExceptionCode();
        if (*DestString != NULL) {
            ExFreePool(*DestString);
            *DestString = NULL;
        }
    }

    return;

}


VOID
SepFreeCapturedString(
    IN PUNICODE_STRING CapturedString
    )

/*++

Routine Description:

    Frees a string captured by SepProbeAndCaptureString.

Arguments:

    CapturedString - Supplies a pointer to a string previously captured
        by SepProbeAndCaptureString.

Return Value:

    None.

--*/

{
    PAGED_CODE();

    ExFreePool( CapturedString );
    return;
}

////////////////////////////////////////////////////////////////////////
//                                                                    //
//                  Privileged Object Audit Alarms                    //
//                                                                    //
////////////////////////////////////////////////////////////////////////


NTSTATUS
NtPrivilegeObjectAuditAlarm (
    __in PUNICODE_STRING SubsystemName,
    __in_opt PVOID HandleId,
    __in HANDLE ClientToken,
    __in ACCESS_MASK DesiredAccess,
    __in PPRIVILEGE_SET Privileges,
    __in BOOLEAN AccessGranted
    )
/*++

Routine Description:

    This routine is used to generate audit and alarm messages when an
    attempt is made to perform privileged operations on a protected
    subsystem object after the object is already opened.  This routine may
    result in several messages being generated and sent to Port objects.
    This may result in a significant latency before returning.  Design of
    routines that must call this routine must take this potential latency
    into account.  This may have an impact on the approach taken for data
    structure mutex locking, for example.

    This API requires the caller have SeAuditPrivilege privilege.  The test
    for this privilege is always against the primary token of the calling
    process, allowing the caller to be impersonating a client during the
    call with no ill effects.

Arguments:

    SubsystemName - Supplies a name string identifying the subsystem
        calling the routine.

    HandleId - A unique value representing the client's handle to the
        object.

    ClientToken - A handle to a token object representing the client that
        requested the operation.  This handle must be obtained from a
        communication session layer, such as from an LPC Port or Local
        Named Pipe, to prevent possible security policy violations.

    DesiredAccess - The desired access mask.  This mask must have been
        previously mapped to contain no generic accesses.

    Privileges - The set of privileges required for the requested
        operation.  Those privileges that were held by the subject are
        marked using the UsedForAccess flag of the attributes
        associated with each privilege.

    AccessGranted - Indicates whether the requested access was granted or
        not.  A value of TRUE indicates the access was granted.  A value of
        FALSE indicates the access was not granted.

Return value:

--*/
{

    KPROCESSOR_MODE PreviousMode;
    PUNICODE_STRING CapturedSubsystemName = NULL;
    PPRIVILEGE_SET CapturedPrivileges = NULL;
    ULONG PrivilegeParameterLength;
    ULONG PrivilegeCount;
    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;
    BOOLEAN Result;
    PTOKEN Token;
    NTSTATUS Status;
    BOOLEAN AuditPerformed;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    ASSERT(PreviousMode != KernelMode);

    Status = ObReferenceObjectByHandle(
         ClientToken,             // Handle
         TOKEN_QUERY,             // DesiredAccess
         SeTokenObjectType,      // ObjectType
         PreviousMode,            // AccessMode
         (PVOID *)&Token,         // Object
         NULL                     // GrantedAccess
         );

    if (!NT_SUCCESS( Status )) {
        return( Status );
    }

    //
    // If the passed token is an impersonation token, make sure
    // it is at SecurityIdentification or above.
    //

    if (Token->TokenType == TokenImpersonation) {

        if (Token->ImpersonationLevel < SecurityIdentification) {

            ObDereferenceObject( (PVOID)Token );

            return( STATUS_BAD_IMPERSONATION_LEVEL );

        }
    }

    //
    // Check for SeAuditPrivilege
    //

    SeCaptureSubjectContext ( &SubjectSecurityContext );

    Result = SeCheckAuditPrivilege (
                 &SubjectSecurityContext,
                 PreviousMode
                 );

    if (!Result) {

        ObDereferenceObject( (PVOID)Token );
        SeReleaseSubjectContext ( &SubjectSecurityContext );
        return(STATUS_PRIVILEGE_NOT_HELD);

    }

    try {

        SepProbeAndCaptureString_U ( SubsystemName,
                                     &CapturedSubsystemName );

        ProbeForReadSmallStructure(
            Privileges,
            sizeof(PRIVILEGE_SET),
            sizeof(ULONG)
            );
        PrivilegeCount = Privileges->PrivilegeCount;

        if (!IsValidPrivilegeCount(PrivilegeCount)) {
            Status= STATUS_INVALID_PARAMETER;
            leave ;
        }
        PrivilegeParameterLength = (ULONG)sizeof(PRIVILEGE_SET) +
                          ((PrivilegeCount - ANYSIZE_ARRAY) *
                            (ULONG)sizeof(LUID_AND_ATTRIBUTES)  );

        ProbeForRead(
            Privileges,
            PrivilegeParameterLength,
            sizeof(ULONG)
            );

        CapturedPrivileges = ExAllocatePoolWithTag( PagedPool,
                                                    PrivilegeParameterLength,
                                                    'rPeS'
                                                  );

        if (CapturedPrivileges != NULL) {

            RtlCopyMemory ( CapturedPrivileges,
                            Privileges,
                            PrivilegeParameterLength );
            CapturedPrivileges->PrivilegeCount = PrivilegeCount;
        }

    } except (EXCEPTION_EXECUTE_HANDLER) {

        Status = GetExceptionCode();
    }
    if (!NT_SUCCESS(Status)) {

        if (CapturedPrivileges != NULL) {
            ExFreePool( CapturedPrivileges );
        }

        if (CapturedSubsystemName != NULL) {
            SepFreeCapturedString ( CapturedSubsystemName );
        }

        SeReleaseSubjectContext ( &SubjectSecurityContext );

        ObDereferenceObject( (PVOID)Token );

        return Status;

    }

    //
    // No need to lock the token, because the only thing we're going
    // to reference in it is the User's Sid, which cannot be changed.
    //

    //
    // SepPrivilegeObjectAuditAlarm will check the global flags
    // to determine if we're supposed to be auditing here.
    //

    AuditPerformed = SepAdtPrivilegeObjectAuditAlarm (
                         CapturedSubsystemName,
                         HandleId,
                         Token,                                // ClientToken
                         SubjectSecurityContext.PrimaryToken,  // PrimaryToken
                         SubjectSecurityContext.ProcessAuditId,
                         DesiredAccess,
                         CapturedPrivileges,
                         AccessGranted
                         );

    if (CapturedPrivileges != NULL) {
        ExFreePool( CapturedPrivileges );
    }

    if (CapturedSubsystemName != NULL) {
        SepFreeCapturedString ( CapturedSubsystemName );
    }

    SeReleaseSubjectContext ( &SubjectSecurityContext );

    ObDereferenceObject( (PVOID)Token );

    return(STATUS_SUCCESS);
}



VOID
SePrivilegeObjectAuditAlarm(
    __in HANDLE Handle,
    __in PSECURITY_SUBJECT_CONTEXT SubjectSecurityContext,
    __in ACCESS_MASK DesiredAccess,
    __in PPRIVILEGE_SET Privileges,
    __in BOOLEAN AccessGranted,
    __in KPROCESSOR_MODE AccessMode
    )

/*++

Routine Description:

    This routine is used by object methods that perform privileged
    operations to generate audit and alarm messages related to the use
    of privileges, or attempts to use privileges.

Arguments:

    Object - Address of the object accessed.  This value will not be
    used as a pointer (referenced).  It is necessary only to enter
    into log messages.

    Handle - Provides the handle value assigned for the open.

    SecurityDescriptor - A pointer to the security descriptor of the
    object being accessed.

    SubjectSecurityContext - A pointer to the captured security
    context of the subject attempting to open the object.

    DesiredAccess - The desired access mask.  This mask must have been
    previously mapped to contain no generic accesses.

    Privileges - Points to a set of privileges required for the access
    attempt.  Those privileges that were held by the subject are
    marked using the UsedForAccess flag of the PRIVILEGE_ATTRIBUTES
    associated with each privilege.

    AccessGranted - Indicates whether the access was granted or
    denied.  A value of TRUE indicates the access was allowed.  A
    value of FALSE indicates the access was denied.

    AccessMode - Indicates the access mode used for the access check.
    Messages will not be generated by kernel mode accesses.

Return Value:

    None.

--*/

{
    BOOLEAN AuditPerformed;

    PAGED_CODE();

    if (AccessMode != KernelMode) {

        AuditPerformed = SepAdtPrivilegeObjectAuditAlarm (
                             (PUNICODE_STRING)&SeSubsystemName,
                             Handle,
                             SubjectSecurityContext->ClientToken,
                             SubjectSecurityContext->PrimaryToken,
                             SubjectSecurityContext->ProcessAuditId,
                             DesiredAccess,
                             Privileges,
                             AccessGranted
                             );
    }
}


////////////////////////////////////////////////////////////////////////
//                                                                    //
//                  Privileged Service Audit Alarms                   //
//                                                                    //
////////////////////////////////////////////////////////////////////////


NTSTATUS
NtPrivilegedServiceAuditAlarm (
    __in PUNICODE_STRING SubsystemName,
    __in PUNICODE_STRING ServiceName,
    __in HANDLE ClientToken,
    __in PPRIVILEGE_SET Privileges,
    __in BOOLEAN AccessGranted
    )

/*++

Routine Description:

    This routine is used to generate audit and alarm messages when an
    attempt is made to perform privileged system service operations.  This
    routine may result in several messages being generated and sent to Port
    objects.  This may result in a significant latency before returning.
    Design of routines that must call this routine must take this potential
    latency into account.  This may have an impact on the approach taken
    for data structure mutex locking, for example.

    This API requires the caller have SeAuditPrivilege privilege.  The test
    for this privilege is always against the primary token of the calling
    process, allowing the caller to be impersonating a client during the
    call with no ill effects

Arguments:

    SubsystemName - Supplies a name string identifying the subsystem
        calling the routine.

    ServiceName - Supplies a name of the privileged subsystem service.  For
        example, "RESET RUNTIME LOCAL SECURITY POLICY" might be specified
        by a Local Security Authority service used to update the local
        security policy database.

    ClientToken - A handle to a token object representing the client that
        requested the operation.  This handle must be obtained from a
        communication session layer, such as from an LPC Port or Local
        Named Pipe, to prevent possible security policy violations.

    Privileges - Points to a set of privileges required to perform the
        privileged operation.  Those privileges that were held by the
        subject are marked using the UsedForAccess flag of the
        attributes associated with each privilege.

    AccessGranted - Indicates whether the requested access was granted or
        not.  A value of TRUE indicates the access was granted.  A value of
        FALSE indicates the access was not granted.

Return value:

--*/

{

    PPRIVILEGE_SET CapturedPrivileges = NULL;
    ULONG PrivilegeParameterLength = 0;
    BOOLEAN Result;
    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;
    KPROCESSOR_MODE PreviousMode;
    PUNICODE_STRING CapturedSubsystemName = NULL;
    PUNICODE_STRING CapturedServiceName = NULL;
    NTSTATUS Status;
    PTOKEN Token;
    ULONG PrivilegeCount;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    ASSERT(PreviousMode != KernelMode);

    Status = ObReferenceObjectByHandle(
                 ClientToken,             // Handle
                 TOKEN_QUERY,             // DesiredAccess
                 SeTokenObjectType,      // ObjectType
                 PreviousMode,            // AccessMode
                 (PVOID *)&Token,         // Object
                 NULL                     // GrantedAccess
                 );

    if ( !NT_SUCCESS( Status )) {
        return( Status );
    }

    //
    // If the passed token is an impersonation token, make sure
    // it is at SecurityIdentification or above.
    //

    if (Token->TokenType == TokenImpersonation) {

        if (Token->ImpersonationLevel < SecurityIdentification) {

            ObDereferenceObject( (PVOID)Token );

            return( STATUS_BAD_IMPERSONATION_LEVEL );

        }
    }

    //
    // Check for SeAuditPrivilege
    //

    SeCaptureSubjectContext ( &SubjectSecurityContext );

    Result = SeCheckAuditPrivilege (
                 &SubjectSecurityContext,
                 PreviousMode
                 );

    if (!Result) {

        ObDereferenceObject( (PVOID)Token );

        SeReleaseSubjectContext ( &SubjectSecurityContext );

        return(STATUS_PRIVILEGE_NOT_HELD);
    }

    try {

        if ( ARGUMENT_PRESENT( SubsystemName )) {
            SepProbeAndCaptureString_U ( SubsystemName,
                                         &CapturedSubsystemName );
        }

        if ( ARGUMENT_PRESENT( ServiceName )) {
            SepProbeAndCaptureString_U ( ServiceName,
                                         &CapturedServiceName );

        }

        ProbeForReadSmallStructure(
            Privileges,
            sizeof(PRIVILEGE_SET),
            sizeof(ULONG)
            );

        PrivilegeCount = Privileges->PrivilegeCount;

        if (!IsValidPrivilegeCount( PrivilegeCount ) ) {
            Status = STATUS_INVALID_PARAMETER;
            leave ;
        }
        PrivilegeParameterLength = (ULONG)sizeof(PRIVILEGE_SET) +
                          ((PrivilegeCount - ANYSIZE_ARRAY) *
                            (ULONG)sizeof(LUID_AND_ATTRIBUTES)  );

        ProbeForRead(
            Privileges,
            PrivilegeParameterLength,
            sizeof(ULONG)
            );

        CapturedPrivileges = ExAllocatePoolWithTag( PagedPool,
                                                    PrivilegeParameterLength,
                                                    'rPeS'
                                                  );

        //
        // If ExAllocatePool has failed, too bad.  Carry on and do as much of the
        // audit as we can.
        //

        if (CapturedPrivileges != NULL) {

            RtlCopyMemory ( CapturedPrivileges,
                            Privileges,
                            PrivilegeParameterLength );
            CapturedPrivileges->PrivilegeCount = PrivilegeCount;

        }

    } except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
    }

    if (!NT_SUCCESS(Status)) {

        if (CapturedSubsystemName != NULL) {
            SepFreeCapturedString ( CapturedSubsystemName );
        }

        if (CapturedServiceName != NULL) {
            SepFreeCapturedString ( CapturedServiceName );
        }

        if (CapturedPrivileges != NULL) {
            ExFreePool ( CapturedPrivileges );
        }

        SeReleaseSubjectContext ( &SubjectSecurityContext );

        ObDereferenceObject( (PVOID)Token );

        return Status;

    }

    //
    // The AuthenticationId is in the read-only part of the token,
    // so we may reference it without having the token read-locked.
    //

    SepAdtPrivilegedServiceAuditAlarm ( &SubjectSecurityContext,
                                        CapturedSubsystemName,
                                        CapturedServiceName,
                                        Token,
                                        SubjectSecurityContext.PrimaryToken,
                                        CapturedPrivileges,
                                        AccessGranted );

    if (CapturedSubsystemName != NULL) {
        SepFreeCapturedString ( CapturedSubsystemName );
    }

    if (CapturedServiceName != NULL) {
        SepFreeCapturedString ( CapturedServiceName );
    }

    if (CapturedPrivileges != NULL) {
        ExFreePool ( CapturedPrivileges );
    }

    ObDereferenceObject( (PVOID)Token );

    SeReleaseSubjectContext ( &SubjectSecurityContext );

    return(STATUS_SUCCESS);
}


VOID
SePrivilegedServiceAuditAlarm (
    __in_opt PUNICODE_STRING ServiceName,
    __in PSECURITY_SUBJECT_CONTEXT SubjectSecurityContext,
    __in PPRIVILEGE_SET Privileges,
    __in BOOLEAN AccessGranted
    )
/*++

Routine Description:

    This routine is to be called whenever a privileged system service
    is attempted.  It should be called immediately after the privilege
    check regardless of whether or not the test succeeds.

Arguments:

    ServiceName - Supplies the name of the privileged system service.

    SubjectSecurityContext - The subject security context representing
        the caller of the system service.

    Privileges - Supplies a privilege set containing the privilege(s)
        required for the access.

    AccessGranted - Supplies the results of the privilege test.

Return Value:

    None.

--*/

{
    PTOKEN Token;

    PAGED_CODE();

#if DBG
    if ( Privileges )
    {
        ASSERT( IsValidPrivilegeCount(Privileges->PrivilegeCount) );
    }
#endif
    
    Token = (PTOKEN)EffectiveToken( SubjectSecurityContext );

    if ( RtlEqualSid( SeLocalSystemSid, SepTokenUserSid( Token ))) {
        return;
    }
                    
    if (RtlEqualSid(SeExports->SeNetworkServiceSid, SepTokenUserSid(Token)) ||
        RtlEqualSid(SeExports->SeLocalServiceSid, SepTokenUserSid(Token))) {
        
        if (!SepFilterPrivilegeAudits(SEP_SERVICES_FILTER, Privileges))
        {
            return;
        }
    }

    SepAdtPrivilegedServiceAuditAlarm (
        SubjectSecurityContext,
        (PUNICODE_STRING)&SeSubsystemName,
        ServiceName,
        SubjectSecurityContext->ClientToken,
        SubjectSecurityContext->PrimaryToken,
        Privileges,
        AccessGranted
        );

    return;
}


NTSTATUS
SepAccessCheckAndAuditAlarm (
    IN PUNICODE_STRING SubsystemName,
    IN PVOID HandleId,
    IN PHANDLE ClientToken OPTIONAL,
    IN PUNICODE_STRING ObjectTypeName,
    IN PUNICODE_STRING ObjectName,
    IN PSECURITY_DESCRIPTOR SecurityDescriptor,
    IN PSID PrincipalSelfSid,
    IN ACCESS_MASK DesiredAccess,
    IN AUDIT_EVENT_TYPE AuditType,
    IN ULONG Flags,
    IN POBJECT_TYPE_LIST ObjectTypeList OPTIONAL,
    IN ULONG ObjectTypeListLength,
    IN PGENERIC_MAPPING GenericMapping,
    OUT PACCESS_MASK GrantedAccess,
    OUT PNTSTATUS AccessStatus,
    OUT PBOOLEAN GenerateOnClose,
    IN BOOLEAN ReturnResultList
    )
/*++

Routine Description:

    This system service is used to perform both an access validation and
    generate the corresponding audit and alarm messages.  This service may
    only be used by a protected server that chooses to impersonate its
    client and thereby specifies the client security context implicitly.

Arguments:

    SubsystemName - Supplies a name string identifying the subsystem
        calling the routine.

    HandleId - A unique value that will be used to represent the client's
        handle to the object.  This value is ignored (and may be re-used)
        if the access is denied.

    ClientToken - Supplies the client token so that the caller does not have
        to impersonate before making the kernel call.

    ObjectTypeName - Supplies the name of the type of the object being
        created or accessed.

    ObjectName - Supplies the name of the object being created or accessed.

    SecurityDescriptor - A pointer to the Security Descriptor against which
        access is to be checked.

    DesiredAccess - The desired access mask.  This mask must have been
        previously mapped to contain no generic accesses.

    AuditType - Specifies the type of audit to be generated.  Valid value
        is: AuditEventObjectAccess

    Flags - Flags modifying the execution of the API:

        AUDIT_ALLOW_NO_PRIVILEGE - If the called does not have AuditPrivilege,
            the call will silently continue to check access and will
            generate no audit.

    ObjectTypeList - Supplies a list of GUIDs representing the object (and
        sub-objects) being accessed.  If no list is present, AccessCheckByType
        behaves identically to AccessCheck.

    ObjectTypeListLength - Specifies the number of elements in the ObjectTypeList.

    GenericMapping - Supplies a pointer to the generic mapping associated
        with this object type.

    ObjectCreation - A boolean flag indicated whether the access will
        result in a new object being created if granted.  A value of TRUE
        indicates an object will be created, FALSE indicates an existing
        object will be opened.

    GrantedAccess - Receives a masking indicating which accesses have been
        granted.

    AccessStatus - Receives an indication of the success or failure of the
        access check.  If access is granted, STATUS_SUCCESS is returned.
        If access is denied, a value appropriate for return to the client
        is returned.  This will be STATUS_ACCESS_DENIED or, when mandatory
        access controls are implemented, STATUS_OBJECT_NOT_FOUND.

    GenerateOnClose - Points to a boolean that is set by the audity
        generation routine and must be passed to NtCloseObjectAuditAlarm
        when the object handle is closed.

    ReturnResultList - If true, GrantedAccess and AccessStatus are actually
        arrays of entries ObjectTypeListLength elements long.

Return Value:

    STATUS_SUCCESS - Indicates the call completed successfully.  In this
        case, ClientStatus receives the result of the access check.

    STATUS_PRIVILEGE_NOT_HELD - Indicates the caller does not have
        sufficient privilege to use this privileged system service.

--*/

{

    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;

    NTSTATUS Status = STATUS_SUCCESS;

    ACCESS_MASK LocalGrantedAccess = (ACCESS_MASK)0;
    PACCESS_MASK LocalGrantedAccessPointer = NULL;
    BOOLEAN LocalGrantedAccessAllocated = FALSE;
    NTSTATUS LocalAccessStatus = STATUS_UNSUCCESSFUL;
    PNTSTATUS LocalAccessStatusPointer = NULL;
    BOOLEAN LocalGenerateOnClose = FALSE;
    POLICY_AUDIT_EVENT_TYPE NtAuditType;

    KPROCESSOR_MODE PreviousMode;

    PUNICODE_STRING CapturedSubsystemName = (PUNICODE_STRING) NULL;
    PUNICODE_STRING CapturedObjectTypeName = (PUNICODE_STRING) NULL;
    PUNICODE_STRING CapturedObjectName = (PUNICODE_STRING) NULL;
    PSECURITY_DESCRIPTOR CapturedSecurityDescriptor = (PSECURITY_DESCRIPTOR) NULL;
    PSID CapturedPrincipalSelfSid = NULL;
    PIOBJECT_TYPE_LIST LocalObjectTypeList = NULL;

    ACCESS_MASK PreviouslyGrantedAccess = (ACCESS_MASK)0;
    GENERIC_MAPPING LocalGenericMapping;

    PPRIVILEGE_SET PrivilegeSet = NULL;

    BOOLEAN Result;

    BOOLEAN AccessGranted;
    BOOLEAN AccessDenied;
    BOOLEAN GenerateSuccessAudit = FALSE;
    BOOLEAN GenerateFailureAudit = FALSE;
    LUID OperationId;
    BOOLEAN AuditPerformed;
    BOOLEAN AvoidAudit = FALSE;

    PTOKEN NewToken = NULL;
    PTOKEN OldToken = NULL;
    BOOLEAN TokenSwapped = FALSE;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    ASSERT( PreviousMode != KernelMode );

    //
    // Capture the subject Context
    //

    SeCaptureSubjectContext ( &SubjectSecurityContext );

    //
    // Convert AuditType
    //

    if ( AuditType == AuditEventObjectAccess ) {
        NtAuditType = AuditCategoryObjectAccess;
    } else if ( AuditType == AuditEventDirectoryServiceAccess ) {
        NtAuditType = AuditCategoryDirectoryServiceAccess;
    } else {
        Status = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }

    //
    // Impersonation checks should be done only if the ClientToken is NULL.
    //

    if ( !ARGUMENT_PRESENT( ClientToken ) ) {

        //
        // Make sure we're impersonating a client...
        //

        if ( (SubjectSecurityContext.ClientToken == NULL) ) {
            Status = STATUS_NO_IMPERSONATION_TOKEN;
            goto Cleanup;
        }


        //
        // ...and at a high enough impersonation level
        //

        if (SubjectSecurityContext.ImpersonationLevel < SecurityIdentification) {
            Status = STATUS_BAD_IMPERSONATION_LEVEL;
            goto Cleanup;
        }
    }

    try {

        if ( ReturnResultList ) {

            if ( ObjectTypeListLength == 0 ) {
                Status = STATUS_INVALID_PARAMETER;
                leave;
            }

            if (!IsValidObjectTypeListCount( ObjectTypeListLength )) {

                Status = STATUS_INVALID_PARAMETER;
                leave;
            }
    
            ProbeForWrite(
                AccessStatus,
                sizeof(NTSTATUS) * ObjectTypeListLength,
                sizeof(ULONG)
                );

            ProbeForWrite(
                GrantedAccess,
                sizeof(ACCESS_MASK) * ObjectTypeListLength,
                sizeof(ULONG)
                );

        } else {
            ProbeForWriteUlong((PULONG)AccessStatus);
            ProbeForWriteUlong((PULONG)GrantedAccess);
        }

        ProbeForReadSmallStructure(
            GenericMapping,
            sizeof(GENERIC_MAPPING),
            sizeof(ULONG)
            );

        LocalGenericMapping = *GenericMapping;

    } except (EXCEPTION_EXECUTE_HANDLER) {

        Status = GetExceptionCode();
    }

    if (!NT_SUCCESS(Status)) {
        goto Cleanup;
    }

    if ( ARGUMENT_PRESENT( ClientToken ) ) {

        Status = ObReferenceObjectByHandle(
                     *ClientToken,                 // Handle
                     (ACCESS_MASK)TOKEN_QUERY,     // DesiredAccess
                     SeTokenObjectType,           // ObjectType
                     PreviousMode,                 // AccessMode
                     (PVOID *)&NewToken,           // Object
                     NULL                          // GrantedAccess
                     );

        if (!NT_SUCCESS(Status)) {
            NewToken = NULL;
            goto Cleanup;
        }

        //
        // Save the old token so that it can be recovered before
        // SeReleaseSubjectContext.
        //

        OldToken = SubjectSecurityContext.ClientToken;

        //
        // Set the impersonation token to the one that has been obtained thru
        // ClientToken handle. This must be freed later in Cleanup.
        //

        SubjectSecurityContext.ClientToken = NewToken;

        TokenSwapped = TRUE;
    }

    //
    // Check for SeAuditPrivilege
    //

    Result = SeCheckAuditPrivilege (
                 &SubjectSecurityContext,
                 PreviousMode
                 );

    if (!Result) {
        if ( Flags & AUDIT_ALLOW_NO_PRIVILEGE ) {
            AvoidAudit = TRUE;
        } else {
            Status = STATUS_PRIVILEGE_NOT_HELD;
            goto Cleanup;
        }
    }

    if (DesiredAccess &
        ( GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL )) {

        Status = STATUS_GENERIC_NOT_MAPPED;
        goto Cleanup;
    }

    //
    // Capture the passed security descriptor.
    //
    // SeCaptureSecurityDescriptor probes the input security descriptor,
    // so we don't have to
    //

    Status = SeCaptureSecurityDescriptor (
                SecurityDescriptor,
                PreviousMode,
                PagedPool,
                FALSE,
                &CapturedSecurityDescriptor
                );

    if (!NT_SUCCESS(Status) ) {
        CapturedSecurityDescriptor = NULL;
        goto Cleanup;
    }

    if ( CapturedSecurityDescriptor == NULL ) {
        Status = STATUS_INVALID_SECURITY_DESCR;
        goto Cleanup;
    }

    //
    // A valid security descriptor must have an owner and a group
    //

    if ( RtlpOwnerAddrSecurityDescriptor(
                (PISECURITY_DESCRIPTOR)CapturedSecurityDescriptor
                ) == NULL ||
         RtlpGroupAddrSecurityDescriptor(
                (PISECURITY_DESCRIPTOR)CapturedSecurityDescriptor
                ) == NULL ) {

        Status = STATUS_INVALID_SECURITY_DESCR;
        goto Cleanup;
    }

    //
    //  Probe and capture the STRING arguments
    //

    try {

        ProbeForWriteBoolean(GenerateOnClose);

        SepProbeAndCaptureString_U ( SubsystemName, &CapturedSubsystemName );

        SepProbeAndCaptureString_U ( ObjectTypeName, &CapturedObjectTypeName );

        SepProbeAndCaptureString_U ( ObjectName, &CapturedObjectName );

    } except (EXCEPTION_EXECUTE_HANDLER) {

        Status = GetExceptionCode();
        goto Cleanup;

    }

    //
    // Capture the PrincipalSelfSid.
    //

    if ( PrincipalSelfSid != NULL ) {
        Status = SeCaptureSid(
                     PrincipalSelfSid,
                     PreviousMode,
                     NULL, 0,
                     PagedPool,
                     TRUE,
                     &CapturedPrincipalSelfSid );

        if (!NT_SUCCESS(Status)) {
            CapturedPrincipalSelfSid = NULL;
            goto Cleanup;
        }
    }

    //
    // Capture any Object type list
    //

    Status = SeCaptureObjectTypeList( ObjectTypeList,
                                      ObjectTypeListLength,
                                      PreviousMode,
                                      &LocalObjectTypeList );

    if (!NT_SUCCESS(Status)) {
        goto Cleanup;
    }

    //
    // See if anything (or everything) in the desired access can be
    // satisfied by privileges.
    //

    Status = SePrivilegePolicyCheck(
                 &DesiredAccess,
                 &PreviouslyGrantedAccess,
                 &SubjectSecurityContext,
                 NULL,
                 &PrivilegeSet,
                 PreviousMode
                 );

    SeLockSubjectContext( &SubjectSecurityContext );

    if (!NT_SUCCESS( Status )) {
        AccessGranted = FALSE;
        AccessDenied = TRUE;
        LocalAccessStatus = Status;

        if ( ReturnResultList ) {
            ULONG ResultListIndex;
            LocalGrantedAccessPointer =
                ExAllocatePoolWithTag( PagedPool, (sizeof(ACCESS_MASK)+sizeof(NTSTATUS)) * ObjectTypeListLength, 'aGeS' );

            if (LocalGrantedAccessPointer == NULL) {
                SeUnlockSubjectContext( &SubjectSecurityContext );
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto Cleanup;
            }
            LocalGrantedAccessAllocated = TRUE;
            LocalAccessStatusPointer = (PNTSTATUS)(LocalGrantedAccessPointer + ObjectTypeListLength);

            for ( ResultListIndex=0; ResultListIndex<ObjectTypeListLength; ResultListIndex++ ) {
                LocalGrantedAccessPointer[ResultListIndex] = LocalGrantedAccess;
                LocalAccessStatusPointer[ResultListIndex] = LocalAccessStatus;
            }

        } else {
        LocalGrantedAccessPointer = &LocalGrantedAccess;
        LocalAccessStatusPointer =  &LocalAccessStatus;
        }

    } else {

        //
        // If the user in the token is the owner of the object, we
        // must automatically grant ReadControl and WriteDac access
        // if desired.  If the DesiredAccess mask is empty after
        // these bits are turned off, we don't have to do any more
        // access checking (ref section 4, DSA ACL Arch)
        //

        if ( DesiredAccess & (WRITE_DAC | READ_CONTROL | MAXIMUM_ALLOWED) ) {

            if (SepTokenIsOwner( SubjectSecurityContext.ClientToken, CapturedSecurityDescriptor, TRUE )) {

                if ( DesiredAccess & MAXIMUM_ALLOWED ) {

                    PreviouslyGrantedAccess |= ( WRITE_DAC | READ_CONTROL );

                } else {

                    PreviouslyGrantedAccess |= (DesiredAccess & (WRITE_DAC | READ_CONTROL));
                }

                DesiredAccess &= ~(WRITE_DAC | READ_CONTROL);
            }

        }

        if (DesiredAccess == 0) {

            LocalGrantedAccess = PreviouslyGrantedAccess;
            if (PreviouslyGrantedAccess == 0){
                AccessGranted = FALSE;
                AccessDenied = TRUE;
                LocalAccessStatus = STATUS_ACCESS_DENIED;
            } else {
                AccessGranted = TRUE;
                AccessDenied = FALSE;
                LocalAccessStatus = STATUS_SUCCESS;
            }

            if ( ReturnResultList ) {
                ULONG ResultListIndex;
                LocalGrantedAccessPointer =
                    ExAllocatePoolWithTag( PagedPool, (sizeof(ACCESS_MASK)+sizeof(NTSTATUS)) * ObjectTypeListLength, 'aGeS' );

                if (LocalGrantedAccessPointer == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    SeUnlockSubjectContext( &SubjectSecurityContext );
                    goto Cleanup;
                }
                LocalGrantedAccessAllocated = TRUE;
                LocalAccessStatusPointer = (PNTSTATUS)(LocalGrantedAccessPointer + ObjectTypeListLength);

                for ( ResultListIndex=0; ResultListIndex<ObjectTypeListLength; ResultListIndex++ ) {
                    LocalGrantedAccessPointer[ResultListIndex] = LocalGrantedAccess;
                    LocalAccessStatusPointer[ResultListIndex] = LocalAccessStatus;
                }

            } else {
            LocalGrantedAccessPointer = &LocalGrantedAccess;
            LocalAccessStatusPointer =  &LocalAccessStatus;
            }

        } else {

            //
            // Finally, do the access check
            //

            if ( ReturnResultList ) {
                LocalGrantedAccessPointer =
                    ExAllocatePoolWithTag( PagedPool, (sizeof(ACCESS_MASK)+sizeof(NTSTATUS)) * ObjectTypeListLength, 'aGeS' );

                if (LocalGrantedAccessPointer == NULL) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    SeUnlockSubjectContext( &SubjectSecurityContext );
                    goto Cleanup;
                }
                LocalGrantedAccessAllocated = TRUE;
                LocalAccessStatusPointer = (PNTSTATUS)(LocalGrantedAccessPointer + ObjectTypeListLength);

            } else {
                LocalGrantedAccessPointer = &LocalGrantedAccess;
                LocalAccessStatusPointer =  &LocalAccessStatus;
            }


            //
            // This does not ask for privilege set to be returned so we can ignore
            // the return value of the call.
            //

            (VOID) SepAccessCheck (
                        CapturedSecurityDescriptor,
                        CapturedPrincipalSelfSid,
                        SubjectSecurityContext.PrimaryToken,
                        SubjectSecurityContext.ClientToken,
                        DesiredAccess,
                        LocalObjectTypeList,
                        ObjectTypeListLength,
                        &LocalGenericMapping,
                        PreviouslyGrantedAccess,
                        PreviousMode,
                        LocalGrantedAccessPointer,
                        NULL,       // Privileges already checked
                        LocalAccessStatusPointer,
                        ReturnResultList,
                        &AccessGranted,
                        &AccessDenied
                        );

        }
    }

    //
    // sound the alarms...
    //

    if ( !AvoidAudit ) {
        if ( SepAdtAuditThisEventWithContext( NtAuditType, AccessGranted, AccessDenied, &SubjectSecurityContext )) {

            SepExamineSaclEx(
                RtlpSaclAddrSecurityDescriptor( (PISECURITY_DESCRIPTOR)CapturedSecurityDescriptor ),
                EffectiveToken( &SubjectSecurityContext ),
                DesiredAccess | PreviouslyGrantedAccess,
                LocalObjectTypeList,
                ObjectTypeListLength,
                ReturnResultList,
                LocalAccessStatusPointer,
                LocalGrantedAccessPointer,
                CapturedPrincipalSelfSid,
                &GenerateSuccessAudit,
                &GenerateFailureAudit
                );
        }

        if ( GenerateSuccessAudit ||
             GenerateFailureAudit ) {

            //
            // Save this to a local here, so we don't
            // have to risk accessing user memory and
            // potentially having to exit before the audit
            //

            if ( AccessGranted ) {

                //
                // SAM calls NtCloseObjectAuditAlarm despite the fact that it may not
                // have successfully opened the object, causing a spurious close audit.
                // Since no one should rely on this anyway if their access attempt
                // failed, make sure it's false and SAM will work properly.
                //

                LocalGenerateOnClose = TRUE;
            }

            //
            // Generate the success audit if needed.
            //
            if ( GenerateSuccessAudit ) {
                ExAllocateLocallyUniqueId( &OperationId );

                // ??
                ASSERT( AccessGranted );
                AuditPerformed = SepAdtOpenObjectAuditAlarm (
                                     CapturedSubsystemName,
                                     AccessGranted ? &HandleId : NULL, // Don't audit handle if failure
                                     CapturedObjectTypeName,
                                     CapturedObjectName,
                                     SubjectSecurityContext.ClientToken,
                                     SubjectSecurityContext.PrimaryToken,
                                     *LocalGrantedAccessPointer,
                                     *LocalGrantedAccessPointer,
                                     &OperationId,
                                     PrivilegeSet,
                                     TRUE,  // Generate success case
                                     PsProcessAuditId( PsGetCurrentProcess() ),
                                     NtAuditType,
                                     LocalObjectTypeList,
                                     ObjectTypeListLength,
                                     ReturnResultList ? LocalGrantedAccessPointer : NULL
                                     );

            }

            //
            // Generate failure audit if it is needed.
            //
            if ( GenerateFailureAudit ) {
                ExAllocateLocallyUniqueId( &OperationId );

                // ??
                ASSERT( AccessDenied );
                AuditPerformed = SepAdtOpenObjectAuditAlarm (
                                     CapturedSubsystemName,
                                     AccessGranted ? &HandleId : NULL, // Don't audit handle if failure
                                     CapturedObjectTypeName,
                                     CapturedObjectName,
                                     SubjectSecurityContext.ClientToken,
                                     SubjectSecurityContext.PrimaryToken,
                                     DesiredAccess,
                                     DesiredAccess,
                                     &OperationId,
                                     PrivilegeSet,
                                     FALSE, // Generate failure case
                                     PsProcessAuditId( PsGetCurrentProcess() ),
                                     NtAuditType,
                                     LocalObjectTypeList,
                                     ObjectTypeListLength,
                                     ReturnResultList ? LocalGrantedAccessPointer : NULL
                                     );
            }
        } else {

            //
            // We didn't generate an audit due to the SACL.  If privileges were used, we need
            // to audit that.  Only audit successful privilege use for opens.
            //

            if ( PrivilegeSet != NULL ) {

                if ( SepAdtAuditThisEventWithContext( AuditCategoryPrivilegeUse, AccessGranted, FALSE, &SubjectSecurityContext) ) {

                    AuditPerformed = SepAdtPrivilegeObjectAuditAlarm ( CapturedSubsystemName,
                                                                       &HandleId,
                                                                       SubjectSecurityContext.ClientToken,
                                                                       SubjectSecurityContext.PrimaryToken,
                                                                       PsProcessAuditId( PsGetCurrentProcess() ),
                                                                       DesiredAccess,
                                                                       PrivilegeSet,
                                                                       AccessGranted
                                                                       );

                    //
                    // We don't want close audits to be generated.  May need to revisit this.
                    //

                    LocalGenerateOnClose = FALSE;
                }
            }
        }
    }

    SeUnlockSubjectContext( &SubjectSecurityContext );

    try {
            if ( ReturnResultList ) {
                ULONG ResultListIndex;
                if ( LocalAccessStatusPointer == NULL ) {
                    for ( ResultListIndex=0; ResultListIndex<ObjectTypeListLength; ResultListIndex++ ) {
                        AccessStatus[ResultListIndex] = LocalAccessStatus;
                        GrantedAccess[ResultListIndex] = LocalGrantedAccess;
                    }
                } else {
                    for ( ResultListIndex=0; ResultListIndex<ObjectTypeListLength; ResultListIndex++ ) {
                        AccessStatus[ResultListIndex] = LocalAccessStatusPointer[ResultListIndex];
                        GrantedAccess[ResultListIndex] = LocalGrantedAccessPointer[ResultListIndex];
                    }
                }

            } else {
                *AccessStatus = LocalAccessStatus;
                *GrantedAccess = LocalGrantedAccess;
            }
            *GenerateOnClose    = LocalGenerateOnClose;
            Status = STATUS_SUCCESS;

    } except (EXCEPTION_EXECUTE_HANDLER) {

        Status = GetExceptionCode();
    }

    //
    // Free locally used resources.
    //
Cleanup:

    if ( TokenSwapped ) {

        //
        // Decrement the reference count for the ClientToken that was passed in.
        //

        ObDereferenceObject( (PVOID)NewToken );

        //
        // Reset the value of the token from saved value.
        //

        SubjectSecurityContext.ClientToken = OldToken;
    }

    //
    // Free any privileges allocated as part of the access check
    //

    if (PrivilegeSet != NULL) {
        ExFreePool( PrivilegeSet );
    }

    SeReleaseSubjectContext ( &SubjectSecurityContext );

    SeReleaseSecurityDescriptor ( CapturedSecurityDescriptor,
                                  PreviousMode,
                                  FALSE );

    if (CapturedSubsystemName != NULL) {
      SepFreeCapturedString( CapturedSubsystemName );
    }

    if (CapturedObjectTypeName != NULL) {
      SepFreeCapturedString( CapturedObjectTypeName );
    }

    if (CapturedObjectName != NULL) {
      SepFreeCapturedString( CapturedObjectName );
    }

    if (CapturedPrincipalSelfSid != NULL) {
        SeReleaseSid( CapturedPrincipalSelfSid, PreviousMode, TRUE);
    }

    if ( LocalObjectTypeList != NULL ) {
        SeFreeCapturedObjectTypeList( LocalObjectTypeList );
    }

    if ( LocalGrantedAccessAllocated ) {
        if ( LocalGrantedAccessPointer != NULL ) {
            ExFreePool( LocalGrantedAccessPointer );
        }
    }

    return Status;
}


NTSTATUS
NtAccessCheckAndAuditAlarm (
    __in PUNICODE_STRING SubsystemName,
    __in_opt PVOID HandleId,
    __in PUNICODE_STRING ObjectTypeName,
    __in PUNICODE_STRING ObjectName,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in ACCESS_MASK DesiredAccess,
    __in PGENERIC_MAPPING GenericMapping,
    __in BOOLEAN ObjectCreation,
    __out PACCESS_MASK GrantedAccess,
    __out PNTSTATUS AccessStatus,
    __out PBOOLEAN GenerateOnClose
    )
/*++

Routine Description:

    See SepAccessCheckAndAuditAlarm.

Arguments:

    See SepAccessCheckAndAuditAlarm.

Return Value:

    STATUS_SUCCESS - Indicates the call completed successfully.  In this
        case, ClientStatus receives the result of the access check.

    STATUS_PRIVILEGE_NOT_HELD - Indicates the caller does not have
        sufficient privilege to use this privileged system service.

--*/

{
    PAGED_CODE();

    UNREFERENCED_PARAMETER( ObjectCreation );
    
    return SepAccessCheckAndAuditAlarm(
            SubsystemName,
            HandleId,
            NULL,
            ObjectTypeName,
            ObjectName,
            SecurityDescriptor,
            NULL,       // No Principal Self sid
            DesiredAccess,
            AuditEventObjectAccess,  // Default to ObjectAccess
            0,          // No Flags
            NULL,       // No ObjectType List
            0,          // No ObjectType List
            GenericMapping,
            GrantedAccess,
            AccessStatus,
            GenerateOnClose,
            FALSE );    // Return a single GrantedAccess and AccessStatus

}


NTSTATUS
NtAccessCheckByTypeAndAuditAlarm (
    __in PUNICODE_STRING SubsystemName,
    __in_opt PVOID HandleId,
    __in PUNICODE_STRING ObjectTypeName,
    __in PUNICODE_STRING ObjectName,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in_opt PSID PrincipalSelfSid,
    __in ACCESS_MASK DesiredAccess,
    __in AUDIT_EVENT_TYPE AuditType,
    __in ULONG Flags,
    __in_ecount_opt(ObjectTypeListLength) POBJECT_TYPE_LIST ObjectTypeList,
    __in ULONG ObjectTypeListLength,
    __in PGENERIC_MAPPING GenericMapping,
    __in BOOLEAN ObjectCreation,
    __out PACCESS_MASK GrantedAccess,
    __out PNTSTATUS AccessStatus,
    __out PBOOLEAN GenerateOnClose
    )
/*++

Routine Description:

    See SepAccessCheckAndAuditAlarm.

Arguments:

    See SepAccessCheckAndAuditAlarm.

Return Value:

    STATUS_SUCCESS - Indicates the call completed successfully.  In this
        case, ClientStatus receives the result of the access check.

    STATUS_PRIVILEGE_NOT_HELD - Indicates the caller does not have
        sufficient privilege to use this privileged system service.

--*/

{
    PAGED_CODE();

    UNREFERENCED_PARAMETER( ObjectCreation );
    
    return SepAccessCheckAndAuditAlarm(
            SubsystemName,
            HandleId,
            NULL,
            ObjectTypeName,
            ObjectName,
            SecurityDescriptor,
            PrincipalSelfSid,
            DesiredAccess,
            AuditType,
            Flags,
            ObjectTypeList,
            ObjectTypeListLength,
            GenericMapping,
            GrantedAccess,
            AccessStatus,
            GenerateOnClose,
            FALSE );  // Return a single GrantedAccess and AccessStatus

}


NTSTATUS
NtAccessCheckByTypeResultListAndAuditAlarm (
    __in PUNICODE_STRING SubsystemName,
    __in_opt PVOID HandleId,
    __in PUNICODE_STRING ObjectTypeName,
    __in PUNICODE_STRING ObjectName,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in_opt PSID PrincipalSelfSid,
    __in ACCESS_MASK DesiredAccess,
    __in AUDIT_EVENT_TYPE AuditType,
    __in ULONG Flags,
    __in_ecount_opt(ObjectTypeListLength) POBJECT_TYPE_LIST ObjectTypeList,
    __in ULONG ObjectTypeListLength,
    __in PGENERIC_MAPPING GenericMapping,
    __in BOOLEAN ObjectCreation,
    __out_ecount(ObjectTypeListLength) PACCESS_MASK GrantedAccess,
    __out_ecount(ObjectTypeListLength) PNTSTATUS AccessStatus,
    __out PBOOLEAN GenerateOnClose
    )
/*++

Routine Description:

    See SepAccessCheckAndAuditAlarm.

Arguments:

    See SepAccessCheckAndAuditAlarm.

Return Value:

    STATUS_SUCCESS - Indicates the call completed successfully.  In this
        case, ClientStatus receives the result of the access check.

    STATUS_PRIVILEGE_NOT_HELD - Indicates the caller does not have
        sufficient privilege to use this privileged system service.

--*/

{
    PAGED_CODE();

    UNREFERENCED_PARAMETER( ObjectCreation );
    
    return SepAccessCheckAndAuditAlarm(
            SubsystemName,
            HandleId,
            NULL,
            ObjectTypeName,
            ObjectName,
            SecurityDescriptor,
            PrincipalSelfSid,
            DesiredAccess,
            AuditType,
            Flags,
            ObjectTypeList,
            ObjectTypeListLength,
            GenericMapping,
            GrantedAccess,
            AccessStatus,
            GenerateOnClose,
            TRUE );  // Return an array of GrantedAccess and AccessStatus

}


NTSTATUS
NtAccessCheckByTypeResultListAndAuditAlarmByHandle (
    __in PUNICODE_STRING SubsystemName,
    __in_opt PVOID HandleId,
    __in HANDLE ClientToken,
    __in PUNICODE_STRING ObjectTypeName,
    __in PUNICODE_STRING ObjectName,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in_opt PSID PrincipalSelfSid,
    __in ACCESS_MASK DesiredAccess,
    __in AUDIT_EVENT_TYPE AuditType,
    __in ULONG Flags,
    __in_ecount_opt(ObjectTypeListLength) POBJECT_TYPE_LIST ObjectTypeList,
    __in ULONG ObjectTypeListLength,
    __in PGENERIC_MAPPING GenericMapping,
    __in BOOLEAN ObjectCreation,
    __out_ecount(ObjectTypeListLength) PACCESS_MASK GrantedAccess,
    __out_ecount(ObjectTypeListLength) PNTSTATUS AccessStatus,
    __out PBOOLEAN GenerateOnClose
    )
/*++

Routine Description:

    See SepAccessCheckAndAuditAlarm.

Arguments:

    See SepAccessCheckAndAuditAlarm.

Return Value:

    STATUS_SUCCESS - Indicates the call completed successfully.  In this
        case, ClientStatus receives the result of the access check.

    STATUS_PRIVILEGE_NOT_HELD - Indicates the caller does not have
        sufficient privilege to use this privileged system service.

--*/

{
    PAGED_CODE();

    UNREFERENCED_PARAMETER( ObjectCreation );
    
    return SepAccessCheckAndAuditAlarm(
            SubsystemName,
            HandleId,
            &ClientToken,
            ObjectTypeName,
            ObjectName,
            SecurityDescriptor,
            PrincipalSelfSid,
            DesiredAccess,
            AuditType,
            Flags,
            ObjectTypeList,
            ObjectTypeListLength,
            GenericMapping,
            GrantedAccess,
            AccessStatus,
            GenerateOnClose,
            TRUE );  // Return an array of GrantedAccess and AccessStatus

}


NTSTATUS
NtOpenObjectAuditAlarm (
    __in PUNICODE_STRING SubsystemName,
    __in_opt PVOID HandleId,
    __in PUNICODE_STRING ObjectTypeName,
    __in PUNICODE_STRING ObjectName,
    __in_opt PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in HANDLE ClientToken,
    __in ACCESS_MASK DesiredAccess,
    __in ACCESS_MASK GrantedAccess,
    __in_opt PPRIVILEGE_SET Privileges,
    __in BOOLEAN ObjectCreation,
    __in BOOLEAN AccessGranted,
    __out PBOOLEAN GenerateOnClose
    )
/*++

    Routine Description:

    This routine is used to generate audit and alarm messages when an
    attempt is made to access an existing protected subsystem object or
    create a new one.  This routine may result in several messages being
    generated and sent to Port objects.  This may result in a significant
    latency before returning.  Design of routines that must call this
    routine must take this potential latency into account.  This may have
    an impact on the approach taken for data structure mutex locking, for
    example.

    This routine may not be able to generate a complete audit record
    due to memory restrictions.

    This API requires the caller have SeAuditPrivilege privilege.  The test
    for this privilege is always against the primary token of the calling
    process, not the impersonation token of the thread.

Arguments:

    SubsystemName - Supplies a name string identifying the
        subsystem calling the routine.

    HandleId - A unique value representing the client's handle to the
        object.  If the access attempt was not successful (AccessGranted is
        FALSE), then this parameter is ignored.

    ObjectTypeName - Supplies the name of the type of object being
        accessed.

    ObjectName - Supplies the name of the object the client
        accessed or attempted to access.

    SecurityDescriptor - An optional pointer to the security descriptor of
        the object being accessed.

    ClientToken - A handle to a token object representing the client that
        requested the operation.  This handle must be obtained from a
        communication session layer, such as from an LPC Port or Local
        Named Pipe, to prevent possible security policy violations.

    DesiredAccess - The desired access mask.  This mask must have been
        previously mapped to contain no generic accesses.

    GrantedAccess - The mask of accesses that were actually granted.

    Privileges - Optionally points to a set of privileges that were
        required for the access attempt.  Those privileges that were held
        by the subject are marked using the UsedForAccess flag of the
        attributes associated with each privilege.

    ObjectCreation - A boolean flag indicating whether the access will
        result in a new object being created if granted.  A value of TRUE
        indicates an object will be created, FALSE indicates an existing
        object will be opened.

    AccessGranted - Indicates whether the requested access was granted or
        not.  A value of TRUE indicates the access was granted.  A value of
        FALSE indicates the access was not granted.

    GenerateOnClose - Points to a boolean that is set by the audit
        generation routine and must be passed to NtCloseObjectAuditAlarm()
        when the object handle is closed.

Return Value:

--*/
{

    KPROCESSOR_MODE PreviousMode;
    ULONG PrivilegeParameterLength;
    PUNICODE_STRING CapturedSubsystemName = (PUNICODE_STRING) NULL;
    PUNICODE_STRING CapturedObjectTypeName = (PUNICODE_STRING) NULL;
    PUNICODE_STRING CapturedObjectName = (PUNICODE_STRING) NULL;
    PSECURITY_DESCRIPTOR CapturedSecurityDescriptor = (PSECURITY_DESCRIPTOR) NULL;
    PPRIVILEGE_SET CapturedPrivileges = NULL;
    BOOLEAN LocalGenerateOnClose = FALSE;
    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;
    BOOLEAN Result;
    NTSTATUS Status;
    BOOLEAN GenerateAudit = FALSE;
    BOOLEAN GenerateAlarm = FALSE;
    PLUID ClientAuthenticationId = NULL;
    HANDLE CapturedHandleId = NULL;
    BOOLEAN AuditPerformed;
    ULONG PrivilegeCount;

    PTOKEN Token;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( ObjectCreation );
    
    PreviousMode = KeGetPreviousMode();

    ASSERT( PreviousMode != KernelMode );

    Status = ObReferenceObjectByHandle( ClientToken,             // Handle
                                        TOKEN_QUERY,             // DesiredAccess
                                        SeTokenObjectType,      // ObjectType
                                        PreviousMode,            // AccessMode
                                        (PVOID *)&Token,         // Object
                                        NULL                     // GrantedAccess
                                        );

    if (!NT_SUCCESS(Status)) {
        return( Status );
    }

    //
    // If the passed token is an impersonation token, make sure
    // it is at SecurityIdentification or above.
    //

    if (Token->TokenType == TokenImpersonation) {

        if (Token->ImpersonationLevel < SecurityIdentification) {

            ObDereferenceObject( (PVOID)Token );

            return( STATUS_BAD_IMPERSONATION_LEVEL );

        }
    }

    //
    // Check for SeAuditPrivilege.  This must be tested against
    // the caller's primary token.
    //

    SeCaptureSubjectContext ( &SubjectSecurityContext );

    Result = SeCheckAuditPrivilege (
                 &SubjectSecurityContext,
                 PreviousMode
                 );

    if (!Result) {

        ObDereferenceObject( (PVOID)Token );

        SeReleaseSubjectContext ( &SubjectSecurityContext );

        return(STATUS_PRIVILEGE_NOT_HELD);
    }

    //
    // This will just return NULL if the input descriptor is NULL
    //

    Status = SeCaptureSecurityDescriptor ( SecurityDescriptor,
                                           PreviousMode,
                                           PagedPool,
                                           FALSE,
                                           &CapturedSecurityDescriptor
                                           );

    //
    // At this point in time, if there's no security descriptor, there's
    // nothing to do.  Return success.
    //

    if (!NT_SUCCESS( Status ) || CapturedSecurityDescriptor == NULL) {

        ObDereferenceObject( (PVOID)Token );

        SeReleaseSubjectContext ( &SubjectSecurityContext );

        return( Status );
    }

    try {

        //
        // Only capture the privileges if we've completed a successful
        // access check.  Otherwise they don't mean anything.
        //

        if (AccessGranted && ARGUMENT_PRESENT(Privileges)) {

            ProbeForReadSmallStructure(
                Privileges,
                sizeof(PRIVILEGE_SET),
                sizeof(ULONG)
                );

            PrivilegeCount = Privileges->PrivilegeCount;

            if (!IsValidPrivilegeCount( PrivilegeCount )) {
                Status = STATUS_INVALID_PARAMETER;
                leave;
            }

            PrivilegeParameterLength = (ULONG)sizeof(PRIVILEGE_SET) +
                              ((PrivilegeCount - ANYSIZE_ARRAY) *
                                (ULONG)sizeof(LUID_AND_ATTRIBUTES)  );

            ProbeForRead(
                Privileges,
                PrivilegeParameterLength,
                sizeof(ULONG)
                );

            CapturedPrivileges = ExAllocatePoolWithTag( PagedPool,
                                                        PrivilegeParameterLength,
                                                        'rPeS'
                                                      );

            if (CapturedPrivileges != NULL) {

                RtlCopyMemory ( CapturedPrivileges,
                                Privileges,
                                PrivilegeParameterLength );
                CapturedPrivileges->PrivilegeCount = PrivilegeCount;
            } else {

                SeReleaseSecurityDescriptor ( CapturedSecurityDescriptor,
                                              PreviousMode,
                                              FALSE );

                ObDereferenceObject( (PVOID)Token );
                SeReleaseSubjectContext ( &SubjectSecurityContext );
                return( STATUS_INSUFFICIENT_RESOURCES );
            }


        }

        if (ARGUMENT_PRESENT( HandleId )) {

            ProbeForReadSmallStructure( (PHANDLE)HandleId, sizeof(PVOID), sizeof(PVOID) );
            CapturedHandleId = *(PHANDLE)HandleId;
        }

        ProbeForWriteBoolean(GenerateOnClose);

        //
        // Probe and Capture the parameter strings.
        // If we run out of memory attempting to capture
        // the strings, the returned pointer will be
        // NULL and we will continue with the audit.
        //

        SepProbeAndCaptureString_U ( SubsystemName,
                                     &CapturedSubsystemName );

        SepProbeAndCaptureString_U ( ObjectTypeName,
                                     &CapturedObjectTypeName );

        SepProbeAndCaptureString_U ( ObjectName,
                                     &CapturedObjectName );

    } except(EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
    }

    if (!NT_SUCCESS(Status)) {

        if (CapturedSubsystemName != NULL) {
          SepFreeCapturedString( CapturedSubsystemName );
        }

        if (CapturedObjectTypeName != NULL) {
          SepFreeCapturedString( CapturedObjectTypeName );
        }

        if (CapturedObjectName != NULL) {
          SepFreeCapturedString( CapturedObjectName );
        }

        if (CapturedPrivileges != NULL) {
          ExFreePool( CapturedPrivileges );
        }

        if (CapturedSecurityDescriptor != NULL) {

            SeReleaseSecurityDescriptor ( CapturedSecurityDescriptor,
                                          PreviousMode,
                                          FALSE );
        }

        ObDereferenceObject( (PVOID)Token );

        SeReleaseSubjectContext ( &SubjectSecurityContext );

        return Status;

    }

    if ( SepAdtAuditThisEventWithContext( AuditCategoryObjectAccess, AccessGranted, !AccessGranted, &SubjectSecurityContext ) ) {

        SepExamineSacl(
            RtlpSaclAddrSecurityDescriptor( (PISECURITY_DESCRIPTOR)CapturedSecurityDescriptor ),
            Token,
            DesiredAccess | GrantedAccess,
            AccessGranted,
            &GenerateAudit,
            &GenerateAlarm
            );

        if (GenerateAudit || GenerateAlarm) {

            LocalGenerateOnClose = TRUE;

            AuditPerformed = SepAdtOpenObjectAuditAlarm ( CapturedSubsystemName,
                                                          ARGUMENT_PRESENT(HandleId) ? (PVOID)&CapturedHandleId : NULL,
                                                          CapturedObjectTypeName,
                                                          CapturedObjectName,
                                                          Token,
                                                          SubjectSecurityContext.PrimaryToken,
                                                          DesiredAccess,
                                                          GrantedAccess,
                                                          NULL,
                                                          CapturedPrivileges,
                                                          AccessGranted,
                                                          PsProcessAuditId( PsGetCurrentProcess() ),
                                                          AuditCategoryObjectAccess,
                                                          NULL,
                                                          0,
                                                          NULL
                                                          );

            LocalGenerateOnClose = AuditPerformed;
        }
    }

    if ( !(GenerateAudit || GenerateAlarm) ) {

        //
        // We didn't attempt to generate an audit above, so if privileges were used,
        // see if we should generate an audit here.
        //

        if ( ARGUMENT_PRESENT(Privileges) ) {

            if ( SepAdtAuditThisEventWithContext( AuditCategoryPrivilegeUse, AccessGranted, FALSE, &SubjectSecurityContext ) ) {

                AuditPerformed = SepAdtPrivilegeObjectAuditAlarm ( CapturedSubsystemName,
                                                                   CapturedHandleId,
                                                                   Token,
                                                                   SubjectSecurityContext.PrimaryToken,
                                                                   PsProcessAuditId( PsGetCurrentProcess() ),
                                                                   DesiredAccess,
                                                                   CapturedPrivileges,
                                                                   AccessGranted
                                                                   );
                //
                // If we generate an audit due to use of privilege, don't set generate on close,
                // because then we'll have a close audit without a corresponding open audit.
                //

                LocalGenerateOnClose = FALSE;
            }
        }
    }

    if (CapturedSecurityDescriptor != NULL) {

        SeReleaseSecurityDescriptor ( CapturedSecurityDescriptor,
                                      PreviousMode,
                                      FALSE );
    }

    if (CapturedSubsystemName != NULL) {
      SepFreeCapturedString( CapturedSubsystemName );
    }

    if (CapturedObjectTypeName != NULL) {
      SepFreeCapturedString( CapturedObjectTypeName );
    }

    if (CapturedObjectName != NULL) {
      SepFreeCapturedString( CapturedObjectName );
    }

    if (CapturedPrivileges != NULL) {
      ExFreePool( CapturedPrivileges );
    }

    ObDereferenceObject( (PVOID)Token );

    SeReleaseSubjectContext ( &SubjectSecurityContext );

    try {

        *GenerateOnClose = LocalGenerateOnClose;

    } except (EXCEPTION_EXECUTE_HANDLER) {

            return GetExceptionCode();
    }

    return(STATUS_SUCCESS);
}



NTSTATUS
NtCloseObjectAuditAlarm (
    __in PUNICODE_STRING SubsystemName,
    __in_opt PVOID HandleId,
    __in BOOLEAN GenerateOnClose
    )

/*++

Routine Description:

    This routine is used to generate audit and alarm messages when a handle
    to a protected subsystem object is deleted.  This routine may result in
    several messages being generated and sent to Port objects.  This may
    result in a significant latency before returning.  Design of routines
    that must call this routine must take this potential latency into
    account.  This may have an impact on the approach taken for data
    structure mutex locking, for example.

    This API requires the caller have SeAuditPrivilege privilege.  The test
    for this privilege is always against the primary token of the calling
    process, allowing the caller to be impersonating a client during the
    call with no ill effects.

Arguments:

    SubsystemName - Supplies a name string identifying the subsystem
        calling the routine.

    HandleId - A unique value representing the client's handle to the
        object.

    GenerateOnClose - Is a boolean value returned from a corresponding
        NtAccessCheckAndAuditAlarm() call or NtOpenObjectAuditAlarm() call
        when the object handle was created.

Return value:

--*/

{
    BOOLEAN Result;
    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;
    KPROCESSOR_MODE PreviousMode;
    PUNICODE_STRING CapturedSubsystemName = NULL;
    PSID UserSid;
    PSID CapturedUserSid = NULL;
    NTSTATUS Status;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    ASSERT(PreviousMode != KernelMode);

    if (!GenerateOnClose) {
        return( STATUS_SUCCESS );
    }

    //
    // Check for SeAuditPrivilege
    //

    SeCaptureSubjectContext ( &SubjectSecurityContext );

    Result = SeCheckAuditPrivilege (
                 &SubjectSecurityContext,
                 PreviousMode
                 );

    if (!Result) {
        Status = STATUS_PRIVILEGE_NOT_HELD;
        goto Cleanup;
    }

    UserSid = SepTokenUserSid( EffectiveToken (&SubjectSecurityContext));

    CapturedUserSid = ExAllocatePoolWithTag(
                          PagedPool,
                          SeLengthSid( UserSid ),
                          'iSeS'
                          );

    if ( CapturedUserSid == NULL ) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }

    Status =  RtlCopySid (
                  SeLengthSid( UserSid ),
                  CapturedUserSid,
                  UserSid
                  );

    ASSERT( NT_SUCCESS( Status ));


    try {

        SepProbeAndCaptureString_U ( SubsystemName,
                                   &CapturedSubsystemName );

    } except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
        goto Cleanup;
    }

    //
    // This routine will check to see if auditing is enabled
    //

    SepAdtCloseObjectAuditAlarm( CapturedSubsystemName, HandleId, CapturedUserSid );

    Status = STATUS_SUCCESS;

Cleanup:
    if ( CapturedSubsystemName != NULL ) {
        SepFreeCapturedString( CapturedSubsystemName );
    }

    if ( CapturedUserSid != NULL ) {
        ExFreePool( CapturedUserSid );
    }

    SeReleaseSubjectContext ( &SubjectSecurityContext );

    return Status;
}


NTSTATUS
NtDeleteObjectAuditAlarm (
    __in PUNICODE_STRING SubsystemName,
    __in_opt PVOID HandleId,
    __in BOOLEAN GenerateOnClose
    )

/*++

Routine Description:

    This routine is used to generate audit and alarm messages when an object
    in a protected subsystem object is deleted.  This routine may result in
    several messages being generated and sent to Port objects.  This may
    result in a significant latency before returning.  Design of routines
    that must call this routine must take this potential latency into
    account.  This may have an impact on the approach taken for data
    structure mutex locking, for example.

    This API requires the caller have SeAuditPrivilege privilege.  The test
    for this privilege is always against the primary token of the calling
    process, allowing the caller to be impersonating a client during the
    call with no ill effects.

Arguments:

    SubsystemName - Supplies a name string identifying the subsystem
        calling the routine.

    HandleId - A unique value representing the client's handle to the
        object.

    GenerateOnClose - Is a boolean value returned from a corresponding
        NtAccessCheckAndAuditAlarm() call or NtOpenObjectAuditAlarm() call
        when the object handle was created.

Return value:

--*/

{
    BOOLEAN Result;
    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;
    KPROCESSOR_MODE PreviousMode;
    PUNICODE_STRING CapturedSubsystemName = NULL;
    PSID UserSid;
    PSID CapturedUserSid;
    NTSTATUS Status;

    PAGED_CODE();

    PreviousMode = KeGetPreviousMode();

    ASSERT(PreviousMode != KernelMode);

    if (!GenerateOnClose) {
        return( STATUS_SUCCESS );
    }

    //
    // Check for SeAuditPrivilege
    //

    SeCaptureSubjectContext ( &SubjectSecurityContext );

    Result = SeCheckAuditPrivilege (
                 &SubjectSecurityContext,
                 PreviousMode
                 );

    if (!Result) {

        SeReleaseSubjectContext ( &SubjectSecurityContext );
        return(STATUS_PRIVILEGE_NOT_HELD);
    }

    UserSid = SepTokenUserSid( EffectiveToken (&SubjectSecurityContext));

    CapturedUserSid = ExAllocatePoolWithTag(
                          PagedPool,
                          SeLengthSid( UserSid ),
                          'iSeS'
                          );

    if ( CapturedUserSid == NULL ) {
        SeReleaseSubjectContext ( &SubjectSecurityContext );
        return( STATUS_INSUFFICIENT_RESOURCES );
    }

    Status =  RtlCopySid (
                  SeLengthSid( UserSid ),
                  CapturedUserSid,
                  UserSid
                  );

    ASSERT( NT_SUCCESS( Status ));


    try {

        SepProbeAndCaptureString_U ( SubsystemName,
                                   &CapturedSubsystemName );

    } except (EXCEPTION_EXECUTE_HANDLER) {

        if ( CapturedSubsystemName != NULL ) {
            SepFreeCapturedString( CapturedSubsystemName );
        }

        ExFreePool( CapturedUserSid );
        SeReleaseSubjectContext ( &SubjectSecurityContext );
        return GetExceptionCode();

    }

    //
    // This routine will check to see if auditing is enabled
    //

    SepAdtDeleteObjectAuditAlarm ( CapturedSubsystemName,
                               HandleId,
                               CapturedUserSid
                               );

    SeReleaseSubjectContext ( &SubjectSecurityContext );

    if ( CapturedSubsystemName != NULL ) {
        SepFreeCapturedString( CapturedSubsystemName );
    }

    ExFreePool( CapturedUserSid );

    return(STATUS_SUCCESS);
}


VOID
SeOpenObjectAuditAlarm (
    __in PUNICODE_STRING ObjectTypeName,
    __in_opt PVOID Object,
    __in_opt PUNICODE_STRING AbsoluteObjectName,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in PACCESS_STATE AccessState,
    __in BOOLEAN ObjectCreated,
    __in BOOLEAN AccessGranted,
    __in KPROCESSOR_MODE AccessMode,
    __out PBOOLEAN GenerateOnClose
    )
/*++

Routine Description:

    SeOpenObjectAuditAlarm is used by the object manager that open objects
    to generate any necessary audit or alarm messages.  The open may be to
    existing objects or for newly created objects.  No messages will be
    generated for Kernel mode accesses.

    This routine is used to generate audit and alarm messages when an
    attempt is made to open an object.

    This routine may result in several messages being generated and sent to
    Port objects.  This may result in a significant latency before
    returning.  Design of routines that must call this routine must take
    this potential latency into account.  This may have an impact on the
    approach taken for data structure mutex locking, for example.

Arguments:

    ObjectTypeName - Supplies the name of the type of object being
        accessed.  This must be the same name provided to the
        ObCreateObjectType service when the object type was created.

    Object - Address of the object accessed.  This value will not be used
        as a pointer (referenced).  It is necessary only to enter into log
        messages.  If the open was not successful, then this argument is
        ignored.  Otherwise, it must be provided.

    AbsoluteObjectName - Supplies the name of the object being accessed.
        If the object doesn't have a name, then this field is left null.
        Otherwise, it must be provided.

    SecurityDescriptor - A pointer to the security descriptor of the
        object being accessed.

    AccessState - A pointer to an access state structure containing the
        subject context, the remaining desired access types, the granted
        access types, and optionally a privilege set to indicate which
        privileges were used to permit the access.

    ObjectCreated - A boolean flag indicating whether the access resulted
        in a new object being created.  A value of TRUE indicates an object
        was created, FALSE indicates an existing object was opened.

    AccessGranted - Indicates if the access was granted or denied based on
        the access check or privilege check.

    AccessMode - Indicates the access mode used for the access check.  One
        of UserMode or KernelMode.  Messages will not be generated by
        kernel mode accesses.

    GenerateOnClose - Points to a boolean that is set by the audit
        generation routine and must be passed to SeCloseObjectAuditAlarm()
        when the object handle is closed.

Return value:

    None.

--*/
{
    BOOLEAN GenerateAudit = FALSE;
    BOOLEAN GenerateAlarm = FALSE;
    ACCESS_MASK RequestedAccess;
    POBJECT_NAME_INFORMATION ObjectNameInfo = NULL;
    PUNICODE_STRING ObjectTypeNameInfo = NULL;
    PUNICODE_STRING ObjectName = NULL;
    PUNICODE_STRING LocalObjectTypeName = NULL;
    PLUID PrimaryAuthenticationId = NULL;
    PLUID ClientAuthenticationId = NULL;
    BOOLEAN AuditPrivileges = FALSE;
    BOOLEAN AuditPerformed;
    PTOKEN Token;
    ACCESS_MASK MappedGrantMask = (ACCESS_MASK)0;
    ACCESS_MASK MappedDenyMask = (ACCESS_MASK)0;
    PAUX_ACCESS_DATA AuxData;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( ObjectCreated );
    
    if ( AccessMode == KernelMode ) {
        return;
    }

    AuxData = (PAUX_ACCESS_DATA)AccessState->AuxData;

    Token = EffectiveToken( &AccessState->SubjectSecurityContext );

    if (ARGUMENT_PRESENT(Token->AuditData)) {

        MappedGrantMask = Token->AuditData->GrantMask;

        RtlMapGenericMask(
            &MappedGrantMask,
            &AuxData->GenericMapping
            );

        MappedDenyMask = Token->AuditData->DenyMask;

        RtlMapGenericMask(
            &MappedDenyMask,
            &AuxData->GenericMapping
            );
    }

    if (SecurityDescriptor != NULL) {

        RequestedAccess = AccessState->RemainingDesiredAccess |
                          AccessState->PreviouslyGrantedAccess;

        if ( SepAdtAuditThisEventWithContext( AuditCategoryObjectAccess, AccessGranted, !AccessGranted, &AccessState->SubjectSecurityContext )) {

            if ( RequestedAccess & (AccessGranted ? MappedGrantMask : MappedDenyMask)) {

                GenerateAudit = TRUE;

            } else {

                SepExamineSacl(
                    RtlpSaclAddrSecurityDescriptor( (PISECURITY_DESCRIPTOR)SecurityDescriptor ),
                    Token,
                    RequestedAccess,
                    AccessGranted,
                    &GenerateAudit,
                    &GenerateAlarm
                    );
            }

            //
            // Only generate an audit on close of we're auditing from SACL
            // settings.
            //

            if (GenerateAudit) {

                *GenerateOnClose = TRUE;

                //
                // Construct the audit mask that will be placed into the handle.
                //

                if (AccessGranted) {

                    SeMaximumAuditMask(
                        RtlpSaclAddrSecurityDescriptor( (PISECURITY_DESCRIPTOR)SecurityDescriptor ),
                        RequestedAccess,
                        Token,
                        &AuxData->MaximumAuditMask
                        );
                }
            }
        }
    }

    //
    // If we don't generate an audit via the SACL, see if we need to generate
    // one for privilege use.
    //
    // Note that we only audit privileges successfully used to open objects,
    // so we don't care about a failed privilege use here.  Therefore, only
    // do this test of access has been granted.
    //

    if (!GenerateAudit && AccessGranted) {

        if ( SepAdtAuditThisEventWithContext( AuditCategoryPrivilegeUse, AccessGranted, FALSE, &AccessState->SubjectSecurityContext )) {

            if ((AuxData->PrivilegesUsed != NULL) &&
                (AuxData->PrivilegesUsed->PrivilegeCount > 0) ) {

                //
                // Make sure these are actually privileges that we want to audit
                //

                if (SepFilterPrivilegeAudits( 0, AuxData->PrivilegesUsed )) {

                    GenerateAudit = TRUE;

                    //
                    // When we finally try to generate this audit, this flag
                    // will tell us that we need to audit the fact that we
                    // used a privilege, as opposed to audit due to the SACL.
                    //

                    AccessState->AuditPrivileges = TRUE;
                }
            }
        }
    }

    //
    // Set up either to generate an audit (if the access check has failed), or save
    // the stuff that we're going to audit later into the AccessState structure.
    //

    if (GenerateAudit || GenerateAlarm) {

        AccessState->GenerateAudit = TRUE;

        //
        // Figure out what we've been passed, and obtain as much
        // missing information as possible.
        //

        if ( !ARGUMENT_PRESENT( AbsoluteObjectName )) {

            if ( ARGUMENT_PRESENT( Object )) {

                ObjectNameInfo = SepQueryNameString( Object  );

                if ( ObjectNameInfo != NULL ) {

                    ObjectName = &ObjectNameInfo->Name;
                }
            }

        } else {

            ObjectName = AbsoluteObjectName;
        }

        if ( !ARGUMENT_PRESENT( ObjectTypeName )) {

            if ( ARGUMENT_PRESENT( Object )) {

                ObjectTypeNameInfo = SepQueryTypeString( Object );

                if ( ObjectTypeNameInfo != NULL ) {

                    LocalObjectTypeName = ObjectTypeNameInfo;
                }
            }

        } else {

            LocalObjectTypeName = ObjectTypeName;
        }

        //
        // If the access attempt failed, do the audit here.  If it succeeded,
        // we'll do the audit later, when the handle is allocated.
        //
        //

        if (!AccessGranted) {

            AuditPerformed = SepAdtOpenObjectAuditAlarm ( (PUNICODE_STRING)&SeSubsystemName,
                                                          NULL,
                                                          LocalObjectTypeName,
                                                          ObjectName,
                                                          AccessState->SubjectSecurityContext.ClientToken,
                                                          AccessState->SubjectSecurityContext.PrimaryToken,
                                                          AccessState->OriginalDesiredAccess,
                                                          AccessState->PreviouslyGrantedAccess,
                                                          &AccessState->OperationID,
                                                          AuxData->PrivilegesUsed,
                                                          FALSE,
                                                          AccessState->SubjectSecurityContext.ProcessAuditId,
                                                          AuditCategoryObjectAccess,
                                                          NULL,
                                                          0,
                                                          NULL );
        } else {

            //
            // Copy all the stuff we're going to need into the
            // AccessState and return.
            //

            if ( ObjectName != NULL ) {

                 if ( AccessState->ObjectName.Buffer != NULL ) {

                     ExFreePool( AccessState->ObjectName.Buffer );
                     AccessState->ObjectName.Length = 0;
                     AccessState->ObjectName.MaximumLength = 0;
                 }

                AccessState->ObjectName.Buffer = ExAllocatePool( PagedPool,ObjectName->MaximumLength );
                if (AccessState->ObjectName.Buffer != NULL) {

                    AccessState->ObjectName.MaximumLength = ObjectName->MaximumLength;
                    RtlCopyUnicodeString( &AccessState->ObjectName, ObjectName );
                }
            }

            if ( LocalObjectTypeName != NULL ) {

                 if ( AccessState->ObjectTypeName.Buffer != NULL ) {

                     ExFreePool( AccessState->ObjectTypeName.Buffer );
                     AccessState->ObjectTypeName.Length = 0;
                     AccessState->ObjectTypeName.MaximumLength = 0;
                 }

                AccessState->ObjectTypeName.Buffer = ExAllocatePool( PagedPool, LocalObjectTypeName->MaximumLength );
                if (AccessState->ObjectTypeName.Buffer != NULL) {

                    AccessState->ObjectTypeName.MaximumLength = LocalObjectTypeName->MaximumLength;
                    RtlCopyUnicodeString( &AccessState->ObjectTypeName, LocalObjectTypeName );
                }
            }
        }

        if ( ObjectNameInfo != NULL ) {

            ExFreePool( ObjectNameInfo );
        }

        if ( ObjectTypeNameInfo != NULL ) {

            ExFreePool( ObjectTypeNameInfo );
        }
    }

    return;
}


VOID
SeOpenObjectForDeleteAuditAlarm (
    __in PUNICODE_STRING ObjectTypeName,
    __in_opt PVOID Object,
    __in_opt PUNICODE_STRING AbsoluteObjectName,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in PACCESS_STATE AccessState,
    __in BOOLEAN ObjectCreated,
    __in BOOLEAN AccessGranted,
    __in KPROCESSOR_MODE AccessMode,
    __out PBOOLEAN GenerateOnClose
    )
/*++

Routine Description:

    SeOpenObjectForDeleteAuditAlarm is used by the object manager that open
    objects to generate any necessary audit or alarm messages.  The open may
    be to existing objects or for newly created objects.  No messages will be
    generated for Kernel mode accesses.

    This routine is used to generate audit and alarm messages when an
    attempt is made to open an object with the intent to delete it.
    Specifically, this is used by file systems when the flag
    FILE_DELETE_ON_CLOSE is specified.

    This routine may result in several messages being generated and sent to
    Port objects.  This may result in a significant latency before
    returning.  Design of routines that must call this routine must take
    this potential latency into account.  This may have an impact on the
    approach taken for data structure mutex locking, for example.

Arguments:

    ObjectTypeName - Supplies the name of the type of object being
        accessed.  This must be the same name provided to the
        ObCreateObjectType service when the object type was created.

    Object - Address of the object accessed.  This value will not be used
        as a pointer (referenced).  It is necessary only to enter into log
        messages.  If the open was not successful, then this argument is
        ignored.  Otherwise, it must be provided.

    AbsoluteObjectName - Supplies the name of the object being accessed.
        If the object doesn't have a name, then this field is left null.
        Otherwise, it must be provided.

    SecurityDescriptor - A pointer to the security descriptor of the
        object being accessed.

    AccessState - A pointer to an access state structure containing the
        subject context, the remaining desired access types, the granted
        access types, and optionally a privilege set to indicate which
        privileges were used to permit the access.

    ObjectCreated - A boolean flag indicating whether the access resulted
        in a new object being created.  A value of TRUE indicates an object
        was created, FALSE indicates an existing object was opened.

    AccessGranted - Indicates if the access was granted or denied based on
        the access check or privilege check.

    AccessMode - Indicates the access mode used for the access check.  One
        of UserMode or KernelMode.  Messages will not be generated by
        kernel mode accesses.

    GenerateOnClose - Points to a boolean that is set by the audit
        generation routine and must be passed to SeCloseObjectAuditAlarm()
        when the object handle is closed.

Return value:

    None.

--*/
{
    BOOLEAN GenerateAudit = FALSE;
    BOOLEAN GenerateAlarm = FALSE;
    ACCESS_MASK RequestedAccess;
    POBJECT_NAME_INFORMATION ObjectNameInfo = NULL;
    PUNICODE_STRING ObjectTypeNameInfo = NULL;
    PUNICODE_STRING ObjectName = NULL;
    PUNICODE_STRING LocalObjectTypeName = NULL;
    PLUID PrimaryAuthenticationId = NULL;
    PLUID ClientAuthenticationId = NULL;
    BOOLEAN AuditPrivileges = FALSE;
    BOOLEAN AuditPerformed;
    PTOKEN Token;
    ACCESS_MASK MappedGrantMask = (ACCESS_MASK)0;
    ACCESS_MASK MappedDenyMask = (ACCESS_MASK)0;
    PAUX_ACCESS_DATA AuxData;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( ObjectCreated );
    
    if ( AccessMode == KernelMode ) {
        return;
    }

    AuxData = (PAUX_ACCESS_DATA)AccessState->AuxData;

    Token = EffectiveToken( &AccessState->SubjectSecurityContext );

    if (ARGUMENT_PRESENT(Token->AuditData)) {

        MappedGrantMask = Token->AuditData->GrantMask;

        RtlMapGenericMask(
            &MappedGrantMask,
            &AuxData->GenericMapping
            );

        MappedDenyMask = Token->AuditData->DenyMask;

        RtlMapGenericMask(
            &MappedDenyMask,
            &AuxData->GenericMapping
            );
    }

    if (SecurityDescriptor != NULL) {

        RequestedAccess = AccessState->RemainingDesiredAccess |
                          AccessState->PreviouslyGrantedAccess;

        if ( SepAdtAuditThisEventWithContext( AuditCategoryObjectAccess, AccessGranted, !AccessGranted, &AccessState->SubjectSecurityContext )) {

            if ( RequestedAccess & (AccessGranted ? MappedGrantMask : MappedDenyMask)) {

                GenerateAudit = TRUE;

            } else {

                SepExamineSacl(
                    RtlpSaclAddrSecurityDescriptor( (PISECURITY_DESCRIPTOR)SecurityDescriptor ),
                    Token,
                    RequestedAccess,
                    AccessGranted,
                    &GenerateAudit,
                    &GenerateAlarm
                    );
            }

            //
            // Only generate an audit on close of we're auditing from SACL
            // settings.
            //

            if (GenerateAudit) {
                *GenerateOnClose = TRUE;
            }
        }
    }

    //
    // If we don't generate an audit via the SACL, see if we need to generate
    // one for privilege use.
    //
    // Note that we only audit privileges successfully used to open objects,
    // so we don't care about a failed privilege use here.  Therefore, only
    // do this test of access has been granted.
    //

    if (!GenerateAudit && (AccessGranted == TRUE)) {

        if ( SepAdtAuditThisEventWithContext( AuditCategoryPrivilegeUse, AccessGranted, FALSE, &AccessState->SubjectSecurityContext )) {

            if ((AuxData->PrivilegesUsed != NULL) &&
                (AuxData->PrivilegesUsed->PrivilegeCount > 0) ) {

                //
                // Make sure these are actually privileges that we want to audit
                //

                if (SepFilterPrivilegeAudits( 0, AuxData->PrivilegesUsed )) {

                    GenerateAudit = TRUE;

                    //
                    // When we finally try to generate this audit, this flag
                    // will tell us that we need to audit the fact that we
                    // used a privilege, as opposed to audit due to the SACL.
                    //

                    AccessState->AuditPrivileges = TRUE;
                }
            }
        }
    }

    //
    // Set up either to generate an audit (if the access check has failed), or save
    // the stuff that we're going to audit later into the AccessState structure.
    //

    if (GenerateAudit || GenerateAlarm) {

        AccessState->GenerateAudit = TRUE;

        //
        // Figure out what we've been passed, and obtain as much
        // missing information as possible.
        //

        if ( !ARGUMENT_PRESENT( AbsoluteObjectName )) {

            if ( ARGUMENT_PRESENT( Object )) {

                ObjectNameInfo = SepQueryNameString( Object  );

                if ( ObjectNameInfo != NULL ) {

                    ObjectName = &ObjectNameInfo->Name;
                }
            }

        } else {

            ObjectName = AbsoluteObjectName;
        }

        if ( !ARGUMENT_PRESENT( ObjectTypeName )) {

            if ( ARGUMENT_PRESENT( Object )) {

                ObjectTypeNameInfo = SepQueryTypeString( Object );

                if ( ObjectTypeNameInfo != NULL ) {

                    LocalObjectTypeName = ObjectTypeNameInfo;
                }
            }

        } else {

            LocalObjectTypeName = ObjectTypeName;
        }

        //
        // If the access attempt failed, do the audit here.  If it succeeded,
        // we'll do the audit later, when the handle is allocated.
        //
        //

        if (!AccessGranted) {

            AuditPerformed = SepAdtOpenObjectAuditAlarm ( (PUNICODE_STRING)&SeSubsystemName,
                                                          NULL,
                                                          LocalObjectTypeName,
                                                          ObjectName,
                                                          AccessState->SubjectSecurityContext.ClientToken,
                                                          AccessState->SubjectSecurityContext.PrimaryToken,
                                                          AccessState->OriginalDesiredAccess,
                                                          AccessState->PreviouslyGrantedAccess,
                                                          &AccessState->OperationID,
                                                          AuxData->PrivilegesUsed,
                                                          FALSE,
                                                          AccessState->SubjectSecurityContext.ProcessAuditId,
                                                          AuditCategoryObjectAccess,
                                                          NULL,
                                                          0,
                                                          NULL );
        } else {

            //
            // Generate the delete audit first
            //

            SepAdtOpenObjectForDeleteAuditAlarm ( (PUNICODE_STRING)&SeSubsystemName,
                                                  NULL,
                                                  LocalObjectTypeName,
                                                  ObjectName,
                                                  AccessState->SubjectSecurityContext.ClientToken,
                                                  AccessState->SubjectSecurityContext.PrimaryToken,
                                                  AccessState->OriginalDesiredAccess,
                                                  AccessState->PreviouslyGrantedAccess,
                                                  &AccessState->OperationID,
                                                  AuxData->PrivilegesUsed,
                                                  TRUE,
                                                  AccessState->SubjectSecurityContext.ProcessAuditId );

            //
            // Copy all the stuff we're going to need into the
            // AccessState and return.
            //

            if ( ObjectName != NULL ) {

                 if ( AccessState->ObjectName.Buffer != NULL ) {

                     ExFreePool( AccessState->ObjectName.Buffer );
                     AccessState->ObjectName.Length = 0;
                     AccessState->ObjectName.MaximumLength = 0;
                 }

                AccessState->ObjectName.Buffer = ExAllocatePool( PagedPool,ObjectName->MaximumLength );
                if (AccessState->ObjectName.Buffer != NULL) {

                    AccessState->ObjectName.MaximumLength = ObjectName->MaximumLength;
                    RtlCopyUnicodeString( &AccessState->ObjectName, ObjectName );
                }
            }

            if ( LocalObjectTypeName != NULL ) {

                 if ( AccessState->ObjectTypeName.Buffer != NULL ) {

                     ExFreePool( AccessState->ObjectTypeName.Buffer );
                     AccessState->ObjectTypeName.Length = 0;
                     AccessState->ObjectTypeName.MaximumLength = 0;
                 }

                AccessState->ObjectTypeName.Buffer = ExAllocatePool( PagedPool, LocalObjectTypeName->MaximumLength );
                if (AccessState->ObjectTypeName.Buffer != NULL) {

                    AccessState->ObjectTypeName.MaximumLength = LocalObjectTypeName->MaximumLength;
                    RtlCopyUnicodeString( &AccessState->ObjectTypeName, LocalObjectTypeName );
                }
            }
        }

        if ( ObjectNameInfo != NULL ) {

            ExFreePool( ObjectNameInfo );
        }

        if ( ObjectTypeNameInfo != NULL ) {

            ExFreePool( ObjectTypeNameInfo );
        }
    }

    return;
}


VOID
SeObjectReferenceAuditAlarm(
    __in_opt PLUID OperationID,
    __in PVOID Object,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in PSECURITY_SUBJECT_CONTEXT SubjectSecurityContext,
    __in ACCESS_MASK DesiredAccess,
    __in_opt PPRIVILEGE_SET Privileges,
    __in BOOLEAN AccessGranted,
    __in KPROCESSOR_MODE AccessMode
    )

/*++
Routine Description:

    description-of-function.

Arguments:

    argument-name - Supplies | Returns description of argument.
    .
    .

Return Value:

    return-value - Description of conditions needed to return value. - or -
    None.

--*/

{
    BOOLEAN GenerateAudit = FALSE;
    BOOLEAN GenerateAlarm = FALSE;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( OperationID );
    UNREFERENCED_PARAMETER( Privileges );
    
    if (AccessMode == KernelMode) {
        return;
    }

    if ( SecurityDescriptor != NULL ) {

        if ( SepAdtAuditThisEventWithContext( AuditCategoryDetailedTracking, AccessGranted, FALSE, SubjectSecurityContext )) {

            SepExamineSacl(
                RtlpSaclAddrSecurityDescriptor( (PISECURITY_DESCRIPTOR)SecurityDescriptor ),
                EffectiveToken( SubjectSecurityContext ),
                DesiredAccess,
                AccessGranted,
                &GenerateAudit,
                &GenerateAlarm
                );

            if ( GenerateAudit || GenerateAlarm ) {

                SepAdtObjectReferenceAuditAlarm(
                    Object,
                    SubjectSecurityContext,
                    DesiredAccess,
                    AccessGranted
                    );
            }
        }
    }

    return;

}



VOID
SeAuditHandleCreation(
    __in PACCESS_STATE AccessState,
    __in HANDLE Handle
    )

/*++

Routine Description:

    This function audits the creation of a handle.

    It will examine the AuditHandleCreation field in the passed AccessState,
    which will indicate whether auditing was performed when the object
    was found or created.

    This routine is necessary because object name decoding and handle
    allocation occur in widely separate places, preventing us from
    auditing everything at once.

Arguments:

    AccessState - Supplies a pointer to the AccessState structure
        representing this access attempt.

    Handle - The newly allocated handle value.

Return Value:

    None.

--*/

{
    BOOLEAN AuditPerformed = FALSE;
    PAUX_ACCESS_DATA AuxData;
    PAGED_CODE();

    AuxData = (PAUX_ACCESS_DATA)AccessState->AuxData;

#if DBG
    if ( AuxData->PrivilegesUsed )
    {
        ASSERT( IsValidPrivilegeCount(AuxData->PrivilegesUsed->PrivilegeCount) );
    }
#endif
    

    if ( AccessState->GenerateAudit ) {

        if ( AccessState->AuditPrivileges ) {

            //
            // ignore the result of the call below so that we
            // do not incorrectly set the value of AuditPerformed
            // which is later assigned to AccessState->GenerateOnClose
            //

            (VOID) SepAdtPrivilegeObjectAuditAlarm (
                       (PUNICODE_STRING)&SeSubsystemName,
                       Handle,
                       (PTOKEN)AccessState->SubjectSecurityContext.ClientToken,
                       (PTOKEN)AccessState->SubjectSecurityContext.PrimaryToken,
                       AccessState->SubjectSecurityContext.ProcessAuditId,
                       AccessState->PreviouslyGrantedAccess,
                       AuxData->PrivilegesUsed,
                       TRUE
                       );
        } else {

            AuditPerformed = SepAdtOpenObjectAuditAlarm (
                                 (PUNICODE_STRING)&SeSubsystemName,
                                 &Handle,
                                 &AccessState->ObjectTypeName,
                                 &AccessState->ObjectName,
                                 AccessState->SubjectSecurityContext.ClientToken,
                                 AccessState->SubjectSecurityContext.PrimaryToken,
                                 AccessState->OriginalDesiredAccess,
                                 AccessState->PreviouslyGrantedAccess,
                                 &AccessState->OperationID,
                                 AuxData->PrivilegesUsed,
                                 TRUE,
                                 PsGetCurrentProcessId(),
                                 AuditCategoryObjectAccess,
                                 NULL,
                                 0,
                                 NULL );
        }
    }

    //
    // If we generated an 'open' audit, make sure we generate a close
    //

    AccessState->GenerateOnClose = AuditPerformed;

    return;
}


VOID
SeCloseObjectAuditAlarm(
    __in PVOID Object,
    __in HANDLE Handle,
    __in BOOLEAN GenerateOnClose
    )

/*++

Routine Description:

    This routine is used to generate audit and alarm messages when a handle
    to an object is deleted.

    This routine may result in several messages being generated and sent to
    Port objects.  This may result in a significant latency before
    returning.  Design of routines that must call this routine must take
    this potential latency into account.  This may have an impact on the
    approach taken for data structure mutex locking, for example.

Arguments:

    Object - Address of the object being accessed.  This value will not be
        used as a pointer (referenced).  It is necessary only to enter into
        log messages.

    Handle - Supplies the handle value assigned to the open.

    GenerateOnClose - Is a boolean value returned from a corresponding
        SeOpenObjectAuditAlarm() call when the object handle was created.

Return Value:

    None.

--*/

{
    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;
    PSID UserSid;
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( Object );
    
    if (GenerateOnClose) {

        SeCaptureSubjectContext ( &SubjectSecurityContext );

        UserSid = SepTokenUserSid( EffectiveToken (&SubjectSecurityContext));


        SepAdtCloseObjectAuditAlarm( (PUNICODE_STRING) &SeSubsystemName,
                                     Handle,
                                     UserSid );

        SeReleaseSubjectContext ( &SubjectSecurityContext );
    }

    return;
}


VOID
SeDeleteObjectAuditAlarm(
    __in PVOID Object,
    __in HANDLE Handle
    )

/*++

Routine Description:

    This routine is used to generate audit and alarm messages when an object
    is marked for deletion.

    This routine may result in several messages being generated and sent to
    Port objects.  This may result in a significant latency before
    returning.  Design of routines that must call this routine must take
    this potential latency into account.  This may have an impact on the
    approach taken for data structure mutex locking, for example.

Arguments:

    Object - Address of the object being accessed.  This value will not be
        used as a pointer (referenced).  It is necessary only to enter into
        log messages.

    Handle - Supplies the handle value assigned to the open.

Return Value:

    None.

--*/

{
    SECURITY_SUBJECT_CONTEXT SubjectSecurityContext;
    PSID UserSid;
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER( Object );
    
    SeCaptureSubjectContext ( &SubjectSecurityContext );

    UserSid = SepTokenUserSid( EffectiveToken (&SubjectSecurityContext));



    SepAdtDeleteObjectAuditAlarm (
        (PUNICODE_STRING)&SeSubsystemName,
        (PVOID)Handle,
        UserSid
        );

    SeReleaseSubjectContext ( &SubjectSecurityContext );

    return;
}


VOID
SepExamineSacl(
    IN PACL Sacl,
    IN PACCESS_TOKEN Token,
    IN ACCESS_MASK DesiredAccess,
    IN BOOLEAN AccessGranted,
    OUT PBOOLEAN GenerateAudit,
    OUT PBOOLEAN GenerateAlarm
    )

/*++

Routine Description:

    This routine will examine the passed Sacl and determine what
    if any action is required based its contents.

    Note that this routine is not aware of any system state, ie,
    whether or not auditing is currently enabled for either the
    system or this particular object type.

Arguments:

    Sacl - Supplies a pointer to the Sacl to be examined.

    Token - Supplies the effective token of the caller

    AccessGranted - Supplies whether or not the access attempt
        was successful.

    GenerateAudit - Returns a boolean indicating whether or not
        we should generate an audit.

    GenerateAlarm - Returns a boolean indicating whether or not
        we should generate an alarm.

Return Value:

    STATUS_SUCCESS - The operation completed successfully.

--*/

{

    ULONG i;
    PVOID Ace;
    ULONG AceCount;
    ACCESS_MASK AccessMask;
    UCHAR AceFlags;
    UCHAR MaximumAllowed;
    PSID AceSid;
    ULONG WorldSidLength = 0;

    PAGED_CODE();

    *GenerateAudit = FALSE;
    *GenerateAlarm = FALSE;


    //
    // If the Sacl is null or empty, do nothing and return
    //

    if (Sacl == NULL) {

        return;
    }

    AceCount = Sacl->AceCount;

    if (AceCount == 0) {
        return;
    }


    //
    // Generate an audit if the user asked for maximum allowed and
    // we find any allowed or deny ace with a sid that matches one
    // of the sids in the user's token.
    //

    MaximumAllowed = 0;
    if (DesiredAccess & MAXIMUM_ALLOWED) {

        if (AccessGranted) {
            MaximumAllowed = SUCCESSFUL_ACCESS_ACE_FLAG;
        } else {
            MaximumAllowed = FAILED_ACCESS_ACE_FLAG;
        }
    }


    //
    // Test whether we are dealing with the anonymous user.
    // To speed things up, we use WorldSidLength for two purposes:
    // - to indicate that the user is anonymous
    // - to hold the length of the world sid
    //

    if (*(PUSHORT)((PTOKEN)Token)->UserAndGroups->Sid == *(PUSHORT)SeAnonymousLogonSid &&
        RtlEqualMemory(
            ((PTOKEN)Token)->UserAndGroups->Sid,
            SeAnonymousLogonSid,
            SeLengthSid(SeAnonymousLogonSid))) {

        WorldSidLength = SeLengthSid(SeWorldSid);
    }


    //
    // Iterate through the ACEs on the Sacl until either we reach
    // the end or discover that we have to take all possible actions,
    // in which case it doesn't pay to look any further
    //

    for ( i = 0, Ace = FirstAce( Sacl ) ;
          (i < AceCount) && !(*GenerateAudit && *GenerateAlarm);
          i++, Ace = NextAce( Ace ) ) {

        if ( !(((PACE_HEADER)Ace)->AceFlags & INHERIT_ONLY_ACE)) {

            if ( (((PACE_HEADER)Ace)->AceType == SYSTEM_AUDIT_ACE_TYPE) ) {

                AceSid = &((PSYSTEM_AUDIT_ACE)Ace)->SidStart;

                if ( SepSidInToken( (PACCESS_TOKEN)Token, NULL, AceSid, FALSE ) ||
                     (WorldSidLength &&
                     *(PUSHORT)SeWorldSid == *(PUSHORT)AceSid &&
                     RtlEqualMemory(SeWorldSid, AceSid, WorldSidLength)) ) {

                    AccessMask = ((PSYSTEM_AUDIT_ACE)Ace)->Mask;
                    AceFlags   = ((PACE_HEADER)Ace)->AceFlags;

                    if ( AccessMask & DesiredAccess ) {

                        if (((AceFlags & SUCCESSFUL_ACCESS_ACE_FLAG) && AccessGranted) ||
                              ((AceFlags & FAILED_ACCESS_ACE_FLAG) && !AccessGranted)) {

                            *GenerateAudit = TRUE;
                        }

                    } else if ( MaximumAllowed & AceFlags ) {
                            *GenerateAudit = TRUE;
                    }
                }

            }

        }
    }

    return;
}


VOID
SepAuditTypeList (
    IN PIOBJECT_TYPE_LIST ObjectTypeList,
    IN ULONG ObjectTypeListLength,
    IN PNTSTATUS AccessStatus,
    IN ULONG StartIndex,
    OUT PBOOLEAN GenerateSuccessAudit,
    OUT PBOOLEAN GenerateFailureAudit
    )
/*++

Routine Description:

    This routine determines if any children of the object represented by
    StartIndex have a different degree of success than the StartIndex element.

Arguments:

    ObjectTypeList - The object type list to update.

    ObjectTypeListLength - Number of elements in ObjectTypeList

    AccessStatus - Specifies STATUS_SUCCESS or other error code to be
        propagated back to the caller

    StartIndex - Index to the target element to update.

    GenerateSuccessAudit - Returns a boolean indicating whether or not
        we should generate a success audit.

    GenerateFailureAudit - Returns a boolean indicating whether or not
        we should generate a failure audit.

Return Value:

    None.

--*/

{
    ULONG Index;
    BOOLEAN WasSuccess;

    PAGED_CODE();

    //
    // Determine if the target was successful.
    //

    WasSuccess = NT_SUCCESS( AccessStatus[StartIndex] );

    //
    // Loop handling all children of the target.
    //

    for ( Index=StartIndex+1; Index < ObjectTypeListLength; Index++ ) {

        //
        // By definition, the children of an object are all those entries
        // immediately following the target.  The list of children (or
        // grandchildren) stops as soon as we reach an entry the has the
        // same level as the target (a sibling) or lower than the target
        // (an uncle).
        //

        if ( ObjectTypeList[Index].Level <= ObjectTypeList[StartIndex].Level ) {
            break;
        }

        //
        // If the child has different access than the target,
        //  mark the child.
        //

        if ( WasSuccess && !NT_SUCCESS( AccessStatus[Index]) ) {

            *GenerateFailureAudit = TRUE;
            ObjectTypeList[Index].Flags |= OBJECT_FAILURE_AUDIT;

        } else if ( !WasSuccess && NT_SUCCESS( AccessStatus[Index]) ) {

            *GenerateSuccessAudit = TRUE;
            ObjectTypeList[Index].Flags |= OBJECT_SUCCESS_AUDIT;
        }
    }
}


VOID
SepSetAuditInfoForObjectType(
    IN  UCHAR AceFlags,
    IN  ACCESS_MASK AccessMask,
    IN  ACCESS_MASK DesiredAccess,
    IN  PIOBJECT_TYPE_LIST ObjectTypeList,
    IN  ULONG ObjectTypeListLength,
    IN  BOOLEAN ReturnResultList,
    IN  ULONG ObjectTypeIndex,
    IN  PNTSTATUS AccessStatus,
    IN  PACCESS_MASK GrantedAccess,
    OUT PBOOLEAN GenerateSuccessAudit,
    OUT PBOOLEAN GenerateFailureAudit
    )
/*++

Routine Description:

    Determine if success/failure audit needs to be generated for
    object at ObjectTypeIndex in ObjectTypeList.

    This helper function is called only by SepExamineSaclEx.

Arguments:

    please refer to arg help for function SepExamineSaclEx

Return Value:

    None.

--*/
{
    UCHAR MaximumAllowed = 0;

    PAGED_CODE();

    if (DesiredAccess & MAXIMUM_ALLOWED) {

        if (NT_SUCCESS(AccessStatus[ObjectTypeIndex])) {
            MaximumAllowed = SUCCESSFUL_ACCESS_ACE_FLAG;
        } else {
            MaximumAllowed = FAILED_ACCESS_ACE_FLAG;
        }
    }

    if ( AccessMask & (DesiredAccess|GrantedAccess[ObjectTypeIndex]) ) {

        if ( ( AceFlags & SUCCESSFUL_ACCESS_ACE_FLAG ) &&
             NT_SUCCESS(AccessStatus[ObjectTypeIndex]) ) {

                *GenerateSuccessAudit = TRUE;

                if ( ObjectTypeListLength != 0 ) {

                    ObjectTypeList[ObjectTypeIndex].Flags |= OBJECT_SUCCESS_AUDIT;

                    if ( ReturnResultList ) {

                        SepAuditTypeList( ObjectTypeList,
                                          ObjectTypeListLength,
                                          AccessStatus,
                                          ObjectTypeIndex,
                                          GenerateSuccessAudit,
                                          GenerateFailureAudit );
                    }
                }
        } else if ( ( AceFlags & FAILED_ACCESS_ACE_FLAG ) &&
                    !NT_SUCCESS(AccessStatus[ObjectTypeIndex]) ) {

                *GenerateFailureAudit = TRUE;

                if ( ObjectTypeListLength != 0 ) {

                    ObjectTypeList[ObjectTypeIndex].Flags |= OBJECT_FAILURE_AUDIT;

                    if ( ReturnResultList ) {

                        SepAuditTypeList( ObjectTypeList,
                                          ObjectTypeListLength,
                                          AccessStatus,
                                          ObjectTypeIndex,
                                          GenerateSuccessAudit,
                                          GenerateFailureAudit );
                    }
                }
        }

    } else if ( MaximumAllowed & AceFlags ) {
        if (MaximumAllowed == FAILED_ACCESS_ACE_FLAG) {
            *GenerateFailureAudit = TRUE;
            if ( ObjectTypeListLength != 0 ) {
                ObjectTypeList[ObjectTypeIndex].Flags |= OBJECT_FAILURE_AUDIT;
            }
        } else {
            *GenerateSuccessAudit = TRUE;
            if ( ObjectTypeListLength != 0 ) {
                ObjectTypeList[ObjectTypeIndex].Flags |= OBJECT_SUCCESS_AUDIT;
            }
        }
    }
}


VOID
SepExamineSaclEx(
    IN PACL Sacl,
    IN PACCESS_TOKEN Token,
    IN ACCESS_MASK DesiredAccess,
    IN PIOBJECT_TYPE_LIST ObjectTypeList OPTIONAL,
    IN ULONG ObjectTypeListLength,
    IN BOOLEAN ReturnResultList,
    IN PNTSTATUS AccessStatus,
    IN PACCESS_MASK GrantedAccess,
    IN PSID PrincipalSelfSid,
    OUT PBOOLEAN GenerateSuccessAudit,
    OUT PBOOLEAN GenerateFailureAudit
    )

/*++

Routine Description:

    This routine will examine the passed Sacl and determine what
    if any action is required based its contents.

    Note that this routine is not aware of any system state, ie,
    whether or not auditing is currently enabled for either the
    system or this particular object type.

Arguments:

    Sacl - Supplies a pointer to the Sacl to be examined.

    Token - Supplies the effective token of the caller

    DesiredAccess - Access that the caller wanted to the object

    ObjectTypeList - Supplies a list of GUIDs representing the object (and
        sub-objects) being accessed.

    ObjectTypeListLength - Specifies the number of elements in the ObjectTypeList.

    ReturnResultList - If true, AccessStatus and GrantedAccess is actually
        an array of entries ObjectTypeListLength elements long.

    AccessStatus - Specifies STATUS_SUCCESS or other error code to be
        propagated back to the caller

    PrincipalSelfSid - If the security descriptor is associated with an object 
        that represents a principal (for example, a user object), 
        the PrincipalSelfSid parameter should be the SID of the object. 
        When evaluating access, this SID logically replaces the SID in any ACE 
        containing the well-known PRINCIPAL_SELF SID (S-1-5-10). 

    GrantedAccess - Specifies the access granted to the caller.

    GenerateSuccessAudit - Returns a boolean indicating whether or not
        we should generate a success audit.

    GenerateFailureAudit - Returns a boolean indicating whether or not
        we should generate a failure audit.

Return Value:

    STATUS_SUCCESS - The operation completed successfully.

--*/

{

    ULONG i, j;
    PVOID Ace;
    ULONG AceCount;
    ACCESS_MASK AccessMask=0;
    UCHAR AceFlags;
    UCHAR MaximumAllowed;
    PSID AceSid;
    ULONG WorldSidLength = 0;
    ULONG Index;
    ULONG SuccessIndex;
#define INVALID_OBJECT_TYPE_LIST_INDEX 0xFFFFFFFF

    PAGED_CODE();

    *GenerateSuccessAudit = FALSE;
    *GenerateFailureAudit = FALSE;


    //
    // If the Sacl is null, do nothing and return
    //

    if (Sacl == NULL) {
        return;
    }

    AceCount = Sacl->AceCount;

    if (AceCount == 0) {
        return;
    }


    //
    // Generate an audit if the user asked for maximum allowed and
    // we find any allowed or deny ace with a sid that matches one
    // of the sids in the user's token.
    //

    MaximumAllowed = 0;
    if (DesiredAccess & MAXIMUM_ALLOWED) {

        if (NT_SUCCESS(*AccessStatus)) {
            MaximumAllowed = SUCCESSFUL_ACCESS_ACE_FLAG;
        } else {
            MaximumAllowed = FAILED_ACCESS_ACE_FLAG;
        }
    }


    //
    // Test whether we are dealing with the anonymous user.
    // To speed things up, we use WorldSidLength for two purposes:
    // - to indicate that the user is anonymous
    // - to hold the length of the world sid
    //

    if (*(PUSHORT)((PTOKEN)Token)->UserAndGroups->Sid == *(PUSHORT)SeAnonymousLogonSid &&
        RtlEqualMemory(
            ((PTOKEN)Token)->UserAndGroups->Sid,
            SeAnonymousLogonSid,
            SeLengthSid(SeAnonymousLogonSid))) {

        WorldSidLength = SeLengthSid(SeWorldSid);
    }


    //
    // Iterate through the ACEs on the Sacl until either we reach
    // the end or discover that we have to take all possible actions,
    // in which case it doesn't pay to look any further
    //

    for ( i = 0, Ace = FirstAce( Sacl ) ;
          (i < AceCount) && !((*GenerateSuccessAudit || *GenerateFailureAudit) && ObjectTypeListLength <= 1 );
          i++, Ace = NextAce( Ace ) ) {

        AceFlags = ((PACE_HEADER)Ace)->AceFlags;
        
        if ( AceFlags & INHERIT_ONLY_ACE ) {

            continue;
        }


        Index = INVALID_OBJECT_TYPE_LIST_INDEX;

        if ( (((PACE_HEADER)Ace)->AceType == SYSTEM_AUDIT_ACE_TYPE) ) {

            AceSid = &((PSYSTEM_AUDIT_ACE)Ace)->SidStart;

            if ( SepSidInToken( Token, PrincipalSelfSid, AceSid, (BOOLEAN) ((AceFlags & FAILED_ACCESS_ACE_FLAG) != 0) ) ||
                 (WorldSidLength &&
                 *(PUSHORT)SeWorldSid == *(PUSHORT)AceSid &&
                 RtlEqualMemory(SeWorldSid, AceSid, WorldSidLength)) ) {

                AccessMask = ((PSYSTEM_AUDIT_ACE)Ace)->Mask;

                if (ObjectTypeListLength == 0) {

                    if ( NT_SUCCESS(AccessStatus[0]) ) {

                        if ( ( AceFlags & SUCCESSFUL_ACCESS_ACE_FLAG ) &&
                             (( AccessMask & GrantedAccess[0] ) || ( MaximumAllowed == SUCCESSFUL_ACCESS_ACE_FLAG )) ) {

                            *GenerateSuccessAudit = TRUE;

                        } 

                    } else {

                        if ( ( AceFlags & FAILED_ACCESS_ACE_FLAG ) &&
                             (( AccessMask & DesiredAccess ) || ( MaximumAllowed == FAILED_ACCESS_ACE_FLAG )) ) {

                            *GenerateFailureAudit = TRUE;
                        }
                    }
                } else {
                    for (j=0; j < ObjectTypeListLength; j++)
                    {
                        SepSetAuditInfoForObjectType(AceFlags,
                                                     AccessMask,
                                                     DesiredAccess,
                                                     ObjectTypeList,
                                                     ObjectTypeListLength,
                                                     ReturnResultList,
                                                     j,
                                                     AccessStatus,
                                                     GrantedAccess,
                                                     GenerateSuccessAudit,
                                                     GenerateFailureAudit
                                                     );
                    }
                    Index = INVALID_OBJECT_TYPE_LIST_INDEX;
                }
            }

            //
            // Handle an object specific audit ACE
            //
        } else if ( (((PACE_HEADER)Ace)->AceType == SYSTEM_AUDIT_OBJECT_ACE_TYPE) ) {
            GUID *ObjectTypeInAce;

            //
            // If no object type is in the ACE,
            //  treat this as a normal audit ACE.
            //

            AccessMask = ((PSYSTEM_AUDIT_OBJECT_ACE)Ace)->Mask;
            ObjectTypeInAce = RtlObjectAceObjectType(Ace);
            AceSid = RtlObjectAceSid(Ace);

            if ( ObjectTypeInAce == NULL ) {

                if ( SepSidInToken( Token, PrincipalSelfSid, AceSid, (BOOLEAN)((AceFlags & FAILED_ACCESS_ACE_FLAG) != 0) ) ||
                     (WorldSidLength &&
                     *(PUSHORT)SeWorldSid == *(PUSHORT)AceSid &&
                     RtlEqualMemory(SeWorldSid, AceSid, WorldSidLength)) ) {

                    for (j=0; j < ObjectTypeListLength; j++)
                    {
                        SepSetAuditInfoForObjectType(AceFlags,
                                                     AccessMask,
                                                     DesiredAccess,
                                                     ObjectTypeList,
                                                     ObjectTypeListLength,
                                                     ReturnResultList,
                                                     j,
                                                     AccessStatus,
                                                     GrantedAccess,
                                                     GenerateSuccessAudit,
                                                     GenerateFailureAudit
                                                     );
                    }
                    Index = INVALID_OBJECT_TYPE_LIST_INDEX;
                }

                //
                // If an object type is in the ACE,
                //   Find it in the LocalTypeList before using the ACE.
                //
            } else {

                if ( SepSidInToken( Token, PrincipalSelfSid, AceSid, (BOOLEAN)((AceFlags & FAILED_ACCESS_ACE_FLAG) != 0) ) ||
                     (WorldSidLength &&
                     *(PUSHORT)SeWorldSid == *(PUSHORT)AceSid &&
                     RtlEqualMemory(SeWorldSid, AceSid, WorldSidLength)) ) {

                    if ( !SepObjectInTypeList( ObjectTypeInAce,
                                               ObjectTypeList,
                                               ObjectTypeListLength,
                                               &Index ) ) {

                        Index = INVALID_OBJECT_TYPE_LIST_INDEX;
                    }
                }
            }

        }

        //
        // If the ACE has a matched SID and a matched GUID,
        //  handle it.
        //

        if ( Index != INVALID_OBJECT_TYPE_LIST_INDEX ) {

            //
            // ASSERT: we have an ACE to be audited.
            //
            // Index is an index into ObjectTypeList of the entry to mark
            //  as the GUID needs auditing.
            //
            // SuccessIndex is an index into AccessStatus to determine if
            //  a success or failure audit is to be generated
            //

            SepSetAuditInfoForObjectType(AceFlags,
                                         AccessMask,
                                         DesiredAccess,
                                         ObjectTypeList,
                                         ObjectTypeListLength,
                                         ReturnResultList,
                                         Index,
                                         AccessStatus,
                                         GrantedAccess,
                                         GenerateSuccessAudit,
                                         GenerateFailureAudit
                                         );
        }

    }

    return;
}



/******************************************************************************
*                                                                             *
*    The following list of privileges is checked at high frequency            *
*    during normal operation, and tend to clog up the audit log when          *
*    privilege auditing is enabled.  The use of these privileges will         *
*    not be audited when they are checked singly or in combination with       *
*    each other.                                                              *
*                                                                             *
*    When adding new privileges, be careful to preserve the NULL              *
*    privilege pointer marking the end of the array.                          *
*                                                                             *
*    Be sure to update the corresponding array in LSA when adding new         *
*    privileges to this list (LsaFilterPrivileges).                           *
*                                                                             *
******************************************************************************/

#ifdef ALLOC_DATA_PRAGMA
#pragma data_seg("PAGEDATA")
#pragma const_seg("PAGECONST")
#endif          

const PLUID SepFilterPrivilegesLong[] =
    {
        &SeChangeNotifyPrivilege,
        &SeAuditPrivilege,
        &SeCreateTokenPrivilege,
        &SeAssignPrimaryTokenPrivilege,
        &SeBackupPrivilege,
        &SeRestorePrivilege,
        &SeDebugPrivilege,
        NULL
    };

/******************************************************************************
*                                                                             *
*  The following list of privileges is the same as the above list, except     *
*  is missing backup and restore privileges.  This allows for auditing        *
*  the use of those privileges at the time they are used.                     *
*                                                                             *
*  The use of this list or the one above is determined by settings in         *
*  the registry.                                                              *
*                                                                             *
******************************************************************************/

const PLUID SepFilterPrivilegesShort[] =
    {
        &SeChangeNotifyPrivilege,
        &SeAuditPrivilege,
        &SeCreateTokenPrivilege,
        &SeAssignPrimaryTokenPrivilege,
        &SeDebugPrivilege,
        NULL
    };

//
// This list contains the privileges which are not audited for the service 
// accounts.  There should be no duplicates between this list and the 
// other filter lists.
//

const PLUID SepServicesFilterPrivileges[] =
    {      
        &SeSystemtimePrivilege,
        NULL
    };

PLUID const * SepFilterPrivileges = SepFilterPrivilegesShort;

BOOLEAN
SepInitializePrivilegeFilter(
    BOOLEAN Verbose
    )
/*++

Routine Description:

    Initializes SepFilterPrivileges for either normal or verbose auditing.

Arguments:

    Verbose - Whether we want to filter by the short or long privileges
    list.  Verbose == TRUE means use the short list.

Return Value:

    TRUE for success, FALSE for failure

--*/
{
    if (Verbose) {
        SepFilterPrivileges = SepFilterPrivilegesShort;
    } else {
        SepFilterPrivileges = SepFilterPrivilegesLong;
    }

    return( TRUE );
}


BOOLEAN
SepFilterPrivilegeAudits(
    IN ULONG Flags,
    IN PPRIVILEGE_SET PrivilegeSet
    )

/*++

Routine Description:

    This routine will filter out a list of privileges as listed in the
    SepFilterPrivileges array.

Arguments:

    Privileges - The privilege set to be audited

Return Value:

    FALSE means that this use of privilege is not to be audited.
    TRUE means that the audit should continue normally.

--*/

{
    PLUID const *Privilege;
    ULONG Match = 0;
    ULONG i;

    PAGED_CODE();

    if ( !ARGUMENT_PRESENT(PrivilegeSet) ||
        (PrivilegeSet->PrivilegeCount == 0) ) {
        return( FALSE );
    }

    for (i=0; i<PrivilegeSet->PrivilegeCount; i++) {

        Privilege = SepFilterPrivileges;

        do {

            if ( RtlEqualLuid( &PrivilegeSet->Privilege[i].Luid, *Privilege )) {

                Match++;
                break;
            }

        } while ( *++Privilege != NULL  );
    }

    if (Flags & SEP_SERVICES_FILTER) {
        
        //
        // Additionally, check the privileges against those that are not 
        // audited for service accounts.
        //

        for (i=0; i<PrivilegeSet->PrivilegeCount; i++) {

            Privilege = SepServicesFilterPrivileges;

            do {

                if ( RtlEqualLuid( &PrivilegeSet->Privilege[i].Luid, *Privilege )) {

                    Match++;
                    break;
                }

            } while ( *++Privilege != NULL  );
        }
    }
    
    if ( Match == PrivilegeSet->PrivilegeCount ) {

        return( FALSE );

    } else {

        return( TRUE );
    }
}


BOOLEAN
SeAuditingFileOrGlobalEvents(
    __in BOOLEAN AccessGranted,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in PSECURITY_SUBJECT_CONTEXT SubjectSecurityContext
    )

/*++

Routine Description:

    This routine is to be called by a file system to quickly determine
    if we are auditing file open events.  This allows the file system
    to avoid the often considerable setup involved in generating an audit.

Arguments:

    AccessGranted - Supplies whether the access attempt was successful
        or a failure.

Return Value:

    Boolean - TRUE if events of type AccessGranted are being audited, FALSE
        otherwise.

--*/

{
    PISECURITY_DESCRIPTOR ISecurityDescriptor = (PISECURITY_DESCRIPTOR) SecurityDescriptor;

    PAGED_CODE();

    if ( ((PTOKEN)EffectiveToken( SubjectSecurityContext ))->AuditData != NULL) {
        return( TRUE );
    }

    if ( RtlpSaclAddrSecurityDescriptor( ISecurityDescriptor ) == NULL ) {

        return( FALSE );
    }

    return( SepAdtAuditThisEventWithContext(AuditCategoryObjectAccess, AccessGranted, !AccessGranted, SubjectSecurityContext) ||
            SepAdtAuditThisEventWithContext(AuditCategoryPrivilegeUse, AccessGranted, !AccessGranted, SubjectSecurityContext) );
}


BOOLEAN
SeAuditingFileEvents(
    __in BOOLEAN AccessGranted,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor
    )

/*++

Routine Description:

    This routine is to be called by a file system to quickly determine
    if we are auditing file open events.  This allows the file system
    to avoid the often considerable setup involved in generating an audit.

Arguments:

    AccessGranted - Supplies whether the access attempt was successful
        or a failure.

Return Value:

    Boolean - TRUE if events of type AccessGranted are being audited, FALSE
        otherwise.

--*/

{
    PAGED_CODE();

    UNREFERENCED_PARAMETER( SecurityDescriptor );

    return( SepAdtAuditThisEvent( AuditCategoryObjectAccess, &AccessGranted ) || 
            SepAdtAuditThisEvent( AuditCategoryPrivilegeUse, &AccessGranted ));
}


BOOLEAN
SeAuditingFileEventsWithContext(
    __in BOOLEAN AccessGranted,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in_opt PSECURITY_SUBJECT_CONTEXT SubjectSecurityContext
    )

/*++

Routine Description:

    This routine is to be called by a file system to quickly determine
    if we are auditing file open events.  This allows the file system
    to avoid the often considerable setup involved in generating an audit.

Arguments:

    AccessGranted - Supplies whether the access attempt was successful
        or a failure.

    SecurityDescriptor - SD to check sacl                                          
    
    SubjectSecurityContext - context to verify per user auditing from
    
Return Value:

    Boolean - TRUE if events of type AccessGranted are being audited, FALSE
        otherwise.

--*/

{
    PAGED_CODE();

    UNREFERENCED_PARAMETER( SecurityDescriptor );

    return( SepAdtAuditThisEventWithContext(AuditCategoryObjectAccess, AccessGranted, !AccessGranted, SubjectSecurityContext) ||
            SepAdtAuditThisEventWithContext(AuditCategoryPrivilegeUse, AccessGranted, !AccessGranted, SubjectSecurityContext) );
}



BOOLEAN                                  
SeAuditingHardLinkEvents(                                
    __in BOOLEAN AccessGranted,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor
    )

/*++

Routine Description:

    This routine is to be called by a file system to quickly determine
    if we are auditing hard link creation.

Arguments:

    AccessGranted - Supplies whether the access attempt was successful
        or a failure.
        
    SecurityDescriptor - SD of the linked file.
    
Return Value:

    Boolean - TRUE if events of type AccessGranted are being audited, FALSE
        otherwise.

--*/

{
    PISECURITY_DESCRIPTOR pSD;
    PACL                  Sacl;

    PAGED_CODE();
   
    pSD  = (PISECURITY_DESCRIPTOR) SecurityDescriptor;                    
    Sacl = RtlpSaclAddrSecurityDescriptor( pSD ); 
    
    //
    // Audit hard link creation if object access auditing is on and the original file
    // has a non empty SACL.
    //

    if ( (NULL != Sacl)        && 
         (0 != Sacl->AceCount) &&
         (SepAdtAuditThisEvent( AuditCategoryObjectAccess, &AccessGranted ))) {

        return TRUE;
    }
    
    return FALSE;
}


BOOLEAN                                  
SeAuditingHardLinkEventsWithContext(                                
    __in BOOLEAN AccessGranted,
    __in PSECURITY_DESCRIPTOR SecurityDescriptor,
    __in_opt PSECURITY_SUBJECT_CONTEXT SubjectSecurityContext
    )

/*++

Routine Description:

    This routine is to be called by a file system to quickly determine
    if we are auditing hard link creation.

Arguments:

    AccessGranted - Supplies whether the access attempt was successful
        or a failure.
        
    SecurityDescriptor - SD of the linked file.
    
Return Value:

    Boolean - TRUE if events of type AccessGranted are being audited, FALSE
        otherwise.

--*/

{
    PISECURITY_DESCRIPTOR pSD;
    PACL                  Sacl;

    PAGED_CODE();
   
    pSD  = (PISECURITY_DESCRIPTOR) SecurityDescriptor;                    
    Sacl = RtlpSaclAddrSecurityDescriptor( pSD ); 
   
    //
    // Audit hard link creation if object access auditing is on and the original file
    // has a non empty SACL.
    //

    if ( (NULL != Sacl) && 
         (0 != Sacl->AceCount) && 
         (SepAdtAuditThisEventWithContext( AuditCategoryObjectAccess, AccessGranted, !AccessGranted, SubjectSecurityContext ))) {
        
        return TRUE;
    }

    return FALSE;
}

#ifdef ALLOC_DATA_PRAGMA
#pragma data_seg()
#pragma const_seg()
#endif          

