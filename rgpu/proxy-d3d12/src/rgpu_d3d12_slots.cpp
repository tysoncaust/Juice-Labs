/* Authoritative D3D12 vtable slot indices, derived directly from the SDK's own C
 * `*Vtbl` structs via offsetof. Compiled with CINTERFACE (which exposes those
 * structs); the main proxy TU uses the C++ interfaces, so the two can't share a
 * translation unit - hence this tiny companion. Using the header's own layout
 * removes any hand-counting error from the hooks. */
#define CINTERFACE
#include <windows.h>
#include <d3d12.h>
#include <cstddef>

#define SLOT(V, M) (unsigned)(offsetof(V, M) / sizeof(void *))

extern "C" {
unsigned RGPU_SLOT_Device_CreateCommandQueue   = SLOT(ID3D12DeviceVtbl, CreateCommandQueue);
unsigned RGPU_SLOT_Device_CreateCommandList    = SLOT(ID3D12DeviceVtbl, CreateCommandList);
unsigned RGPU_SLOT_Device4_CreateCommandList1  = SLOT(ID3D12Device4Vtbl, CreateCommandList1);
unsigned RGPU_SLOT_Device9_CreateCommandQueue1 = SLOT(ID3D12Device9Vtbl, CreateCommandQueue1);
unsigned RGPU_SLOT_Queue_ExecuteCommandLists = SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists);
unsigned RGPU_SLOT_GCL_Close                 = SLOT(ID3D12GraphicsCommandListVtbl, Close);
unsigned RGPU_SLOT_GCL_DrawInstanced         = SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced);
unsigned RGPU_SLOT_GCL_DrawIndexedInstanced  = SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced);
unsigned RGPU_SLOT_GCL_Dispatch              = SLOT(ID3D12GraphicsCommandListVtbl, Dispatch);
unsigned RGPU_SLOT_GCL_CopyResource          = SLOT(ID3D12GraphicsCommandListVtbl, CopyResource);
unsigned RGPU_SLOT_GCL_ResourceBarrier       = SLOT(ID3D12GraphicsCommandListVtbl, ResourceBarrier);
unsigned RGPU_SLOT_GCL_OMSetRenderTargets    = SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets);
unsigned RGPU_SLOT_GCL_ClearRenderTargetView = SLOT(ID3D12GraphicsCommandListVtbl, ClearRenderTargetView);
unsigned RGPU_SLOT_Factory_CreateDevice      = SLOT(ID3D12DeviceFactoryVtbl, CreateDevice);
}
