/* Authoritative D3D12 vtable slot indices, derived directly from the SDK's own C
 * `*Vtbl` structs via offsetof. Compiled with CINTERFACE (which exposes those
 * structs); the main proxy TU uses the C++ interfaces, so the two can't share a
 * translation unit - hence this tiny companion. Using the header's own layout
 * removes any hand-counting error from the hooks. */
#define CINTERFACE
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <cstddef>

#define SLOT(V, M) (unsigned)(offsetof(V, M) / sizeof(void *))

extern "C" {
unsigned RGPU_SLOT_Device_CreateCommandQueue   = SLOT(ID3D12DeviceVtbl, CreateCommandQueue);
unsigned RGPU_SLOT_Device_CreateCommandList    = SLOT(ID3D12DeviceVtbl, CreateCommandList);
unsigned RGPU_SLOT_Device4_CreateCommandList1  = SLOT(ID3D12Device4Vtbl, CreateCommandList1);
unsigned RGPU_SLOT_Device9_CreateCommandQueue1 = SLOT(ID3D12Device9Vtbl, CreateCommandQueue1);
unsigned RGPU_SLOT_Device_CheckFeatureSupport  = SLOT(ID3D12DeviceVtbl, CheckFeatureSupport);
unsigned RGPU_SLOT_Queue_ExecuteCommandLists = SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists);
unsigned RGPU_SLOT_GCL_Close                 = SLOT(ID3D12GraphicsCommandListVtbl, Close);
unsigned RGPU_SLOT_GCL_DrawInstanced         = SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced);
unsigned RGPU_SLOT_GCL_DrawIndexedInstanced  = SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced);
unsigned RGPU_SLOT_GCL_Dispatch              = SLOT(ID3D12GraphicsCommandListVtbl, Dispatch);
unsigned RGPU_SLOT_GCL_CopyResource          = SLOT(ID3D12GraphicsCommandListVtbl, CopyResource);
unsigned RGPU_SLOT_GCL_ResourceBarrier       = SLOT(ID3D12GraphicsCommandListVtbl, ResourceBarrier);
unsigned RGPU_SLOT_GCL_OMSetRenderTargets    = SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets);
unsigned RGPU_SLOT_GCL_ClearRenderTargetView = SLOT(ID3D12GraphicsCommandListVtbl, ClearRenderTargetView);
unsigned RGPU_SLOT_GCL_ExecuteIndirect       = SLOT(ID3D12GraphicsCommandListVtbl, ExecuteIndirect);
unsigned RGPU_SLOT_GCL6_DispatchMesh         = SLOT(ID3D12GraphicsCommandList6Vtbl, DispatchMesh);   /* mesh shaders (Nanite) */
unsigned RGPU_SLOT_GCL7_Barrier              = SLOT(ID3D12GraphicsCommandList7Vtbl, Barrier);         /* enhanced barriers (UE5 5.x) */
unsigned RGPU_SLOT_Factory_CreateDevice      = SLOT(ID3D12DeviceFactoryVtbl, CreateDevice);
unsigned RGPU_SLOT_SDKConfig1_CreateDeviceFactory = SLOT(ID3D12SDKConfiguration1Vtbl, CreateDeviceFactory);
/* DXGI swap-chain creation. For D3D12 the swap chain's `pDevice` argument is the direct
 * command QUEUE the swap chain presents from - i.e. the game's real render queue. */
unsigned RGPU_SLOT_DXGIFactory_CreateSwapChain                = SLOT(IDXGIFactoryVtbl, CreateSwapChain);
unsigned RGPU_SLOT_DXGIFactory2_CreateSwapChainForHwnd        = SLOT(IDXGIFactory2Vtbl, CreateSwapChainForHwnd);
unsigned RGPU_SLOT_DXGIFactory2_CreateSwapChainForComposition = SLOT(IDXGIFactory2Vtbl, CreateSwapChainForComposition);
}
