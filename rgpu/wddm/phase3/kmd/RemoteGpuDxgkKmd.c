#include <stddef.h>
#include <string.h>
#include <ntddk.h>
#ifndef FAR
#define FAR
#endif
#include <windef.h>
#include <winerror.h>
#include <wingdi.h>
#include <winddi.h>
#include <ntddvdeo.h>
#include <d3dkmddi.h>
#include <d3dkmthk.h>
#include <dispmprt.h>

#define RGPU_DXGK_POOL_TAG 'gDxR'
#define RGPU_DXGK_TARGET_WDK "10.0.28000"

// Build-only render-miniport scaffold. It deliberately exposes zero display
// children and fails all resource/submission paths until the memory, scheduling
// and remote-completion model is implemented and tested in an isolated system.

typedef struct _RGPU_DXGK_ADAPTER {
    PDEVICE_OBJECT PhysicalDeviceObject;
    DXGKRNL_INTERFACE DxgkInterface;
    ULONG Started;
    ULONG DeviceLost;
    ULONG64 CompletedFence;
} RGPU_DXGK_ADAPTER, *PRGPU_DXGK_ADAPTER;

DRIVER_INITIALIZE DriverEntry;
DXGKDDI_ADD_DEVICE RgpuDxgkAddDevice;
DXGKDDI_START_DEVICE RgpuDxgkStartDevice;
DXGKDDI_STOP_DEVICE RgpuDxgkStopDevice;
DXGKDDI_REMOVE_DEVICE RgpuDxgkRemoveDevice;
DXGKDDI_DISPATCH_IO_REQUEST RgpuDxgkDispatchIoRequest;
DXGKDDI_INTERRUPT_ROUTINE RgpuDxgkInterruptRoutine;
DXGKDDI_DPC_ROUTINE RgpuDxgkDpcRoutine;
DXGKDDI_QUERY_CHILD_RELATIONS RgpuDxgkQueryChildRelations;
DXGKDDI_QUERY_CHILD_STATUS RgpuDxgkQueryChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR RgpuDxgkQueryDeviceDescriptor;
DXGKDDI_SET_POWER_STATE RgpuDxgkSetPowerState;
DXGKDDI_RESET_DEVICE RgpuDxgkResetDevice;
DXGKDDI_UNLOAD RgpuDxgkUnload;
DXGKDDI_QUERYADAPTERINFO RgpuDxgkQueryAdapterInfo;
DXGKDDI_CREATEDEVICE RgpuDxgkCreateDevice;
DXGKDDI_DESTROYDEVICE RgpuDxgkDestroyDevice;
DXGKDDI_CREATECONTEXT RgpuDxgkCreateContext;
DXGKDDI_DESTROYCONTEXT RgpuDxgkDestroyContext;
DXGKDDI_CREATEALLOCATION RgpuDxgkCreateAllocation;
DXGKDDI_DESTROYALLOCATION RgpuDxgkDestroyAllocation;
DXGKDDI_OPENALLOCATIONINFO RgpuDxgkOpenAllocation;
DXGKDDI_CLOSEALLOCATION RgpuDxgkCloseAllocation;
DXGKDDI_SUBMITCOMMAND RgpuDxgkSubmitCommand;
DXGKDDI_PREEMPTCOMMAND RgpuDxgkPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE RgpuDxgkQueryCurrentFence;
DXGKDDI_BUILDPAGINGBUFFER RgpuDxgkBuildPagingBuffer;
DXGKDDI_RESETFROMTIMEOUT RgpuDxgkResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT RgpuDxgkRestartFromTimeout;

NTSTATUS RgpuDxgkAddDevice(
    IN_CONST_PDEVICE_OBJECT PhysicalDeviceObject,
    OUT_PPVOID MiniportDeviceContext)
{
    PRGPU_DXGK_ADAPTER adapter;
    if (PhysicalDeviceObject == NULL || MiniportDeviceContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter = (PRGPU_DXGK_ADAPTER)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*adapter), RGPU_DXGK_POOL_TAG);
    if (adapter == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(adapter, sizeof(*adapter));
    adapter->PhysicalDeviceObject = PhysicalDeviceObject;
    *MiniportDeviceContext = adapter;
    return STATUS_SUCCESS;
}

NTSTATUS RgpuDxgkStartDevice(
    IN_CONST_PVOID MiniportDeviceContext,
    IN_PDXGK_START_INFO DxgkStartInfo,
    IN_PDXGKRNL_INTERFACE DxgkInterface,
    OUT_PULONG NumberOfVideoPresentSources,
    OUT_PULONG NumberOfChildren)
{
    PRGPU_DXGK_ADAPTER adapter = (PRGPU_DXGK_ADAPTER)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(DxgkStartInfo);
    if (adapter == NULL || DxgkInterface == NULL ||
        NumberOfVideoPresentSources == NULL || NumberOfChildren == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    adapter->DxgkInterface = *DxgkInterface;
    adapter->Started = 1;
    adapter->DeviceLost = 0;
    adapter->CompletedFence = 0;
    *NumberOfVideoPresentSources = 0;
    *NumberOfChildren = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RgpuDxgkStopDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    PRGPU_DXGK_ADAPTER adapter = (PRGPU_DXGK_ADAPTER)MiniportDeviceContext;
    if (adapter == NULL) return STATUS_INVALID_PARAMETER;
    adapter->Started = 0;
    return STATUS_SUCCESS;
}

NTSTATUS RgpuDxgkRemoveDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    if (MiniportDeviceContext != NULL) {
        ExFreePoolWithTag((PVOID)MiniportDeviceContext, RGPU_DXGK_POOL_TAG);
    }
    return STATUS_SUCCESS;
}

NTSTATUS RgpuDxgkDispatchIoRequest(
    IN_CONST_PVOID MiniportDeviceContext,
    IN_ULONG VidPnSourceId,
    IN_PVIDEO_REQUEST_PACKET VideoRequestPacket)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(VideoRequestPacket);
    return STATUS_NOT_SUPPORTED;
}

BOOLEAN RgpuDxgkInterruptRoutine(
    IN_CONST_PVOID MiniportDeviceContext,
    IN_ULONG MessageNumber)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(MessageNumber);
    return FALSE;
}

VOID RgpuDxgkDpcRoutine(IN_CONST_PVOID MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
}

NTSTATUS RgpuDxgkQueryChildRelations(
    IN_CONST_PVOID MiniportDeviceContext,
    _Inout_updates_bytes_(ChildRelationsSize) PDXGK_CHILD_DESCRIPTOR ChildRelations,
    _In_ ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    if (ChildRelations != NULL && ChildRelationsSize != 0) {
        RtlZeroMemory(ChildRelations, ChildRelationsSize);
    }
    return STATUS_SUCCESS;
}

NTSTATUS RgpuDxgkQueryChildStatus(
    IN_CONST_PVOID MiniportDeviceContext,
    INOUT_PDXGK_CHILD_STATUS ChildStatus,
    IN_BOOLEAN NonDestructiveOnly)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildStatus);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS RgpuDxgkQueryDeviceDescriptor(
    IN_CONST_PVOID MiniportDeviceContext,
    IN_ULONG ChildUid,
    INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildUid);
    UNREFERENCED_PARAMETER(DeviceDescriptor);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS RgpuDxgkSetPowerState(
    IN_CONST_PVOID MiniportDeviceContext,
    IN_ULONG DeviceUid,
    IN_DEVICE_POWER_STATE DevicePowerState,
    IN_POWER_ACTION ActionType)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceUid);
    UNREFERENCED_PARAMETER(DevicePowerState);
    UNREFERENCED_PARAMETER(ActionType);
    return STATUS_SUCCESS;
}

VOID RgpuDxgkResetDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    PRGPU_DXGK_ADAPTER adapter = (PRGPU_DXGK_ADAPTER)MiniportDeviceContext;
    if (adapter != NULL) {
        adapter->DeviceLost = 1;
    }
}

VOID RgpuDxgkUnload(VOID)
{
}

NTSTATUS APIENTRY RgpuDxgkQueryAdapterInfo(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_QUERYADAPTERINFO pQueryAdapterInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pQueryAdapterInfo);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY RgpuDxgkCreateDevice(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_CREATEDEVICE pCreateDevice)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCreateDevice);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY RgpuDxgkDestroyDevice(IN_CONST_HANDLE hDevice)
{
    UNREFERENCED_PARAMETER(hDevice);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY RgpuDxgkCreateContext(
    IN_CONST_HANDLE hDevice,
    INOUT_PDXGKARG_CREATECONTEXT pCreateContext)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCreateContext);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY RgpuDxgkDestroyContext(IN_CONST_HANDLE hContext)
{
    UNREFERENCED_PARAMETER(hContext);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY RgpuDxgkCreateAllocation(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_CREATEALLOCATION pCreateAllocation)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCreateAllocation);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY RgpuDxgkDestroyAllocation(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_DESTROYALLOCATION pDestroyAllocation)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pDestroyAllocation);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY RgpuDxgkOpenAllocation(
    IN_CONST_HANDLE hDevice,
    IN_CONST_PDXGKARG_OPENALLOCATION pOpenAllocation)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pOpenAllocation);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY RgpuDxgkCloseAllocation(
    IN_CONST_HANDLE hDevice,
    IN_CONST_PDXGKARG_CLOSEALLOCATION pCloseAllocation)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCloseAllocation);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY RgpuDxgkSubmitCommand(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_SUBMITCOMMAND pSubmitCommand)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSubmitCommand);
    return STATUS_DEVICE_NOT_READY;
}

NTSTATUS APIENTRY RgpuDxgkPreemptCommand(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_PREEMPTCOMMAND pPreemptCommand)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pPreemptCommand);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY RgpuDxgkQueryCurrentFence(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_QUERYCURRENTFENCE pCurrentFence)
{
    PRGPU_DXGK_ADAPTER adapter = (PRGPU_DXGK_ADAPTER)hAdapter;
    if (adapter == NULL || pCurrentFence == NULL) return STATUS_INVALID_PARAMETER;
    pCurrentFence->CurrentFence = (UINT)adapter->CompletedFence;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY RgpuDxgkBuildPagingBuffer(
    IN_CONST_HANDLE hAdapter,
    IN_PDXGKARG_BUILDPAGINGBUFFER pBuildPagingBuffer)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pBuildPagingBuffer);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY CALLBACK RgpuDxgkResetFromTimeout(IN_CONST_HANDLE hAdapter)
{
    PRGPU_DXGK_ADAPTER adapter = (PRGPU_DXGK_ADAPTER)hAdapter;
    if (adapter == NULL) return STATUS_INVALID_PARAMETER;
    adapter->DeviceLost = 1;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY CALLBACK RgpuDxgkRestartFromTimeout(IN_CONST_HANDLE hAdapter)
{
    PRGPU_DXGK_ADAPTER adapter = (PRGPU_DXGK_ADAPTER)hAdapter;
    if (adapter == NULL) return STATUS_INVALID_PARAMETER;
    adapter->DeviceLost = 0;
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA initialization;
    RtlZeroMemory(&initialization, sizeof(initialization));
    initialization.Version = DXGKDDI_INTERFACE_VERSION;
    initialization.DxgkDdiAddDevice = RgpuDxgkAddDevice;
    initialization.DxgkDdiStartDevice = RgpuDxgkStartDevice;
    initialization.DxgkDdiStopDevice = RgpuDxgkStopDevice;
    initialization.DxgkDdiRemoveDevice = RgpuDxgkRemoveDevice;
    initialization.DxgkDdiDispatchIoRequest = RgpuDxgkDispatchIoRequest;
    initialization.DxgkDdiInterruptRoutine = RgpuDxgkInterruptRoutine;
    initialization.DxgkDdiDpcRoutine = RgpuDxgkDpcRoutine;
    initialization.DxgkDdiQueryChildRelations = RgpuDxgkQueryChildRelations;
    initialization.DxgkDdiQueryChildStatus = RgpuDxgkQueryChildStatus;
    initialization.DxgkDdiQueryDeviceDescriptor = RgpuDxgkQueryDeviceDescriptor;
    initialization.DxgkDdiSetPowerState = RgpuDxgkSetPowerState;
    initialization.DxgkDdiResetDevice = RgpuDxgkResetDevice;
    initialization.DxgkDdiUnload = RgpuDxgkUnload;
    initialization.DxgkDdiQueryAdapterInfo = RgpuDxgkQueryAdapterInfo;
    initialization.DxgkDdiCreateDevice = RgpuDxgkCreateDevice;
    initialization.DxgkDdiDestroyDevice = RgpuDxgkDestroyDevice;
    initialization.DxgkDdiCreateContext = RgpuDxgkCreateContext;
    initialization.DxgkDdiDestroyContext = RgpuDxgkDestroyContext;
    initialization.DxgkDdiCreateAllocation = RgpuDxgkCreateAllocation;
    initialization.DxgkDdiDestroyAllocation = RgpuDxgkDestroyAllocation;
    initialization.DxgkDdiOpenAllocation = RgpuDxgkOpenAllocation;
    initialization.DxgkDdiCloseAllocation = RgpuDxgkCloseAllocation;
    initialization.DxgkDdiSubmitCommand = RgpuDxgkSubmitCommand;
    initialization.DxgkDdiPreemptCommand = RgpuDxgkPreemptCommand;
    initialization.DxgkDdiQueryCurrentFence = RgpuDxgkQueryCurrentFence;
    initialization.DxgkDdiBuildPagingBuffer = RgpuDxgkBuildPagingBuffer;
    initialization.DxgkDdiResetFromTimeout = RgpuDxgkResetFromTimeout;
    initialization.DxgkDdiRestartFromTimeout = RgpuDxgkRestartFromTimeout;
    return DxgkInitialize(DriverObject, RegistryPath, &initialization);
}
