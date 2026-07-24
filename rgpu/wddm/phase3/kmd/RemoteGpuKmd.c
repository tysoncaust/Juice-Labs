#include <ntddk.h>

#define RGPU_DEVICE_NAME L"\\Device\\RemoteGpuKmd"
#define RGPU_DOS_DEVICE_NAME L"\\DosDevices\\RemoteGpuKmd"
#define IOCTL_RGPU_QUERY_ABI CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)

#define RGPU_KMD_MAGIC 0x33444D4Bu /* KMD3 */
#define RGPU_KMD_VERSION 2u
#define RGPU_CONTROL_CAPACITY 256u
#define RGPU_CLIENT_CHANNELS 16u
#define RGPU_CLIENT_COMPLETION_CAPACITY 128u
#define RGPU_BULK_ARENA_BYTES (8u * 1024u * 1024u)

typedef struct _RGPU_KMD_ABI {
    ULONG Magic;
    ULONG Version;
    ULONG StructureBytes;
    ULONG ControlQueueCapacity;
    ULONG ClientChannelCount;
    ULONG ClientCompletionCapacity;
    ULONG BulkArenaBytes;
    ULONG ConnectionGenerationSupported;
    ULONG AsyncFenceCompletionSupported;
    ULONG ProcessOwnershipEnforced;
    ULONG NetworkOperationsInKernel;
    ULONG BlockingNetworkWaitsInCallbacks;
} RGPU_KMD_ABI;

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD RgpuUnload;
_Dispatch_type_(IRP_MJ_CREATE) DRIVER_DISPATCH RgpuCreateClose;
_Dispatch_type_(IRP_MJ_CLOSE) DRIVER_DISPATCH RgpuCreateClose;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH RgpuDeviceControl;

static NTSTATUS CompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS RgpuCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS RgpuDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    ULONG code;
    UNREFERENCED_PARAMETER(DeviceObject);

    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    if (code != IOCTL_RGPU_QUERY_ABI) {
        return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(RGPU_KMD_ABI)) {
        return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, sizeof(RGPU_KMD_ABI));
    }

    {
        RGPU_KMD_ABI* abi = (RGPU_KMD_ABI*)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(abi, sizeof(*abi));
        abi->Magic = RGPU_KMD_MAGIC;
        abi->Version = RGPU_KMD_VERSION;
        abi->StructureBytes = sizeof(*abi);
        abi->ControlQueueCapacity = RGPU_CONTROL_CAPACITY;
        abi->ClientChannelCount = RGPU_CLIENT_CHANNELS;
        abi->ClientCompletionCapacity = RGPU_CLIENT_COMPLETION_CAPACITY;
        abi->BulkArenaBytes = RGPU_BULK_ARENA_BYTES;
        abi->ConnectionGenerationSupported = 1;
        abi->AsyncFenceCompletionSupported = 1;
        abi->ProcessOwnershipEnforced = 1;
        abi->NetworkOperationsInKernel = 0;
        abi->BlockingNetworkWaitsInCallbacks = 0;
    }
    return CompleteIrp(Irp, STATUS_SUCCESS, sizeof(RGPU_KMD_ABI));
}

VOID RgpuUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos_name;
    RtlInitUnicodeString(&dos_name, RGPU_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&dos_name);
    if (DriverObject->DeviceObject != NULL) {
        IoDeleteDevice(DriverObject->DeviceObject);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING device_name;
    UNICODE_STRING dos_name;
    PDEVICE_OBJECT device_object = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&device_name, RGPU_DEVICE_NAME);
    RtlInitUnicodeString(&dos_name, RGPU_DOS_DEVICE_NAME);

    status = IoCreateDevice(DriverObject, 0, &device_name, FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &device_object);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = IoCreateSymbolicLink(&dos_name, &device_name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device_object);
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = RgpuCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = RgpuCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RgpuDeviceControl;
    DriverObject->DriverUnload = RgpuUnload;
    device_object->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
