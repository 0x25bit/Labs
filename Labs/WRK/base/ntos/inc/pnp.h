/*++ BUILD Version: 0001    // Increment this if a change has global effects

Copyright (c) Microsoft Corporation. All rights reserved. 

You may only use this code if you agree to the terms of the Windows Research Kernel Source Code License agreement (see License.txt).
If you do not agree to the terms, do not use the code.


Module Name:

    pnp.h

Abstract:

    This module contains the internal structure definitions and APIs used by
    the kernel-mode Plug and Play manager.

    This file is included by including "ntos.h".

--*/

#ifndef _PNP_
#define _PNP_

//
// The following global variables provide/control access to PnP Manager data.
//

extern ERESOURCE  PpRegistryDeviceResource;
extern PDRIVER_OBJECT IoPnpDriverObject;

// begin_ntddk begin_nthal begin_ntifs begin_wdm begin_ntosp

//
// Define PnP Device Property for IoGetDeviceProperty
//

typedef enum {
    DevicePropertyDeviceDescription,
    DevicePropertyHardwareID,
    DevicePropertyCompatibleIDs,
    DevicePropertyBootConfiguration,
    DevicePropertyBootConfigurationTranslated,
    DevicePropertyClassName,
    DevicePropertyClassGuid,
    DevicePropertyDriverKeyName,
    DevicePropertyManufacturer,
    DevicePropertyFriendlyName,
    DevicePropertyLocationInformation,
    DevicePropertyPhysicalDeviceObjectName,
    DevicePropertyBusTypeGuid,
    DevicePropertyLegacyBusType,
    DevicePropertyBusNumber,
    DevicePropertyEnumeratorName,
    DevicePropertyAddress,
    DevicePropertyUINumber,
    DevicePropertyInstallState,
    DevicePropertyRemovalPolicy
} DEVICE_REGISTRY_PROPERTY;

typedef BOOLEAN (*PTRANSLATE_BUS_ADDRESS)(
    IN PVOID Context,
    IN PHYSICAL_ADDRESS BusAddress,
    IN ULONG Length,
    IN OUT PULONG AddressSpace,
    OUT PPHYSICAL_ADDRESS TranslatedAddress
    );

typedef struct _DMA_ADAPTER *(*PGET_DMA_ADAPTER)(
    IN PVOID Context,
    IN struct _DEVICE_DESCRIPTION *DeviceDescriptor,
    OUT PULONG NumberOfMapRegisters
    );

typedef ULONG (*PGET_SET_DEVICE_DATA)(
    IN PVOID Context,
    IN ULONG DataType,
    IN PVOID Buffer,
    IN ULONG Offset,
    IN ULONG Length
    );

typedef enum _DEVICE_INSTALL_STATE {
    InstallStateInstalled,
    InstallStateNeedsReinstall,
    InstallStateFailedInstall,
    InstallStateFinishInstall
} DEVICE_INSTALL_STATE, *PDEVICE_INSTALL_STATE;

//
// Define structure returned in response to IRP_MN_QUERY_BUS_INFORMATION by a
// PDO indicating the type of bus the device exists on.
//

typedef struct _PNP_BUS_INFORMATION {
    GUID BusTypeGuid;
    INTERFACE_TYPE LegacyBusType;
    ULONG BusNumber;
} PNP_BUS_INFORMATION, *PPNP_BUS_INFORMATION;

//
// Define structure returned in response to IRP_MN_QUERY_LEGACY_BUS_INFORMATION
// by an FDO indicating the type of bus it is.  This is normally the same bus
// type as the device's children (i.e., as retrieved from the child PDO's via
// IRP_MN_QUERY_BUS_INFORMATION) except for cases like CardBus, which can
// support both 16-bit (PCMCIABus) and 32-bit (PCIBus) cards.
//

typedef struct _LEGACY_BUS_INFORMATION {
    GUID BusTypeGuid;
    INTERFACE_TYPE LegacyBusType;
    ULONG BusNumber;
} LEGACY_BUS_INFORMATION, *PLEGACY_BUS_INFORMATION;

//
// Defines for IoGetDeviceProperty(DevicePropertyRemovalPolicy).
//
typedef enum _DEVICE_REMOVAL_POLICY {

// end_ntddk end_wdm end_nthal end_ntifs end_ntosp
    RemovalPolicyNotDetermined = 0,
// begin_ntddk begin_wdm begin_nthal begin_ntifs begin_ntosp
    RemovalPolicyExpectNoRemoval = 1,
    RemovalPolicyExpectOrderlyRemoval = 2,
    RemovalPolicyExpectSurpriseRemoval = 3
// end_ntddk end_wdm end_nthal end_ntifs end_ntosp
                                          ,
    RemovalPolicySuggestOrderlyRemoval = 4,
    RemovalPolicySuggestSurpriseRemoval = 5,
    RemovalPolicyUnspecified = 6
// begin_ntddk begin_wdm begin_nthal begin_ntifs begin_ntosp

} DEVICE_REMOVAL_POLICY, *PDEVICE_REMOVAL_POLICY;



typedef struct _BUS_INTERFACE_STANDARD {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    //
    // standard bus interfaces
    //
    PTRANSLATE_BUS_ADDRESS TranslateBusAddress;
    PGET_DMA_ADAPTER GetDmaAdapter;
    PGET_SET_DEVICE_DATA SetBusData;
    PGET_SET_DEVICE_DATA GetBusData;

} BUS_INTERFACE_STANDARD, *PBUS_INTERFACE_STANDARD;

// end_wdm
typedef struct _AGP_TARGET_BUS_INTERFACE_STANDARD {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    //
    // config munging routines
    //
    PGET_SET_DEVICE_DATA SetBusData;
    PGET_SET_DEVICE_DATA GetBusData;
    UCHAR CapabilityID;  // 2 (AGPv2 host) or new 0xE (AGPv3 bridge)

} AGP_TARGET_BUS_INTERFACE_STANDARD, *PAGP_TARGET_BUS_INTERFACE_STANDARD;
// begin_wdm

//
// The following definitions are used in ACPI QueryInterface
//
typedef BOOLEAN (* PGPE_SERVICE_ROUTINE) (
                            PVOID,
                            PVOID);

typedef NTSTATUS (* PGPE_CONNECT_VECTOR) (
                            PDEVICE_OBJECT,
                            ULONG,
                            KINTERRUPT_MODE,
                            BOOLEAN,
                            PGPE_SERVICE_ROUTINE,
                            PVOID,
                            PVOID);

typedef NTSTATUS (* PGPE_DISCONNECT_VECTOR) (
                            PVOID);

typedef NTSTATUS (* PGPE_ENABLE_EVENT) (
                            PDEVICE_OBJECT,
                            PVOID);

typedef NTSTATUS (* PGPE_DISABLE_EVENT) (
                            PDEVICE_OBJECT,
                            PVOID);

typedef NTSTATUS (* PGPE_CLEAR_STATUS) (
                            PDEVICE_OBJECT,
                            PVOID);

typedef VOID (* PDEVICE_NOTIFY_CALLBACK) (
                            PVOID,
                            ULONG);

typedef NTSTATUS (* PREGISTER_FOR_DEVICE_NOTIFICATIONS) (
                            PDEVICE_OBJECT,
                            PDEVICE_NOTIFY_CALLBACK,
                            PVOID);

typedef void (* PUNREGISTER_FOR_DEVICE_NOTIFICATIONS) (
                            PDEVICE_OBJECT,
                            PDEVICE_NOTIFY_CALLBACK);

typedef struct _ACPI_INTERFACE_STANDARD {
    //
    // Generic interface header
    //
    USHORT                  Size;
    USHORT                  Version;
    PVOID                   Context;
    PINTERFACE_REFERENCE    InterfaceReference;
    PINTERFACE_DEREFERENCE  InterfaceDereference;
    //
    // ACPI interfaces
    //
    PGPE_CONNECT_VECTOR                     GpeConnectVector;
    PGPE_DISCONNECT_VECTOR                  GpeDisconnectVector;
    PGPE_ENABLE_EVENT                       GpeEnableEvent;
    PGPE_DISABLE_EVENT                      GpeDisableEvent;
    PGPE_CLEAR_STATUS                       GpeClearStatus;
    PREGISTER_FOR_DEVICE_NOTIFICATIONS      RegisterForDeviceNotifications;
    PUNREGISTER_FOR_DEVICE_NOTIFICATIONS    UnregisterForDeviceNotifications;

} ACPI_INTERFACE_STANDARD, *PACPI_INTERFACE_STANDARD;

// end_wdm end_ntddk

typedef enum _ACPI_REG_TYPE {
    PM1a_ENABLE,
    PM1b_ENABLE,
    PM1a_STATUS,
    PM1b_STATUS,
    PM1a_CONTROL,
    PM1b_CONTROL,
    GP_STATUS,
    GP_ENABLE,
    SMI_CMD,
    MaxRegType
} ACPI_REG_TYPE, *PACPI_REG_TYPE;

typedef USHORT (*PREAD_ACPI_REGISTER) (
  IN ACPI_REG_TYPE AcpiReg,
  IN ULONG         Register);

typedef VOID (*PWRITE_ACPI_REGISTER) (
  IN ACPI_REG_TYPE AcpiReg,
  IN ULONG         Register,
  IN USHORT        Value
  );

typedef struct ACPI_REGS_INTERFACE_STANDARD {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID  Context;
    PINTERFACE_REFERENCE   InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    //
    // READ/WRITE_ACPI_REGISTER functions
    //
    PREAD_ACPI_REGISTER  ReadAcpiRegister;
    PWRITE_ACPI_REGISTER WriteAcpiRegister;

} ACPI_REGS_INTERFACE_STANDARD, *PACPI_REGS_INTERFACE_STANDARD;


typedef NTSTATUS (*PHAL_QUERY_ALLOCATE_PORT_RANGE) (
  IN BOOLEAN IsSparse,
  IN BOOLEAN PrimaryIsMmio,
  IN PVOID VirtBaseAddr OPTIONAL,
  IN PHYSICAL_ADDRESS PhysBaseAddr,  // Only valid if PrimaryIsMmio = TRUE
  IN ULONG Length,                   // Only valid if PrimaryIsMmio = TRUE
  OUT PUSHORT NewRangeId
  );

typedef VOID (*PHAL_FREE_PORT_RANGE)(
    IN USHORT RangeId
    );


typedef struct _HAL_PORT_RANGE_INTERFACE {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID  Context;
    PINTERFACE_REFERENCE   InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    //
    // QueryAllocateRange/FreeRange functions
    //
    PHAL_QUERY_ALLOCATE_PORT_RANGE QueryAllocateRange;
    PHAL_FREE_PORT_RANGE FreeRange;

} HAL_PORT_RANGE_INTERFACE, *PHAL_PORT_RANGE_INTERFACE;


//
// describe the CMOS HAL interface
//

typedef enum _CMOS_DEVICE_TYPE {
    CmosTypeStdPCAT,
    CmosTypeIntelPIIX4,
    CmosTypeDal1501
} CMOS_DEVICE_TYPE;


typedef
ULONG
(*PREAD_ACPI_CMOS) (
    IN CMOS_DEVICE_TYPE     CmosType,
    IN ULONG                SourceAddress,
    IN PUCHAR               DataBuffer,
    IN ULONG                ByteCount
    );

typedef
ULONG
(*PWRITE_ACPI_CMOS) (
    IN CMOS_DEVICE_TYPE     CmosType,
    IN ULONG                SourceAddress,
    IN PUCHAR               DataBuffer,
    IN ULONG                ByteCount
    );

typedef struct _ACPI_CMOS_INTERFACE_STANDARD {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID  Context;
    PINTERFACE_REFERENCE   InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    //
    // READ/WRITE_ACPI_CMOS functions
    //
    PREAD_ACPI_CMOS     ReadCmos;
    PWRITE_ACPI_CMOS    WriteCmos;

} ACPI_CMOS_INTERFACE_STANDARD, *PACPI_CMOS_INTERFACE_STANDARD;

//
// These definitions are used for getting PCI Interrupt Routing interfaces
//

typedef struct {
    PVOID   LinkNode;
    ULONG   StaticVector;
    UCHAR   Flags;
} ROUTING_TOKEN, *PROUTING_TOKEN;

//
// Flag indicating that the device supports
// MSI interrupt routing or that the provided token contains
// MSI routing information
//

#define PCI_MSI_ROUTING         0x1
#define PCI_STATIC_ROUTING      0x2

typedef
NTSTATUS
(*PGET_INTERRUPT_ROUTING)(
    IN  PDEVICE_OBJECT  Pdo,
    OUT ULONG           *Bus,
    OUT ULONG           *PciSlot,
    OUT UCHAR           *InterruptLine,
    OUT UCHAR           *InterruptPin,
    OUT UCHAR           *ClassCode,
    OUT UCHAR           *SubClassCode,
    OUT PDEVICE_OBJECT  *ParentPdo,
    OUT ROUTING_TOKEN   *RoutingToken,
    OUT UCHAR           *Flags
    );

typedef
NTSTATUS
(*PSET_INTERRUPT_ROUTING_TOKEN)(
    IN  PDEVICE_OBJECT  Pdo,
    IN  PROUTING_TOKEN  RoutingToken
    );

typedef
VOID
(*PUPDATE_INTERRUPT_LINE)(
    IN PDEVICE_OBJECT Pdo,
    IN UCHAR LineRegister
    );

typedef struct _INT_ROUTE_INTERFACE_STANDARD {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    //
    // standard bus interfaces
    //
    PGET_INTERRUPT_ROUTING GetInterruptRouting;
    PSET_INTERRUPT_ROUTING_TOKEN SetInterruptRoutingToken;
    PUPDATE_INTERRUPT_LINE UpdateInterruptLine;

} INT_ROUTE_INTERFACE_STANDARD, *PINT_ROUTE_INTERFACE_STANDARD;

// Some well-known interface versions supported by the PCI Bus Driver

#define PCI_INT_ROUTE_INTRF_STANDARD_VER 1

// end_nthal end_ntifs end_ntosp

NTKERNELAPI
BOOLEAN
PpInitSystem (
    VOID
    );

NTKERNELAPI
NTSTATUS
PpDeviceRegistration(
    IN PUNICODE_STRING DeviceInstancePath,
    IN BOOLEAN Add,
    IN PUNICODE_STRING ServiceKeyName OPTIONAL
    );

// begin_ntosp
NTKERNELAPI
NTSTATUS
IoSynchronousInvalidateDeviceRelations(
    PDEVICE_OBJECT DeviceObject,
    DEVICE_RELATION_TYPE Type
    );

// begin_ntddk begin_nthal begin_ntifs

NTKERNELAPI
NTSTATUS
IoReportDetectedDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN INTERFACE_TYPE LegacyBusType,
    IN ULONG BusNumber,
    IN ULONG SlotNumber,
    IN PCM_RESOURCE_LIST ResourceList,
    IN PIO_RESOURCE_REQUIREMENTS_LIST ResourceRequirements OPTIONAL,
    IN BOOLEAN ResourceAssigned,
    IN OUT PDEVICE_OBJECT *DeviceObject
    );

// begin_wdm

NTKERNELAPI
VOID
IoInvalidateDeviceRelations(
    IN PDEVICE_OBJECT DeviceObject,
    IN DEVICE_RELATION_TYPE Type
    );

NTKERNELAPI
VOID
IoRequestDeviceEject(
    IN PDEVICE_OBJECT PhysicalDeviceObject
    );

NTKERNELAPI
NTSTATUS
IoGetDeviceProperty(
    IN PDEVICE_OBJECT DeviceObject,
    IN DEVICE_REGISTRY_PROPERTY DeviceProperty,
    IN ULONG BufferLength,
    OUT PVOID PropertyBuffer,
    OUT PULONG ResultLength
    );

//
// The following definitions are used in IoOpenDeviceRegistryKey
//

#define PLUGPLAY_REGKEY_DEVICE  1
#define PLUGPLAY_REGKEY_DRIVER  2
#define PLUGPLAY_REGKEY_CURRENT_HWPROFILE 4

NTKERNELAPI
NTSTATUS
IoOpenDeviceRegistryKey(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG DevInstKeyType,
    IN ACCESS_MASK DesiredAccess,
    OUT PHANDLE DevInstRegKey
    );

NTKERNELAPI
NTSTATUS
NTAPI
IoRegisterDeviceInterface(
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    IN CONST GUID *InterfaceClassGuid,
    IN PUNICODE_STRING ReferenceString,     OPTIONAL
    OUT PUNICODE_STRING SymbolicLinkName
    );

NTKERNELAPI
NTSTATUS
IoOpenDeviceInterfaceRegistryKey(
    IN PUNICODE_STRING SymbolicLinkName,
    IN ACCESS_MASK DesiredAccess,
    OUT PHANDLE DeviceInterfaceKey
    );

NTKERNELAPI
NTSTATUS
IoSetDeviceInterfaceState(
    IN PUNICODE_STRING SymbolicLinkName,
    IN BOOLEAN Enable
    );

NTKERNELAPI
NTSTATUS
NTAPI
IoGetDeviceInterfaces(
    IN CONST GUID *InterfaceClassGuid,
    IN PDEVICE_OBJECT PhysicalDeviceObject OPTIONAL,
    IN ULONG Flags,
    OUT PWSTR *SymbolicLinkList
    );

#define DEVICE_INTERFACE_INCLUDE_NONACTIVE   0x00000001

NTKERNELAPI
NTSTATUS
NTAPI
IoGetDeviceInterfaceAlias(
    IN PUNICODE_STRING SymbolicLinkName,
    IN CONST GUID *AliasInterfaceClassGuid,
    OUT PUNICODE_STRING AliasSymbolicLinkName
    );

//
// Define PnP notification event categories
//

typedef enum _IO_NOTIFICATION_EVENT_CATEGORY {
    EventCategoryReserved,
    EventCategoryHardwareProfileChange,
    EventCategoryDeviceInterfaceChange,
    EventCategoryTargetDeviceChange
} IO_NOTIFICATION_EVENT_CATEGORY;

//
// Define flags that modify the behavior of IoRegisterPlugPlayNotification
// for the various event categories...
//

#define PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES    0x00000001

typedef
NTSTATUS
(*PDRIVER_NOTIFICATION_CALLBACK_ROUTINE) (
    IN PVOID NotificationStructure,
    IN PVOID Context
);


NTKERNELAPI
NTSTATUS
IoRegisterPlugPlayNotification(
    IN IO_NOTIFICATION_EVENT_CATEGORY EventCategory,
    IN ULONG EventCategoryFlags,
    IN PVOID EventCategoryData OPTIONAL,
    IN PDRIVER_OBJECT DriverObject,
    IN PDRIVER_NOTIFICATION_CALLBACK_ROUTINE CallbackRoutine,
    IN PVOID Context,
    OUT PVOID *NotificationEntry
    );

NTKERNELAPI
NTSTATUS
IoUnregisterPlugPlayNotification(
    IN PVOID NotificationEntry
    );

NTKERNELAPI
NTSTATUS
IoReportTargetDeviceChange(
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    IN PVOID NotificationStructure  // always begins with a PLUGPLAY_NOTIFICATION_HEADER
    );

typedef
VOID
(*PDEVICE_CHANGE_COMPLETE_CALLBACK)(
    IN PVOID Context
    );

NTKERNELAPI
VOID
IoInvalidateDeviceState(
    IN PDEVICE_OBJECT PhysicalDeviceObject
    );

#define IoAdjustPagingPathCount(_count_,_paging_) {     \
    if (_paging_) {                                     \
        InterlockedIncrement(_count_);                  \
    } else {                                            \
        InterlockedDecrement(_count_);                  \
    }                                                   \
}

NTKERNELAPI
NTSTATUS
IoReportTargetDeviceChangeAsynchronous(
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    IN PVOID NotificationStructure,  // always begins with a PLUGPLAY_NOTIFICATION_HEADER
    IN PDEVICE_CHANGE_COMPLETE_CALLBACK Callback,       OPTIONAL
    IN PVOID Context    OPTIONAL
    );
// end_wdm end_ntosp
//
// Device location interface declarations
//
typedef
NTSTATUS
(*PGET_LOCATION_STRING) (
    IN PVOID Context,
    OUT PWCHAR *LocationStrings
    );

typedef struct _PNP_LOCATION_INTERFACE {
    //
    // generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    //
    // interface specific entry
    //
    PGET_LOCATION_STRING GetLocationString;

} PNP_LOCATION_INTERFACE, *PPNP_LOCATION_INTERFACE;

//
// Resource arbiter declarations
//

typedef enum _ARBITER_ACTION {
    ArbiterActionTestAllocation,
    ArbiterActionRetestAllocation,
    ArbiterActionCommitAllocation,
    ArbiterActionRollbackAllocation,
    ArbiterActionQueryAllocatedResources,
    ArbiterActionWriteReservedResources,
    ArbiterActionQueryConflict,
    ArbiterActionQueryArbitrate,
    ArbiterActionAddReserved,
    ArbiterActionBootAllocation
} ARBITER_ACTION, *PARBITER_ACTION;

typedef struct _ARBITER_CONFLICT_INFO {
    //
    // The device object owning the device that is causing the conflict
    //
    PDEVICE_OBJECT OwningObject;

    //
    // The start of the conflicting range
    //
    ULONGLONG Start;

    //
    // The end of the conflicting range
    //
    ULONGLONG End;

} ARBITER_CONFLICT_INFO, *PARBITER_CONFLICT_INFO;

//
// The parameters for those actions
//

typedef struct _ARBITER_PARAMETERS {

    union {

        struct {

            //
            // Doubly linked list of ARBITER_LIST_ENTRY's
            //
            IN OUT PLIST_ENTRY ArbitrationList;

            //
            // The size of the AllocateFrom array
            //
            IN ULONG AllocateFromCount;

            //
            // Array of resource descriptors describing the resources available
            // to the arbiter for it to arbitrate
            //
            IN PCM_PARTIAL_RESOURCE_DESCRIPTOR AllocateFrom;

        } TestAllocation;

        struct {

            //
            // Doubly linked list of ARBITER_LIST_ENTRY's
            //
            IN OUT PLIST_ENTRY ArbitrationList;

            //
            // The size of the AllocateFrom array
            //
            IN ULONG AllocateFromCount;

            //
            // Array of resource descriptors describing the resources available
            // to the arbiter for it to arbitrate
            //
            IN PCM_PARTIAL_RESOURCE_DESCRIPTOR AllocateFrom;

        } RetestAllocation;

        struct {

            //
            // Doubly linked list of ARBITER_LIST_ENTRY's
            //
            IN OUT PLIST_ENTRY ArbitrationList;

        } BootAllocation;

        struct {

            //
            // The resources that are currently allocated
            //
            OUT PCM_PARTIAL_RESOURCE_LIST *AllocatedResources;

        } QueryAllocatedResources;

        struct {

            //
            // This is the device we are trying to find a conflict for
            //
            IN PDEVICE_OBJECT PhysicalDeviceObject;

            //
            // This is the resource to find the conflict for
            //
            IN PIO_RESOURCE_DESCRIPTOR ConflictingResource;

            //
            // Number of devices conflicting on the resource
            //
            OUT PULONG ConflictCount;

            //
            // Pointer to array describing the conflicting device objects and ranges
            //
            OUT PARBITER_CONFLICT_INFO *Conflicts;

        } QueryConflict;

        struct {

            //
            // Doubly linked list of ARBITER_LIST_ENTRY's - should have
            // only one entry
            //
            IN PLIST_ENTRY ArbitrationList;

        } QueryArbitrate;

        struct {

            //
            // Indicates the device whose resources are to be marked as reserved
            //
            PDEVICE_OBJECT ReserveDevice;

        } AddReserved;

    } Parameters;

} ARBITER_PARAMETERS, *PARBITER_PARAMETERS;



typedef enum _ARBITER_REQUEST_SOURCE {

    ArbiterRequestUndefined = -1,
    ArbiterRequestLegacyReported,   // IoReportResourceUsage
    ArbiterRequestHalReported,      // IoReportHalResourceUsage
    ArbiterRequestLegacyAssigned,   // IoAssignResources
    ArbiterRequestPnpDetected,      // IoReportResourceForDetection
    ArbiterRequestPnpEnumerated     // IRP_MN_QUERY_RESOURCE_REQUIREMENTS

} ARBITER_REQUEST_SOURCE;


typedef enum _ARBITER_RESULT {

    ArbiterResultUndefined = -1,
    ArbiterResultSuccess,
    ArbiterResultExternalConflict, // This indicates that the request can never be solved for devices in this list
    ArbiterResultNullRequest       // The request was for length zero and thus no translation should be attempted

} ARBITER_RESULT;

//
// ARBITER_FLAG_BOOT_CONFIG - this indicates that the request is for the
// resources assigned by the firmware/BIOS.  It should be succeeded even if
// it conflicts with another devices boot config.
//

#define ARBITER_FLAG_BOOT_CONFIG 0x00000001

// begin_ntosp

NTKERNELAPI
NTSTATUS
IoReportResourceForDetection(
    IN PDRIVER_OBJECT DriverObject,
    IN PCM_RESOURCE_LIST DriverList OPTIONAL,
    IN ULONG DriverListSize OPTIONAL,
    IN PDEVICE_OBJECT DeviceObject OPTIONAL,
    IN PCM_RESOURCE_LIST DeviceList OPTIONAL,
    IN ULONG DeviceListSize OPTIONAL,
    OUT PBOOLEAN ConflictDetected
    );

// end_ntosp

typedef struct _ARBITER_LIST_ENTRY {

    //
    // This is a doubly linked list of entries for easy sorting
    //
    LIST_ENTRY ListEntry;

    //
    // The number of alternative allocation
    //
    ULONG AlternativeCount;

    //
    // Pointer to an array of resource descriptors for the possible allocations
    //
    PIO_RESOURCE_DESCRIPTOR Alternatives;

    //
    // The device object of the device requesting these resources.
    //
    PDEVICE_OBJECT PhysicalDeviceObject;

    //
    // Indicates where the request came from
    //
    ARBITER_REQUEST_SOURCE RequestSource;

    //
    // Flags these indicate a variety of things (use ARBITER_FLAG_*)
    //
    ULONG Flags;

    //
    // Space to aid the arbiter in processing the list it is initialized to 0 when
    // the entry is created.  The system will not attempt to interpret it.
    //
    LONG_PTR WorkSpace;

    //
    // Interface Type, Slot Number and Bus Number from Resource Requirements list,
    // used only for reverse identification.
    //
    INTERFACE_TYPE InterfaceType;
    ULONG SlotNumber;
    ULONG BusNumber;

    //
    // A pointer to a descriptor to indicate the resource that was allocated.
    // This is allocated by the system and filled in by the arbiter in response to an
    // ArbiterActionTestAllocation.
    //
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Assignment;

    //
    // Pointer to the alternative that was chosen from to provide the assignment.
    // This is filled in by the arbiter in response to an ArbiterActionTestAllocation.
    //
    PIO_RESOURCE_DESCRIPTOR SelectedAlternative;

    //
    // The result of the operation
    // This is filled in by the arbiter in response to an ArbiterActionTestAllocation.
    //
    ARBITER_RESULT Result;

} ARBITER_LIST_ENTRY, *PARBITER_LIST_ENTRY;

//
// The arbiter's entry point
//

typedef
NTSTATUS
(*PARBITER_HANDLER) (
    IN PVOID Context,
    IN ARBITER_ACTION Action,
    IN OUT PARBITER_PARAMETERS Parameters
    );

//
// Arbiter interface
//

#define ARBITER_PARTIAL   0x00000001


typedef struct _ARBITER_INTERFACE {

    //
    // Generic interface header
    //
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    //
    // Entry point to the arbiter
    //
    PARBITER_HANDLER ArbiterHandler;

    //
    // Other information about the arbiter, use ARBITER_* flags
    //
    ULONG Flags;

} ARBITER_INTERFACE, *PARBITER_INTERFACE;

//
// The directions translation can take place in
//

typedef enum _RESOURCE_TRANSLATION_DIRECTION { // ntosp
    TranslateChildToParent,                    // ntosp
    TranslateParentToChild                     // ntosp
} RESOURCE_TRANSLATION_DIRECTION;              // ntosp

//
// Translation functions
//
// begin_ntosp

typedef
NTSTATUS
(*PTRANSLATE_RESOURCE_HANDLER)(
    IN PVOID Context,
    IN PCM_PARTIAL_RESOURCE_DESCRIPTOR Source,
    IN RESOURCE_TRANSLATION_DIRECTION Direction,
    IN ULONG AlternativesCount, OPTIONAL
    IN IO_RESOURCE_DESCRIPTOR Alternatives[], OPTIONAL
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    OUT PCM_PARTIAL_RESOURCE_DESCRIPTOR Target
);

typedef
NTSTATUS
(*PTRANSLATE_RESOURCE_REQUIREMENTS_HANDLER)(
    IN PVOID Context,
    IN PIO_RESOURCE_DESCRIPTOR Source,
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    OUT PULONG TargetCount,
    OUT PIO_RESOURCE_DESCRIPTOR *Target
);

//
// Translator Interface
//

typedef struct _TRANSLATOR_INTERFACE {
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    PTRANSLATE_RESOURCE_HANDLER TranslateResources;
    PTRANSLATE_RESOURCE_REQUIREMENTS_HANDLER TranslateResourceRequirements;
} TRANSLATOR_INTERFACE, *PTRANSLATOR_INTERFACE;

// end_ntddk end_ntosp

//
// Legacy Device Detection Handler
//

typedef
NTSTATUS
(*PLEGACY_DEVICE_DETECTION_HANDLER)(
    IN PVOID Context,
    IN INTERFACE_TYPE LegacyBusType,
    IN ULONG BusNumber,
    IN ULONG SlotNumber,
    OUT PDEVICE_OBJECT *PhysicalDeviceObject
);

//
// Legacy Device Detection Interface
//

typedef struct _LEGACY_DEVICE_DETECTION_INTERFACE {
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    PLEGACY_DEVICE_DETECTION_HANDLER LegacyDeviceDetection;
} LEGACY_DEVICE_DETECTION_INTERFACE, *PLEGACY_DEVICE_DETECTION_INTERFACE;

// end_nthal end_ntifs

// begin_wdm begin_ntddk begin_ntifs begin_nthal begin_ntosp

//
// Header structure for all Plug&Play notification events...
//

typedef struct _PLUGPLAY_NOTIFICATION_HEADER {
    USHORT Version; // presently at version 1.
    USHORT Size;    // size (in bytes) of header + event-specific data.
    GUID Event;
    //
    // Event-specific stuff starts here.
    //
} PLUGPLAY_NOTIFICATION_HEADER, *PPLUGPLAY_NOTIFICATION_HEADER;

//
// Notification structure for all EventCategoryHardwareProfileChange events...
//

typedef struct _HWPROFILE_CHANGE_NOTIFICATION {
    USHORT Version;
    USHORT Size;
    GUID Event;
    //
    // (No event-specific data)
    //
} HWPROFILE_CHANGE_NOTIFICATION, *PHWPROFILE_CHANGE_NOTIFICATION;


//
// Notification structure for all EventCategoryDeviceInterfaceChange events...
//

typedef struct _DEVICE_INTERFACE_CHANGE_NOTIFICATION {
    USHORT Version;
    USHORT Size;
    GUID Event;
    //
    // Event-specific data
    //
    GUID InterfaceClassGuid;
    PUNICODE_STRING SymbolicLinkName;
} DEVICE_INTERFACE_CHANGE_NOTIFICATION, *PDEVICE_INTERFACE_CHANGE_NOTIFICATION;


//
// Notification structures for EventCategoryTargetDeviceChange...
//

//
// The following structure is used for TargetDeviceQueryRemove,
// TargetDeviceRemoveCancelled, and TargetDeviceRemoveComplete:
//
typedef struct _TARGET_DEVICE_REMOVAL_NOTIFICATION {
    USHORT Version;
    USHORT Size;
    GUID Event;
    //
    // Event-specific data
    //
    PFILE_OBJECT FileObject;
} TARGET_DEVICE_REMOVAL_NOTIFICATION, *PTARGET_DEVICE_REMOVAL_NOTIFICATION;

//
// The following structure header is used for all other (i.e., 3rd-party)
// target device change events.  The structure accommodates both a
// variable-length binary data buffer, and a variable-length unicode text
// buffer.  The header must indicate where the text buffer begins, so that
// the data can be delivered in the appropriate format (ANSI or Unicode)
// to user-mode recipients (i.e., that have registered for handle-based
// notification via RegisterDeviceNotification).
//

typedef struct _TARGET_DEVICE_CUSTOM_NOTIFICATION {
    USHORT Version;
    USHORT Size;
    GUID Event;
    //
    // Event-specific data
    //
    PFILE_OBJECT FileObject;    // This field must be set to NULL by callers of
                                // IoReportTargetDeviceChange.  Clients that
                                // have registered for target device change
                                // notification on the affected PDO will be
                                // called with this field set to the file object
                                // they specified during registration.
                                //
    LONG NameBufferOffset;      // offset (in bytes) from beginning of
                                // CustomDataBuffer where text begins (-1 if none)
                                //
    UCHAR CustomDataBuffer[1];  // variable-length buffer, containing (optionally)
                                // a binary data at the start of the buffer,
                                // followed by an optional unicode text buffer
                                // (word-aligned).
                                //
} TARGET_DEVICE_CUSTOM_NOTIFICATION, *PTARGET_DEVICE_CUSTOM_NOTIFICATION;

// end_wdm end_ntddk end_ntifs end_nthal end_ntosp

NTSTATUS
PpSetCustomTargetEvent(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PKEVENT SyncEvent                           OPTIONAL,
    OUT PULONG Result                               OPTIONAL,
    IN  PDEVICE_CHANGE_COMPLETE_CALLBACK Callback   OPTIONAL,
    IN  PVOID Context                               OPTIONAL,
    IN  PTARGET_DEVICE_CUSTOM_NOTIFICATION NotificationStructure
    );

NTSTATUS
PpSetTargetDeviceRemove(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  BOOLEAN KernelInitiated,
    IN  BOOLEAN NoRestart,
    IN  BOOLEAN OnlyRestartRelations,
    IN  BOOLEAN DoEject,
    IN  ULONG Problem,
    IN  PKEVENT SyncEvent        OPTIONAL,
    OUT PULONG Result            OPTIONAL,
    OUT PPNP_VETO_TYPE VetoType  OPTIONAL,
    OUT PUNICODE_STRING VetoName OPTIONAL
    );

NTSTATUS
PpSetDeviceRemovalSafe(
    IN  PDEVICE_OBJECT DeviceObject,
    IN  PKEVENT SyncEvent           OPTIONAL,
    OUT PULONG Result               OPTIONAL
    );

NTSTATUS
PpNotifyUserModeRemovalSafe(
    IN  PDEVICE_OBJECT DeviceObject
    );

#define TDF_DEVICEEJECTABLE         0x00000001
#define TDF_NO_RESTART              0x00000002
#define TDF_KERNEL_INITIATED        0x00000004
//
// This flag is valid only if TDF_NO_RESTART is not set. If set, only relations
// are restarted. If not set, original device and relations are restarted.
//
#define TDF_ONLY_RESTART_RELATIONS  0x00000008  

NTSTATUS
PpSetDeviceClassChange(
    IN CONST GUID *EventGuid,
    IN CONST GUID *ClassGuid,
    IN PUNICODE_STRING SymbolicLinkName
    );

VOID
PpSetPlugPlayEvent(
    IN CONST GUID *EventGuid,
    IN PDEVICE_OBJECT DeviceObject
    );

NTSTATUS
PpInitializeNotification(
    VOID
    );

VOID
PpShutdownSystem (
    IN BOOLEAN Reboot,
    IN ULONG Phase,
    IN OUT PVOID *Context
    );

NTSTATUS
PpSetPowerEvent(
    IN   ULONG EventCode,
    IN   ULONG EventData,
    IN   PKEVENT CompletionEvent    OPTIONAL,
    OUT  PNTSTATUS CompletionStatus OPTIONAL,
    OUT  PPNP_VETO_TYPE VetoType    OPTIONAL,
    OUT  PUNICODE_STRING VetoName   OPTIONAL
    );

NTSTATUS
PpSetHwProfileChangeEvent(
    IN   CONST GUID *EventGuid,
    IN   PKEVENT CompletionEvent    OPTIONAL,
    OUT  PNTSTATUS CompletionStatus OPTIONAL,
    OUT  PPNP_VETO_TYPE VetoType    OPTIONAL,
    OUT  PUNICODE_STRING VetoName   OPTIONAL
    );

NTSTATUS
PpSetBlockedDriverEvent(
    IN   GUID CONST *BlockedDriverGuid
    );

NTSTATUS
PpSynchronizeDeviceEventQueue(
    VOID
    );

NTSTATUS
PpSetInvalidIDEvent(
    IN   PUNICODE_STRING ParentInstance
    );

NTSTATUS
PpSetPowerVetoEvent(
    IN  POWER_ACTION    VetoedPowerOperation,
    IN  PKEVENT         CompletionEvent         OPTIONAL,
    OUT PNTSTATUS       CompletionStatus        OPTIONAL,
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PNP_VETO_TYPE   VetoType,
    IN  PUNICODE_STRING VetoName                OPTIONAL
    );

NTSTATUS
PpPagePathAssign(
    IN PFILE_OBJECT FileObject
    );

NTSTATUS
PpPagePathRelease(
    IN PFILE_OBJECT FileObject
    );

//
// Entry point for USER to deliver notifications (public)
//

// begin_ntosp

NTKERNELAPI
ULONG
IoPnPDeliverServicePowerNotification(
    IN   POWER_ACTION           PowerOperation,
    IN   ULONG                  PowerNotificationCode,
    IN   ULONG                  PowerNotificationData,
    IN   BOOLEAN                Synchronous
    );

// end_ntosp

#endif // _PNP_

