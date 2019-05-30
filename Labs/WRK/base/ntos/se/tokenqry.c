/*++

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    Tokenqry.c

Abstract:

    This module implements the QUERY function for the executive
    token object.

--*/

#include "pch.h"

#pragma hdrstop


#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE,NtQueryInformationToken)
#pragma alloc_text(PAGE,SeQueryAuthenticationIdToken)
#pragma alloc_text(PAGE,SeQueryInformationToken)
#pragma alloc_text(PAGE,SeQuerySessionIdToken)
#endif


NTSTATUS
NtQueryInformationToken (
    __in HANDLE TokenHandle,
    __in TOKEN_INFORMATION_CLASS TokenInformationClass,
    __out_bcount_part_opt(TokenInformationLength,*ReturnLength) PVOID TokenInformation,
    __in ULONG TokenInformationLength,
    __out PULONG ReturnLength
    )

/*++


Routine Description:

    Retrieve information about a specified token.

Arguments:

    TokenHandle - Provides a handle to the token to operate on.

    TokenInformationClass - The token information class about which
        to retrieve information.

    TokenInformation - The buffer to receive the requested class of
        information.  The buffer must be aligned on at least a
        longword boundary.  The actual structures returned are
        dependent upon the information class requested, as defined in
        the TokenInformationClass parameter description.

        TokenInformation Format By Information Class:

           TokenUser => TOKEN_USER data structure.  TOKEN_QUERY
           access is needed to retrieve this information about a
           token.

           TokenGroups => TOKEN_GROUPS data structure.  TOKEN_QUERY
           access is needed to retrieve this information about a
           token.

           TokenPrivileges => TOKEN_PRIVILEGES data structure.
           TOKEN_QUERY access is needed to retrieve this information
           about a token.

           TokenOwner => TOKEN_OWNER data structure.  TOKEN_QUERY
           access is needed to retrieve this information about a
           token.

           TokenPrimaryGroup => TOKEN_PRIMARY_GROUP data structure.
           TOKEN_QUERY access is needed to retrieve this information
           about a token.

           TokenDefaultDacl => TOKEN_DEFAULT_DACL data structure.
           TOKEN_QUERY access is needed to retrieve this information
           about a token.

           TokenSource => TOKEN_SOURCE data structure.
           TOKEN_QUERY_SOURCE access is needed to retrieve this
           information about a token.

           TokenType => TOKEN_TYPE data structure.
           TOKEN_QUERY access is needed to retrieve this information
           about a token.

           TokenStatistics => TOKEN_STATISTICS data structure.
           TOKEN_QUERY access is needed to retrieve this
           information about a token.

           TokenGroups => TOKEN_GROUPS data structure.  TOKEN_QUERY
           access is needed to retrieve this information about a
           token.

           TokenSessionId => ULONG.  TOKEN_QUERY access is needed to 
           query the Session ID of the token.

           TokenAuditPolicy => TOKEN_AUDIT_POLICY structure.  TOKEN_QUERY
           access is needed to retrieve this information about a token.
           
           TokenOrigin => TOKEN_ORIGIN structure.
           
    TokenInformationLength - Indicates the length, in bytes, of the
        TokenInformation buffer.

    ReturnLength - This OUT parameter receives the actual length of
        the requested information.  If this value is larger than that
        provided by the TokenInformationLength parameter, then the
        buffer provided to receive the requested information is not
        large enough to hold that data and no data is returned.

        If the queried class is TokenDefaultDacl and there is no
        default Dacl established for the token, then the return
        length will be returned as zero, and no data will be returned.

Return Value:

    STATUS_SUCCESS - Indicates the operation was successful.

    STATUS_BUFFER_TOO_SMALL - if the requested information did not
        fit in the provided output buffer.  In this case, the
        ReturnLength OUT parameter contains the number of bytes
        actually needed to store the requested information.

--*/
{

    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;

    PTOKEN Token;

    ULONG RequiredLength;
    ULONG Index;
    ULONG GroupsLength;
    ULONG RestrictedSidsLength;
    ULONG PrivilegesLength;

    PTOKEN_TYPE LocalType;
    PTOKEN_USER LocalUser;
    PTOKEN_GROUPS LocalGroups;
    PTOKEN_PRIVILEGES LocalPrivileges;
    PTOKEN_OWNER LocalOwner;
    PTOKEN_PRIMARY_GROUP LocalPrimaryGroup;
    PTOKEN_DEFAULT_DACL LocalDefaultDacl;
    PTOKEN_SOURCE LocalSource;
    PSECURITY_IMPERSONATION_LEVEL LocalImpersonationLevel;
    PTOKEN_STATISTICS LocalStatistics;
    PTOKEN_GROUPS_AND_PRIVILEGES LocalGroupsAndPrivileges;
    PTOKEN_ORIGIN Origin ;

    PSID PSid;
    PACL PAcl;

    PVOID Ignore;
    ULONG SessionId;

    PTOKEN_AUDIT_POLICY pAuditPolicy; 
    LONG AuditPolicyElementCount;
    SEP_AUDIT_POLICY CurrentTokenAuditPolicy;

    PAGED_CODE();

    //
    // Get previous processor mode and probe output argument if necessary.
    //

    PreviousMode = KeGetPreviousMode();
    if (PreviousMode != KernelMode) {
        try {

            ProbeForWrite(
                TokenInformation,
                TokenInformationLength,
                sizeof(ULONG)
                );

            ProbeForWriteUlong(ReturnLength);

        } except(EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
    }

    //
    // Case on information class.
    //

    switch ( TokenInformationClass ) {

    case TokenUser:

        LocalUser = (PTOKEN_USER)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        //  Gain exclusive access to the token.
        //

        SepAcquireTokenReadLock( Token );



        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        RequiredLength = SeLengthSid( Token->UserAndGroups[0].Sid) +
                         (ULONG)sizeof( TOKEN_USER );

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        // Return the user SID
        //

        try {

            //
            //  Put SID immediately following TOKEN_USER data structure
            //
            PSid = (PSID)( (ULONG_PTR)LocalUser + (ULONG)sizeof(TOKEN_USER) );

            RtlCopySidAndAttributesArray(
                1,
                Token->UserAndGroups,
                RequiredLength,
                &(LocalUser->User),
                PSid,
                ((PSID *)&Ignore),
                ((PULONG)&Ignore)
                );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;

    case TokenGroups:

        LocalGroups = (PTOKEN_GROUPS)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        Index = 1;

        //
        //  Gain exclusive access to the token.
        //

        SepAcquireTokenReadLock( Token );

        //
        // Figure out how much space is needed to return the group SIDs.
        // That's the size of TOKEN_GROUPS (without any array entries)
        // plus the size of an SID_AND_ATTRIBUTES times the number of groups.
        // The number of groups is Token->UserAndGroups-1 (since the count
        // includes the user ID).  Then the lengths of each individual group
        // must be added.
        //

        RequiredLength = (ULONG)sizeof(TOKEN_GROUPS) +
                         ((Token->UserAndGroupCount - ANYSIZE_ARRAY - 1) *
                         ((ULONG)sizeof(SID_AND_ATTRIBUTES)) );

        while (Index < Token->UserAndGroupCount) {

            RequiredLength += SeLengthSid( Token->UserAndGroups[Index].Sid );

            Index += 1;

        } // endwhile

        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        // Now copy the groups.
        //

        try {

            LocalGroups->GroupCount = Token->UserAndGroupCount - 1;

            PSid = (PSID)( (ULONG_PTR)LocalGroups +
                           (ULONG)sizeof(TOKEN_GROUPS) +
                           (   (Token->UserAndGroupCount - ANYSIZE_ARRAY - 1) *
                               (ULONG)sizeof(SID_AND_ATTRIBUTES) )
                         );

            RtlCopySidAndAttributesArray(
                (ULONG)(Token->UserAndGroupCount - 1),
                &(Token->UserAndGroups[1]),
                RequiredLength,
                LocalGroups->Groups,
                PSid,
                ((PSID *)&Ignore),
                ((PULONG)&Ignore)
                );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;

    case TokenRestrictedSids:

        LocalGroups = (PTOKEN_GROUPS)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        Index = 0;

        //
        //  Gain exclusive access to the token.
        //

        SepAcquireTokenReadLock( Token );

        //
        // Figure out how much space is needed to return the group SIDs.
        // That's the size of TOKEN_GROUPS (without any array entries)
        // plus the size of an SID_AND_ATTRIBUTES times the number of groups.
        // The number of groups is Token->UserAndGroups-1 (since the count
        // includes the user ID).  Then the lengths of each individual group
        // must be added.
        //

        RequiredLength = (ULONG)sizeof(TOKEN_GROUPS) +
                         ((Token->RestrictedSidCount) *
                         ((ULONG)sizeof(SID_AND_ATTRIBUTES)) -
                         ANYSIZE_ARRAY * sizeof(SID_AND_ATTRIBUTES) );

        while (Index < Token->RestrictedSidCount) {

            RequiredLength += SeLengthSid( Token->RestrictedSids[Index].Sid );

            Index += 1;

        } // endwhile

        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        // Now copy the groups.
        //

        try {

            LocalGroups->GroupCount = Token->RestrictedSidCount;

            PSid = (PSID)( (ULONG_PTR)LocalGroups +
                           (ULONG)sizeof(TOKEN_GROUPS) +
                           (   (Token->RestrictedSidCount ) *
                               (ULONG)sizeof(SID_AND_ATTRIBUTES) -
                               ANYSIZE_ARRAY * sizeof(SID_AND_ATTRIBUTES) )
                         );

            RtlCopySidAndAttributesArray(
                (ULONG)(Token->RestrictedSidCount),
                Token->RestrictedSids,
                RequiredLength,
                LocalGroups->Groups,
                PSid,
                ((PSID *)&Ignore),
                ((PULONG)&Ignore)
                );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;

    case TokenPrivileges:

        LocalPrivileges = (PTOKEN_PRIVILEGES)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        //  Gain exclusive access to the token to prevent changes
        //  from occurring to the privileges.
        //

        SepAcquireTokenReadLock( Token );


        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        RequiredLength = (ULONG)sizeof(TOKEN_PRIVILEGES) +
                         ((Token->PrivilegeCount - ANYSIZE_ARRAY) *
                         ((ULONG)sizeof(LUID_AND_ATTRIBUTES)) );


        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        // Return the token privileges.
        //

        try {

            LocalPrivileges->PrivilegeCount = Token->PrivilegeCount;

            RtlCopyLuidAndAttributesArray(
                Token->PrivilegeCount,
                Token->Privileges,
                LocalPrivileges->Privileges
                );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;

    case TokenOwner:

        LocalOwner = (PTOKEN_OWNER)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        //  Gain exclusive access to the token to prevent changes
        //  from occurring to the owner.
        //

        SepAcquireTokenReadLock( Token );

        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        PSid = Token->UserAndGroups[Token->DefaultOwnerIndex].Sid;
        RequiredLength = (ULONG)sizeof(TOKEN_OWNER) +
                         SeLengthSid( PSid );

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        // Return the owner SID
        //

        PSid = (PSID)((ULONG_PTR)LocalOwner +
                      (ULONG)sizeof(TOKEN_OWNER));

        try {

            LocalOwner->Owner = PSid;

            Status = RtlCopySid(
                         (RequiredLength - (ULONG)sizeof(TOKEN_OWNER)),
                         PSid,
                         Token->UserAndGroups[Token->DefaultOwnerIndex].Sid
                         );

            ASSERT( NT_SUCCESS(Status) );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;

    case TokenPrimaryGroup:

        LocalPrimaryGroup = (PTOKEN_PRIMARY_GROUP)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        //  Gain exclusive access to the token to prevent changes
        //  from occurring to the owner.
        //

        SepAcquireTokenReadLock( Token );

        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        RequiredLength = (ULONG)sizeof(TOKEN_PRIMARY_GROUP) +
                         SeLengthSid( Token->PrimaryGroup );

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        // Return the primary group SID
        //

        PSid = (PSID)((ULONG_PTR)LocalPrimaryGroup +
                      (ULONG)sizeof(TOKEN_PRIMARY_GROUP));

        try {

            LocalPrimaryGroup->PrimaryGroup = PSid;

            Status = RtlCopySid( (RequiredLength - (ULONG)sizeof(TOKEN_PRIMARY_GROUP)),
                                 PSid,
                                 Token->PrimaryGroup
                                 );

            ASSERT( NT_SUCCESS(Status) );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;

    case TokenDefaultDacl:

        LocalDefaultDacl = (PTOKEN_DEFAULT_DACL)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        RequiredLength = (ULONG)sizeof(TOKEN_DEFAULT_DACL);

        //
        //  Gain exclusive access to the token to prevent changes
        //  from occurring to the owner.
        //

        SepAcquireTokenReadLock( Token );


        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        if (ARGUMENT_PRESENT(Token->DefaultDacl)) {

            RequiredLength += Token->DefaultDacl->AclSize;

        }

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        // Return the default Dacl
        //

        PAcl = (PACL)((ULONG_PTR)LocalDefaultDacl +
                      (ULONG)sizeof(TOKEN_DEFAULT_DACL));

        try {

            if (ARGUMENT_PRESENT(Token->DefaultDacl)) {

                LocalDefaultDacl->DefaultDacl = PAcl;

                RtlCopyMemory( (PVOID)PAcl,
                               (PVOID)Token->DefaultDacl,
                               Token->DefaultDacl->AclSize
                               );
            } else {

                LocalDefaultDacl->DefaultDacl = NULL;

            }

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;



    case TokenSource:

        LocalSource = (PTOKEN_SOURCE)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY_SOURCE,    // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        // The type of a token can not be changed, so
        // exclusive access to the token is not necessary.
        //

        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        RequiredLength = (ULONG) sizeof(TOKEN_SOURCE);

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }


        //
        // Return the token source
        //

        try {

            (*LocalSource) = Token->TokenSource;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        ObDereferenceObject( Token );
        return STATUS_SUCCESS;

    case TokenType:

        LocalType = (PTOKEN_TYPE)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        // The type of a token can not be changed, so
        // exclusive access to the token is not necessary.
        //

        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        RequiredLength = (ULONG) sizeof(TOKEN_TYPE);

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }


        //
        // Return the token type
        //

        try {

            (*LocalType) = Token->TokenType;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        ObDereferenceObject( Token );
        return STATUS_SUCCESS;


    case TokenImpersonationLevel:

        LocalImpersonationLevel = (PSECURITY_IMPERSONATION_LEVEL)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        // The impersonation level of a token can not be changed, so
        // exclusive access to the token is not necessary.
        //

        //
        //  Make sure the token is an appropriate type to be retrieving
        //  the impersonation level from.
        //

        if (Token->TokenType != TokenImpersonation) {

            ObDereferenceObject( Token );
            return STATUS_INVALID_INFO_CLASS;

        }

        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        RequiredLength = (ULONG) sizeof(SECURITY_IMPERSONATION_LEVEL);

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }


        //
        // Return the impersonation level
        //

        try {

            (*LocalImpersonationLevel) = Token->ImpersonationLevel;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        ObDereferenceObject( Token );
        return STATUS_SUCCESS;


    case TokenStatistics:

        LocalStatistics = (PTOKEN_STATISTICS)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        RequiredLength = (ULONG)sizeof( TOKEN_STATISTICS );




        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //


        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        //  Gain exclusive access to the token.
        //

        SepAcquireTokenReadLock( Token );

        //
        // Return the statistics
        //

        try {
            ULONG Size;

            LocalStatistics->TokenId            = Token->TokenId;
            LocalStatistics->AuthenticationId   = Token->AuthenticationId;
            LocalStatistics->ExpirationTime     = Token->ExpirationTime;
            LocalStatistics->TokenType          = Token->TokenType;
            LocalStatistics->ImpersonationLevel = Token->ImpersonationLevel;
            LocalStatistics->DynamicCharged     = Token->DynamicCharged;

            Size = Token->DynamicCharged - SeLengthSid( Token->PrimaryGroup );;

            if (Token->DefaultDacl) {
                Size -= Token->DefaultDacl->AclSize;
            }
            LocalStatistics->DynamicAvailable   = Size;
            LocalStatistics->GroupCount         = Token->UserAndGroupCount-1;
            LocalStatistics->PrivilegeCount     = Token->PrivilegeCount;
            LocalStatistics->ModifiedId         = Token->ModifiedId;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;

    case TokenSessionId:

        try {

            *ReturnLength = sizeof(ULONG);

        } except(EXCEPTION_EXECUTE_HANDLER) {

            return GetExceptionCode();
        }

        if ( TokenInformationLength < sizeof(ULONG) )
            return( STATUS_BUFFER_TOO_SMALL );

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        // Get SessionId for the token
        //
        SeQuerySessionIdToken( (PACCESS_TOKEN)Token,
                               &SessionId);

        try {

            *(PULONG)TokenInformation = SessionId;
            *ReturnLength = sizeof(ULONG);

        } except(EXCEPTION_EXECUTE_HANDLER) {
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        ObDereferenceObject( Token );
        return( STATUS_SUCCESS );


    case TokenGroupsAndPrivileges:

        LocalGroupsAndPrivileges = (PTOKEN_GROUPS_AND_PRIVILEGES)TokenInformation;

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        //  Gain exclusive access to the token.
        //

        SepAcquireTokenReadLock( Token );

        //
        // Figure out how much space is needed to return the group SIDs.
        // The data arrangement is as follows:
        //     GroupsAndPrivileges struct
        //     User and Groups
        //     Restricted sids
        //     Privileges
        //

        PrivilegesLength = Token->PrivilegeCount *
                           ((ULONG)sizeof(LUID_AND_ATTRIBUTES));

        GroupsLength = Token->UserAndGroupCount *
                       ((ULONG)sizeof(SID_AND_ATTRIBUTES));

        RestrictedSidsLength = Token->RestrictedSidCount *
                               ((ULONG)sizeof(SID_AND_ATTRIBUTES));

        Index = 0;
        while (Index < Token->UserAndGroupCount) {

            GroupsLength += SeLengthSid( Token->UserAndGroups[Index].Sid );

            Index += 1;

        } // endwhile

        Index = 0;
        while (Index < Token->RestrictedSidCount) {

            RestrictedSidsLength += SeLengthSid( Token->RestrictedSids[Index].Sid );

            Index += 1;

        } // endwhile

        RequiredLength = (ULONG)sizeof(TOKEN_GROUPS_AND_PRIVILEGES) +
                         PrivilegesLength + RestrictedSidsLength + GroupsLength;
        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;

        }

        //
        // Now copy the groups, followed by restricted sids, followed by
        // privileges.
        //

        try {

            LocalGroupsAndPrivileges->AuthenticationId = Token->AuthenticationId;

            LocalGroupsAndPrivileges->SidLength = GroupsLength;
            LocalGroupsAndPrivileges->SidCount = Token->UserAndGroupCount;
            LocalGroupsAndPrivileges->Sids = (PSID_AND_ATTRIBUTES) ((ULONG_PTR)LocalGroupsAndPrivileges +
                                               (ULONG)sizeof(TOKEN_GROUPS_AND_PRIVILEGES));

            LocalGroupsAndPrivileges->RestrictedSidLength = RestrictedSidsLength;
            LocalGroupsAndPrivileges->RestrictedSidCount = Token->RestrictedSidCount;

            //
            // To distinguish between a restricted token with zero sids and
            // a non-restricted token.
            //

            if (SeTokenIsRestricted((PACCESS_TOKEN) Token))
            {
                LocalGroupsAndPrivileges->RestrictedSids = (PSID_AND_ATTRIBUTES) ((ULONG_PTR) LocalGroupsAndPrivileges->Sids +
                                                             GroupsLength);
            }
            else
            {
                LocalGroupsAndPrivileges->RestrictedSids = NULL;
            }

            LocalGroupsAndPrivileges->PrivilegeLength = PrivilegesLength;
            LocalGroupsAndPrivileges->PrivilegeCount = Token->PrivilegeCount;
            LocalGroupsAndPrivileges->Privileges = (PLUID_AND_ATTRIBUTES) ((ULONG_PTR) LocalGroupsAndPrivileges->Sids + GroupsLength +
                                                    RestrictedSidsLength);

            PSid = (PSID)( (ULONG_PTR)LocalGroupsAndPrivileges->Sids +
                           (Token->UserAndGroupCount *
                           (ULONG)sizeof(SID_AND_ATTRIBUTES))
                         );

            RtlCopySidAndAttributesArray(
                (ULONG)Token->UserAndGroupCount,
                Token->UserAndGroups,
                GroupsLength - (Token->UserAndGroupCount * ((ULONG)sizeof(SID_AND_ATTRIBUTES))),
                LocalGroupsAndPrivileges->Sids,
                PSid,
                ((PSID *)&Ignore),
                ((PULONG)&Ignore)
                );

            PSid = (PSID)((ULONG_PTR)LocalGroupsAndPrivileges->RestrictedSids +
                           ((Token->RestrictedSidCount ) *
                            (ULONG)sizeof(SID_AND_ATTRIBUTES))
                         );

            if (LocalGroupsAndPrivileges->RestrictedSidCount > 0)
            {
                RtlCopySidAndAttributesArray(
                    (ULONG)(Token->RestrictedSidCount),
                    Token->RestrictedSids,
                    RestrictedSidsLength - (Token->RestrictedSidCount * ((ULONG)sizeof(SID_AND_ATTRIBUTES))),
                    LocalGroupsAndPrivileges->RestrictedSids,
                    PSid,
                    ((PSID *)&Ignore),
                    ((PULONG)&Ignore)
                    );
            }

            RtlCopyLuidAndAttributesArray(
                Token->PrivilegeCount,
                Token->Privileges,
                LocalGroupsAndPrivileges->Privileges
                );

        } except(EXCEPTION_EXECUTE_HANDLER) {

            SepReleaseTokenReadLock( Token );
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }


        SepReleaseTokenReadLock( Token );
        ObDereferenceObject( Token );
        return STATUS_SUCCESS;


    case TokenSandBoxInert:

        try {
            *ReturnLength = sizeof(ULONG);
        } except(EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }

        if ( TokenInformationLength < sizeof(ULONG) ) {
            return( STATUS_BUFFER_TOO_SMALL );
        }

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        try {

            //
            // If the flag is present in the token then return TRUE.
            // Else return FALSE.
            //

            *(PULONG)TokenInformation = (Token->TokenFlags & TOKEN_SANDBOX_INERT) 
                                              ? TRUE : FALSE;
        } except(EXCEPTION_EXECUTE_HANDLER) {
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        ObDereferenceObject( Token );
        return( STATUS_SUCCESS );


    case TokenAuditPolicy:
    {
        pAuditPolicy = (PTOKEN_AUDIT_POLICY)TokenInformation;
        AuditPolicyElementCount = 0;

        //                      
        // We only allow callers with Security privilege to read AuditPolicy
        //

        if ( !SeSinglePrivilegeCheck(SeSecurityPrivilege,PreviousMode) ) {
            
            return( STATUS_PRIVILEGE_NOT_HELD );
        
        }

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,     // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        //
        // Copy the audit policy structure.  It is larger than a DWORD
        // so the lock is needed to do this safely.
        //

        SepAcquireTokenReadLock( Token );
        CurrentTokenAuditPolicy = Token->AuditPolicy;
        SepReleaseTokenReadLock( Token );
        
        //
        // Figure out how much space is needed to return the audit policy.  Count
        // the policy elements present in the token.
        // 
        
        if (CurrentTokenAuditPolicy.Overlay) {
            
            if (CurrentTokenAuditPolicy.PolicyElements.System) {
                AuditPolicyElementCount++;
            }
            if (CurrentTokenAuditPolicy.PolicyElements.Logon) {
                AuditPolicyElementCount++;
            }
            if (CurrentTokenAuditPolicy.PolicyElements.ObjectAccess) {
                AuditPolicyElementCount++;
            }
            if (CurrentTokenAuditPolicy.PolicyElements.PrivilegeUse) {
                AuditPolicyElementCount++;
            }
            if (CurrentTokenAuditPolicy.PolicyElements.DetailedTracking) {
                AuditPolicyElementCount++;
            }
            if (CurrentTokenAuditPolicy.PolicyElements.PolicyChange) {
                AuditPolicyElementCount++;
            }
            if (CurrentTokenAuditPolicy.PolicyElements.AccountManagement) {
                AuditPolicyElementCount++;
            }
            if (CurrentTokenAuditPolicy.PolicyElements.DirectoryServiceAccess) {
                AuditPolicyElementCount++;
            }
            if (CurrentTokenAuditPolicy.PolicyElements.AccountLogon) {
                AuditPolicyElementCount++;
            }
        }

        RequiredLength = PER_USER_AUDITING_POLICY_SIZE_BY_COUNT(AuditPolicyElementCount);

        //
        // Return the length required now in case not enough buffer
        // was provided by the caller and we have to return an error.
        //

        try {

            *ReturnLength = RequiredLength;

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        if ( TokenInformationLength < RequiredLength ) {

            ObDereferenceObject( Token );
            return STATUS_BUFFER_TOO_SMALL;
        }

        //
        // Now construct the policy.
        //

        try {

            LONG PolicyIndex = 0;
            pAuditPolicy->PolicyCount = AuditPolicyElementCount;
            
            if (pAuditPolicy->PolicyCount) {
                
                if (CurrentTokenAuditPolicy.PolicyElements.System) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategorySystem;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.System;
                    PolicyIndex++;
                }
                if (CurrentTokenAuditPolicy.PolicyElements.Logon) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategoryLogon;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.Logon;
                    PolicyIndex++;
                }
                if (CurrentTokenAuditPolicy.PolicyElements.ObjectAccess) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategoryObjectAccess;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.ObjectAccess;
                    PolicyIndex++;
                }
                if (CurrentTokenAuditPolicy.PolicyElements.PrivilegeUse) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategoryPrivilegeUse;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.PrivilegeUse;
                    PolicyIndex++;
                }
                if (CurrentTokenAuditPolicy.PolicyElements.DetailedTracking) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategoryDetailedTracking;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.DetailedTracking;
                    PolicyIndex++;
                }
                if (CurrentTokenAuditPolicy.PolicyElements.PolicyChange) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategoryPolicyChange;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.PolicyChange;
                    PolicyIndex++;
                }
                if (CurrentTokenAuditPolicy.PolicyElements.AccountManagement) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategoryAccountManagement;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.AccountManagement;
                    PolicyIndex++;
                }
                if (CurrentTokenAuditPolicy.PolicyElements.DirectoryServiceAccess) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategoryDirectoryServiceAccess;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.DirectoryServiceAccess;
                    PolicyIndex++;
                }
                if (CurrentTokenAuditPolicy.PolicyElements.AccountLogon) {
                    pAuditPolicy->Policy[PolicyIndex].Category = AuditCategoryAccountLogon;
                    pAuditPolicy->Policy[PolicyIndex].PolicyMask = CurrentTokenAuditPolicy.PolicyElements.AccountLogon;
                    PolicyIndex++;
                }
            }

        } except(EXCEPTION_EXECUTE_HANDLER) {

            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        ObDereferenceObject( Token );
        return STATUS_SUCCESS;
    }

    case TokenOrigin:
    {
        try {
            *ReturnLength = sizeof( TOKEN_ORIGIN );

        }
        except ( EXCEPTION_EXECUTE_HANDLER ) {

            return GetExceptionCode() ;

        }

        if ( TokenInformationLength < sizeof( TOKEN_ORIGIN ) ) {
            return STATUS_BUFFER_TOO_SMALL ;
            
        }

        Status = ObReferenceObjectByHandle(
                 TokenHandle,           // Handle
                 TOKEN_QUERY,           // DesiredAccess
                 SeTokenObjectType,    // ObjectType
                 PreviousMode,          // AccessMode
                 (PVOID *)&Token,       // Object
                 NULL                   // GrantedAccess
                 );

        if ( !NT_SUCCESS(Status) ) {
            return Status;
        }

        try {

            //
            // If the flag is present in the token then return TRUE.
            // Else return FALSE.
            //

            Origin = (PTOKEN_ORIGIN) TokenInformation ;

            Origin->OriginatingLogonSession = Token->OriginatingLogonSession ;

        } except(EXCEPTION_EXECUTE_HANDLER) {
            ObDereferenceObject( Token );
            return GetExceptionCode();
        }

        ObDereferenceObject( Token );
        return( STATUS_SUCCESS );


    }



    default:

        return STATUS_INVALID_INFO_CLASS;
    }
}


NTSTATUS
SeQueryAuthenticationIdToken(
    __in PACCESS_TOKEN Token,
    __out PLUID AuthenticationId
    )

/*++


Routine Description:

    Retrieve authentication ID out of the token.

Arguments:

    Token - Referenced pointer to a token.

    AutenticationId - Receives the token's authentication ID.

Return Value:

    STATUS_SUCCESS - Indicates the operation was successful.

    This is the only expected status.

--*/
{
    PAGED_CODE();

    //
    // Token AuthenticationId is a readonly field. No locks are required
    // to read this as its constant for the life of the token.
    //

    (*AuthenticationId) = ((PTOKEN)Token)->AuthenticationId;

    return(STATUS_SUCCESS);
}



NTSTATUS
SeQueryInformationToken (
    __in PACCESS_TOKEN AccessToken,
    __in TOKEN_INFORMATION_CLASS TokenInformationClass,
    __deref_out PVOID *TokenInformation
    )

/*++


Routine Description:

    Retrieve information about a specified token.

Arguments:

    TokenHandle - Provides a handle to the token to operate on.

    TokenInformationClass - The token information class about which
        to retrieve information.

    TokenInformation - Receives a pointer to the requested information.
        The actual structures returned are dependent upon the information
        class requested, as defined in the TokenInformationClass parameter
        description.

        TokenInformation Format By Information Class:

           TokenUser => TOKEN_USER data structure.  TOKEN_QUERY
           access is needed to retrieve this information about a
           token.

           TokenGroups => TOKEN_GROUPS data structure.  TOKEN_QUERY
           access is needed to retrieve this information about a
           token.

           TokenPrivileges => TOKEN_PRIVILEGES data structure.
           TOKEN_QUERY access is needed to retrieve this information
           about a token.

           TokenOwner => TOKEN_OWNER data structure.  TOKEN_QUERY
           access is needed to retrieve this information about a
           token.

           TokenPrimaryGroup => TOKEN_PRIMARY_GROUP data structure.
           TOKEN_QUERY access is needed to retrieve this information
           about a token.

           TokenDefaultDacl => TOKEN_DEFAULT_DACL data structure.
           TOKEN_QUERY access is needed to retrieve this information
           about a token.

           TokenSource => TOKEN_SOURCE data structure.
           TOKEN_QUERY_SOURCE access is needed to retrieve this
           information about a token.

           TokenType => TOKEN_TYPE data structure.
           TOKEN_QUERY access is needed to retrieve this information
           about a token.

           TokenStatistics => TOKEN_STATISTICS data structure.
           TOKEN_QUERY access is needed to retrieve this
           information about a token.

Return Value:

    STATUS_SUCCESS - Indicates the operation was successful.

--*/
{

    NTSTATUS Status;

    ULONG RequiredLength;
    ULONG Index;

    PSID PSid;
    PACL PAcl;

    PVOID Ignore;
    PTOKEN Token = (PTOKEN)AccessToken;

    PAGED_CODE();

    //
    // Case on information class.
    //

    switch ( TokenInformationClass ) {

        case TokenUser:
            {
                PTOKEN_USER LocalUser;

                //
                //  Gain exclusive access to the token.
                //

                SepAcquireTokenReadLock( Token );

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //

                RequiredLength = SeLengthSid( Token->UserAndGroups[0].Sid) +
                                 (ULONG)sizeof( TOKEN_USER );

                LocalUser = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalUser == NULL) {
                    SepReleaseTokenReadLock( Token );
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Return the user SID
                //
                //  Put SID immediately following TOKEN_USER data structure
                //

                PSid = (PSID)( (ULONG_PTR)LocalUser + (ULONG)sizeof(TOKEN_USER) );

                RtlCopySidAndAttributesArray(
                    1,
                    Token->UserAndGroups,
                    RequiredLength,
                    &(LocalUser->User),
                    PSid,
                    ((PSID *)&Ignore),
                    ((PULONG)&Ignore)
                    );

                SepReleaseTokenReadLock( Token );
                *TokenInformation = LocalUser;
                return STATUS_SUCCESS;
            }


        case TokenGroups:
            {
                PTOKEN_GROUPS LocalGroups;

                //
                //  Gain exclusive access to the token.
                //

                SepAcquireTokenReadLock( Token );

                //
                // Figure out how much space is needed to return the group SIDs.
                // That's the size of TOKEN_GROUPS (without any array entries)
                // plus the size of an SID_AND_ATTRIBUTES times the number of groups.
                // The number of groups is Token->UserAndGroups-1 (since the count
                // includes the user ID).  Then the lengths of each individual group
                // must be added.
                //

                RequiredLength = (ULONG)sizeof(TOKEN_GROUPS) +
                                 ((Token->UserAndGroupCount - ANYSIZE_ARRAY - 1) *
                                 ((ULONG)sizeof(SID_AND_ATTRIBUTES)) );

                Index = 1;
                while (Index < Token->UserAndGroupCount) {

                    RequiredLength += SeLengthSid( Token->UserAndGroups[Index].Sid );

                    Index += 1;

                } // endwhile

                LocalGroups = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalGroups == NULL) {
                    SepReleaseTokenReadLock( Token );
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Now copy the groups.
                //

                LocalGroups->GroupCount = Token->UserAndGroupCount - 1;

                PSid = (PSID)( (ULONG_PTR)LocalGroups +
                               (ULONG)sizeof(TOKEN_GROUPS) +
                               (   (Token->UserAndGroupCount - ANYSIZE_ARRAY - 1) *
                                   (ULONG)sizeof(SID_AND_ATTRIBUTES) )
                             );

                RtlCopySidAndAttributesArray(
                    (ULONG)(Token->UserAndGroupCount - 1),
                    &(Token->UserAndGroups[1]),
                    RequiredLength,
                    LocalGroups->Groups,
                    PSid,
                    ((PSID *)&Ignore),
                    ((PULONG)&Ignore)
                    );

                SepReleaseTokenReadLock( Token );
                *TokenInformation = LocalGroups;
                return STATUS_SUCCESS;
            }


        case TokenPrivileges:
            {
                PTOKEN_PRIVILEGES LocalPrivileges;

                //
                //  Gain exclusive access to the token to prevent changes
                //  from occurring to the privileges.
                //

                SepAcquireTokenReadLock( Token );

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //

                RequiredLength = (ULONG)sizeof(TOKEN_PRIVILEGES) +
                                 ((Token->PrivilegeCount - ANYSIZE_ARRAY) *
                                 ((ULONG)sizeof(LUID_AND_ATTRIBUTES)) );

                LocalPrivileges = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalPrivileges == NULL) {
                    SepReleaseTokenReadLock( Token );
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Return the token privileges.
                //

                LocalPrivileges->PrivilegeCount = Token->PrivilegeCount;

                RtlCopyLuidAndAttributesArray(
                    Token->PrivilegeCount,
                    Token->Privileges,
                    LocalPrivileges->Privileges
                    );

                SepReleaseTokenReadLock( Token );
                *TokenInformation = LocalPrivileges;
                return STATUS_SUCCESS;
            }


        case TokenOwner:
            {
                PTOKEN_OWNER LocalOwner;

                //
                //  Gain exclusive access to the token to prevent changes
                //  from occurring to the owner.
                //

                SepAcquireTokenReadLock( Token );

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //

                PSid = Token->UserAndGroups[Token->DefaultOwnerIndex].Sid;
                RequiredLength = (ULONG)sizeof(TOKEN_OWNER) +
                                 SeLengthSid( PSid );

                LocalOwner = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalOwner == NULL) {
                    SepReleaseTokenReadLock( Token );
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Return the owner SID
                //

                PSid = (PSID)((ULONG_PTR)LocalOwner +
                              (ULONG)sizeof(TOKEN_OWNER));

                LocalOwner->Owner = PSid;

                Status = RtlCopySid(
                             (RequiredLength - (ULONG)sizeof(TOKEN_OWNER)),
                             PSid,
                             Token->UserAndGroups[Token->DefaultOwnerIndex].Sid
                             );

                ASSERT( NT_SUCCESS(Status) );

                SepReleaseTokenReadLock( Token );
                *TokenInformation = LocalOwner;
                return STATUS_SUCCESS;
            }


        case TokenPrimaryGroup:
            {
                PTOKEN_PRIMARY_GROUP LocalPrimaryGroup;

                //
                //  Gain exclusive access to the token to prevent changes
                //  from occurring to the owner.
                //

                SepAcquireTokenReadLock( Token );

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //

                RequiredLength = (ULONG)sizeof(TOKEN_PRIMARY_GROUP) +
                                 SeLengthSid( Token->PrimaryGroup );

                LocalPrimaryGroup = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalPrimaryGroup == NULL) {
                    SepReleaseTokenReadLock( Token );
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Return the primary group SID
                //

                PSid = (PSID)((ULONG_PTR)LocalPrimaryGroup +
                              (ULONG)sizeof(TOKEN_PRIMARY_GROUP));

                LocalPrimaryGroup->PrimaryGroup = PSid;

                Status = RtlCopySid( (RequiredLength - (ULONG)sizeof(TOKEN_PRIMARY_GROUP)),
                                     PSid,
                                     Token->PrimaryGroup
                                     );

                ASSERT( NT_SUCCESS(Status) );

                SepReleaseTokenReadLock( Token );
                *TokenInformation = LocalPrimaryGroup;
                return STATUS_SUCCESS;
            }


        case TokenDefaultDacl:
            {
                PTOKEN_DEFAULT_DACL LocalDefaultDacl;

                RequiredLength = (ULONG)sizeof(TOKEN_DEFAULT_DACL);

                //
                //  Gain exclusive access to the token to prevent changes
                //  from occurring to the owner.
                //

                SepAcquireTokenReadLock( Token );

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //


                if (ARGUMENT_PRESENT(Token->DefaultDacl)) {

                    RequiredLength += Token->DefaultDacl->AclSize;
                }

                LocalDefaultDacl = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalDefaultDacl == NULL) {
                    SepReleaseTokenReadLock( Token );
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Return the default Dacl
                //

                PAcl = (PACL)((ULONG_PTR)LocalDefaultDacl +
                              (ULONG)sizeof(TOKEN_DEFAULT_DACL));

                if (ARGUMENT_PRESENT(Token->DefaultDacl)) {

                    LocalDefaultDacl->DefaultDacl = PAcl;

                    RtlCopyMemory( (PVOID)PAcl,
                                   (PVOID)Token->DefaultDacl,
                                   Token->DefaultDacl->AclSize
                                   );
                } else {

                    LocalDefaultDacl->DefaultDacl = NULL;
                }

                SepReleaseTokenReadLock( Token );
                *TokenInformation = LocalDefaultDacl;
                return STATUS_SUCCESS;
            }


        case TokenSource:
            {
                PTOKEN_SOURCE LocalSource;

                //
                // The type of a token can not be changed, so
                // exclusive access to the token is not necessary.
                //

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //

                RequiredLength = (ULONG) sizeof(TOKEN_SOURCE);

                LocalSource = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalSource == NULL) {
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Return the token source
                //

                (*LocalSource) = Token->TokenSource;
                *TokenInformation = LocalSource;

                return STATUS_SUCCESS;
            }


        case TokenType:
            {
                PTOKEN_TYPE LocalType;

                //
                // The type of a token can not be changed, so
                // exclusive access to the token is not necessary.
                //

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //

                RequiredLength = (ULONG) sizeof(TOKEN_TYPE);

                LocalType = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalType == NULL) {
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Return the token type
                //

                (*LocalType) = Token->TokenType;
                *TokenInformation = LocalType;
                return STATUS_SUCCESS;
            }


        case TokenImpersonationLevel:
            {
                PSECURITY_IMPERSONATION_LEVEL LocalImpersonationLevel;

                //
                // The impersonation level of a token can not be changed, so
                // exclusive access to the token is not necessary.
                //

                //
                //  Make sure the token is an appropriate type to be retrieving
                //  the impersonation level from.
                //

                if (Token->TokenType != TokenImpersonation) {

                    return STATUS_INVALID_INFO_CLASS;
                }

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //

                RequiredLength = (ULONG) sizeof(SECURITY_IMPERSONATION_LEVEL);

                LocalImpersonationLevel = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalImpersonationLevel == NULL) {
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Return the impersonation level
                //

                (*LocalImpersonationLevel) = Token->ImpersonationLevel;
                *TokenInformation = LocalImpersonationLevel;
                return STATUS_SUCCESS;
            }


        case TokenStatistics:
            {
                PTOKEN_STATISTICS LocalStatistics;
                ULONG Size;

                //
                // Return the length required now in case not enough buffer
                // was provided by the caller and we have to return an error.
                //

                RequiredLength = (ULONG)sizeof( TOKEN_STATISTICS );

                LocalStatistics = ExAllocatePool( PagedPool, RequiredLength );

                if (LocalStatistics == NULL) {
                    return( STATUS_INSUFFICIENT_RESOURCES );
                }

                //
                // Copy readonly fields outside of the lock
                //
                LocalStatistics->TokenId            = Token->TokenId;
                LocalStatistics->AuthenticationId   = Token->AuthenticationId;
                LocalStatistics->TokenType          = Token->TokenType;
                LocalStatistics->ImpersonationLevel = Token->ImpersonationLevel;
                LocalStatistics->ExpirationTime     = Token->ExpirationTime;

                //
                //  Gain shared access to the token.
                //

                SepAcquireTokenReadLock( Token );

                //
                // Return the statistics
                //

                LocalStatistics->DynamicCharged     = Token->DynamicCharged;

                Size = Token->DynamicCharged - SeLengthSid( Token->PrimaryGroup );

                if (Token->DefaultDacl) {
                    Size -= Token->DefaultDacl->AclSize;
                }

                LocalStatistics->DynamicAvailable   = Size;
                LocalStatistics->DynamicAvailable   = Token->DynamicAvailable;
                LocalStatistics->GroupCount         = Token->UserAndGroupCount-1;
                LocalStatistics->PrivilegeCount     = Token->PrivilegeCount;
                LocalStatistics->ModifiedId         = Token->ModifiedId;

                SepReleaseTokenReadLock( Token );
                *TokenInformation = LocalStatistics;
                return STATUS_SUCCESS;
            }

    case TokenSessionId:

        /*
         * Get SessionId for the token
         */
        SeQuerySessionIdToken( (PACCESS_TOKEN)Token,
                             (PULONG)TokenInformation );

        return( STATUS_SUCCESS );

    default:

        return STATUS_INVALID_INFO_CLASS;
    }
}



NTSTATUS
SeQuerySessionIdToken(
    __in PACCESS_TOKEN Token,
    __out PULONG SessionId
    )

/*++


Routine Description:

    Gets the SessionId from the specified token object.

Arguments:

    Token (input)
      Opaque kernel ACCESS_TOKEN pointer
    SessionId (output)
      pointer to location to return SessionId

Return Value:

    STATUS_SUCCESS - no error

--*/
{
    PAGED_CODE();

    //
    // Get the SessionId.
    //
    SepAcquireTokenReadLock( ((PTOKEN)Token) );
    (*SessionId) = ((PTOKEN)Token)->SessionId;
    SepReleaseTokenReadLock( ((PTOKEN)Token) );

    return( STATUS_SUCCESS );
}

