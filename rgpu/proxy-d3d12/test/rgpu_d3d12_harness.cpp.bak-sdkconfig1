/* rgpu d3d12 command-stream tee harness - proves the vtable-hook tee captures a
 * real D3D12 command stream, in a process we fully control (no game-specific
 * anti-tamper/CFG). Loads rgpu_d3d12.dll, creates a device via its D3D12CreateDevice
 * export (which arms the tee), builds a command list that clears a render target
 * with barriers + a draw, submits it via ExecuteCommandLists, and checks the
 * proxy's stats: submissions, barriers, clears, and draws must all be tee'd. */
#include <windows.h>
#include <d3d12.h>
#include <cstdio>

typedef HRESULT (WINAPI *pCreate)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef void    (WINAPI *pStats)(long *, long *, long *, long *, long *, long *, long *, unsigned *);

int main() {
    std::printf("rgpu d3d12 command-stream tee harness\n-------------------------------------\n");
    HMODULE dll = LoadLibraryA("rgpu_d3d12.dll");
    if (!dll) { std::printf("  FAIL: load rgpu_d3d12.dll (err %lu)\n", GetLastError()); return 1; }
    pCreate CreateDevice = (pCreate)GetProcAddress(dll, "D3D12CreateDevice");
    pStats  Stats        = (pStats)GetProcAddress(dll, "rgpu_d3d12_stats");
    if (!CreateDevice || !Stats) { std::printf("  FAIL: proxy missing exports\n"); return 1; }

    ID3D12Device *dev = nullptr;
    HRESULT hr = CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev);
    if (FAILED(hr) || !dev) { std::printf("  FAIL: D3D12CreateDevice 0x%08lX\n", (unsigned long)hr); return 1; }
    std::printf("  device created via proxy (tee armed)\n");

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr; dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue);
    ID3D12CommandAllocator *alloc = nullptr; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&alloc);
    ID3D12GraphicsCommandList *list = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&list);
    if (!queue || !alloc || !list) { std::printf("  FAIL: queue/allocator/list\n"); return 1; }

    /* a real render target + RTV so the clear/barriers form a valid, submittable list */
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = 64; rd.Height = 64;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void **)&rt);
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ID3D12DescriptorHeap *rtvHeap = nullptr; dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&rtvHeap);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);

    /* record a valid clear-with-barriers list -> tests ResourceBarrier/OMSetRT/Clear/Execute */
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float color[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    list->ClearRenderTargetView(rtv, color, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    if (SUCCEEDED(list->Close())) {
        ID3D12CommandList *lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        ID3D12Fence *fence = nullptr; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&fence);
        queue->Signal(fence, 1);
        if (fence->GetCompletedValue() < 1) {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 2000); CloseHandle(ev);
        }
        fence->Release();
        std::printf("  submitted a valid clear list via ExecuteCommandLists\n");
    } else std::printf("  (clear list Close failed)\n");

    /* a second list exercising the draw hook (invalid without a PSO, but the vtable call still fires our hook) */
    alloc->Reset(); list->Reset(alloc, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    list->Close();

    long execs=0, lists_n=0, draws=0, dispatch=0, barriers=0, clears=0, copies=0; unsigned bytes=0;
    Stats(&execs, &lists_n, &draws, &dispatch, &barriers, &clears, &copies, &bytes);
    std::printf("  tee stats: execs=%ld lists=%ld draws=%ld barriers=%ld clears=%ld copies=%ld | batch=%u bytes\n",
                execs, lists_n, draws, barriers, clears, copies, bytes);

    int ok = execs >= 1 && barriers >= 1 && clears >= 1 && draws >= 1 && bytes > 0;
    std::printf("-------------------------------------\n");
    std::printf(ok ? "RESULT: D3D12 command stream captured (ExecuteCommandLists + barrier + clear + draw tee'd)\n"
                   : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
