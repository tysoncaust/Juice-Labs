/* rgpu d3d12 proxy DLL - D3D12 injection point + command-stream tee (Phase-1, steps 1-2).
 *
 * Built as `d3d12.dll` and placed next to a D3D12 game's .exe (e.g. Tokyo Xtreme
 * Racer, UE5). App-dir DLL precedence loads this before System32\d3d12.dll.
 *
 * STEP 1 (pass-through): exports the loader surface a D3D12 game imports and
 * forwards each to the real System32\d3d12.dll, preserving the game's Agility SDK
 * selection (the system loader still reads the EXE's D3D12SDKVersion/Path exports
 * and boots the game's own D3D12Core.dll - we never touch it).
 *
 * STEP 2 (command-stream tee): D3D12 has no immediate context; work is recorded
 * into command lists and submitted through command queues, so we intercept via
 * in-place VTABLE HOOKING (as PIX/RenderDoc do) rather than 100+-method wrappers.
 * This preserves COM identity perfectly - the object stays the real object, and
 * QI'd newer interface versions share the same patched vtable. One hook on
 * ID3D12Device::CreateCommandQueue bootstraps a hook on each queue's
 * ExecuteCommandLists (the submission boundary), which bootstraps hooks on the
 * shared ID3D12GraphicsCommandList recording slots (Draw, Dispatch, Barrier,
 * Clear, Copy, Close). Every hook records the call into the rgpu protocol then
 * calls the original, so the game runs unchanged while we tee its REAL per-frame
 * command stream and mark each ExecuteCommandLists submission. (Full remote replay
 * additionally needs the resource object-graph + handle translation - see README.) */
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include "../../proto/rgpu_cmds.h"

static HMODULE g_real = nullptr;

/* ---------------- logging ------------------------------------------------- */
static void rgpu_log(const char *fmt, ...) {
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (!n || n >= MAX_PATH - 24) return;
    std::strcat(dir, "rgpu_d3d12.log");
    FILE *f = std::fopen(dir, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); std::vfprintf(f, fmt, ap); va_end(ap);
    std::fprintf(f, "\n"); std::fclose(f);
}

/* ---------------- protocol tee (thread-safe, size-capped) ----------------- */
static CRITICAL_SECTION g_cs;
static unsigned char g_batch[4u * 1024 * 1024];
static rgpu_batch_writer g_bw;
static volatile LONG g_exec = 0, g_lists = 0, g_draws = 0, g_dispatch = 0,
                     g_barriers = 0, g_clears = 0, g_copies = 0, g_rts = 0;

static void tee(uint32_t op, uint32_t handle, const void *a, uint32_t n) {
    EnterCriticalSection(&g_cs);
    rgpu_bw_cmd(&g_bw, op, handle, a, n);   /* stops appending when the 4MB cap is hit */
    LeaveCriticalSection(&g_cs);
}
static inline uint32_t oid(const void *p) { return (uint32_t)(uintptr_t)p; }

/* ---------------- vtable hooking ------------------------------------------ */
/* ABI-stable vtable slot indices (D3D12 interface layout is frozen). Derived by
 * counting the inheritance chain from IUnknown:
 *   ID3D12Device : ID3D12Object : IUnknown  ->  IUnknown[0..2], ID3D12Object[3..6]
 *     (GetPrivateData,SetPrivateData,SetPrivateDataInterface,SetName), then
 *     GetNodeCount[7], CreateCommandQueue[8].
 *   ID3D12CommandQueue : ID3D12Pageable : ID3D12DeviceChild : ID3D12Object : IUnknown
 *     IUnknown[0..2], ID3D12Object[3..6], GetDevice[7], then UpdateTileMappings[8],
 *     CopyTileMappings[9], ExecuteCommandLists[10].
 *   ID3D12GraphicsCommandList : ID3D12CommandList : ID3D12DeviceChild : ID3D12Object
 *     IUnknown[0..2], ID3D12Object[3..6], GetDevice[7], GetType[8], then Close[9],
 *     Reset[10], ClearState[11], DrawInstanced[12], DrawIndexedInstanced[13],
 *     Dispatch[14], CopyBufferRegion[15], CopyTextureRegion[16], CopyResource[17],
 *     ... ResourceBarrier[26], ... OMSetRenderTargets[46], ClearDepthStencilView[47],
 *     ClearRenderTargetView[48]. */
/* Authoritative slot indices from rgpu_d3d12_slots.cpp (SDK C-vtable offsets). */
extern "C" {
extern unsigned RGPU_SLOT_Device_CreateCommandQueue, RGPU_SLOT_Queue_ExecuteCommandLists,
    RGPU_SLOT_GCL_Close, RGPU_SLOT_GCL_DrawInstanced, RGPU_SLOT_GCL_DrawIndexedInstanced,
    RGPU_SLOT_GCL_Dispatch, RGPU_SLOT_GCL_CopyResource, RGPU_SLOT_GCL_ResourceBarrier,
    RGPU_SLOT_GCL_OMSetRenderTargets, RGPU_SLOT_GCL_ClearRenderTargetView, RGPU_SLOT_Factory_CreateDevice,
    RGPU_SLOT_Device_CreateCommandList, RGPU_SLOT_Device4_CreateCommandList1, RGPU_SLOT_Device9_CreateCommandQueue1;
}

static bool patch_slot(void *obj, size_t index, void *hook, void **orig) {
    void **vtbl = *(void ***)obj;
    DWORD old;
    if (!VirtualProtect(&vtbl[index], sizeof(void *), PAGE_READWRITE, &old)) return false;
    *orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
    return vtbl[index] == hook;   /* confirm the write actually took */
}

/* Track the vtables we've already patched. The game can create objects from
 * more than one D3D12Core (a system-runtime probe device AND the Agility runtime
 * device via ID3D12DeviceFactory), which are DIFFERENT vtables. We re-arm when a
 * genuinely new queue/list vtable appears so the actively-rendering device's
 * objects are covered. */
static void *g_armed_queue_vtbl = nullptr;
static void *g_armed_list_vtbl  = nullptr;

typedef void    (STDMETHODCALLTYPE *fnExecuteCommandLists)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
typedef void    (STDMETHODCALLTYPE *fnDrawInstanced)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT);
typedef void    (STDMETHODCALLTYPE *fnDrawIndexedInstanced)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT);
typedef void    (STDMETHODCALLTYPE *fnDispatch)(ID3D12GraphicsCommandList *, UINT, UINT, UINT);
typedef void    (STDMETHODCALLTYPE *fnResourceBarrier)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
typedef void    (STDMETHODCALLTYPE *fnClearRTV)(ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT *, UINT, const D3D12_RECT *);
typedef void    (STDMETHODCALLTYPE *fnOMSetRenderTargets)(ID3D12GraphicsCommandList *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *);
typedef void    (STDMETHODCALLTYPE *fnCopyResource)(ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *);
typedef HRESULT (STDMETHODCALLTYPE *fnClose)(ID3D12GraphicsCommandList *);

static fnExecuteCommandLists  o_ExecuteCommandLists;
static fnDrawInstanced        o_DrawInstanced;
static fnDrawIndexedInstanced o_DrawIndexedInstanced;
static fnDispatch             o_Dispatch;
static fnResourceBarrier      o_ResourceBarrier;
static fnClearRTV             o_ClearRTV;
static fnOMSetRenderTargets   o_OMSetRenderTargets;
static fnCopyResource         o_CopyResource;
static fnClose                o_Close;

/* ---- ID3D12GraphicsCommandList recording hooks (tee + call original) ----- */
static void STDMETHODCALLTYPE h_DrawInstanced(ID3D12GraphicsCommandList *This, UINT vc, UINT ic, UINT sv, UINT si) {
    if (InterlockedIncrement(&g_draws) == 1) rgpu_log("FIRST DrawInstanced hooked call -> list recording hooks are LIVE");
    rgpu_args_draw a{vc, sv, ic, si}; tee(RGPU_CMD_DRAW, oid(This), &a, sizeof(a));
    o_DrawInstanced(This, vc, ic, sv, si);
}
static void STDMETHODCALLTYPE h_DrawIndexedInstanced(ID3D12GraphicsCommandList *This, UINT ic, UINT inst, UINT si, INT bv, UINT sinst) {
    InterlockedIncrement(&g_draws);
    rgpu_args_draw_indexed a{ic, si, bv, inst, sinst}; tee(RGPU_CMD_DRAW_INDEXED, oid(This), &a, sizeof(a));
    o_DrawIndexedInstanced(This, ic, inst, si, bv, sinst);
}
static void STDMETHODCALLTYPE h_Dispatch(ID3D12GraphicsCommandList *This, UINT x, UINT y, UINT z) {
    if (InterlockedIncrement(&g_dispatch) == 1) rgpu_log("FIRST Dispatch hooked call -> capturing TXR compute work");
    rgpu_args_dispatch a{x, y, z}; tee(RGPU_CMD_DISPATCH, oid(This), &a, sizeof(a));
    o_Dispatch(This, x, y, z);
}
static void STDMETHODCALLTYPE h_ResourceBarrier(ID3D12GraphicsCommandList *This, UINT n, const D3D12_RESOURCE_BARRIER *b) {
    LONG c = InterlockedIncrement(&g_barriers);
    if (c == 1 || (c % 20000) == 0) rgpu_log("ResourceBarrier hooked call #%ld -> capturing TXR command stream (barriers=%ld draws=%ld dispatch=%ld clears=%ld)", c, g_barriers, g_draws, g_dispatch, g_clears);
    tee(RGPU_CMD_RESOURCE_BARRIER, oid(This), &n, sizeof(n));
    o_ResourceBarrier(This, n, b);
}
static void STDMETHODCALLTYPE h_ClearRTV(ID3D12GraphicsCommandList *This, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT *c, UINT nr, const D3D12_RECT *r) {
    InterlockedIncrement(&g_clears);
    rgpu_args_clear_rtv a; std::memcpy(a.rgba, c, sizeof(a.rgba)); tee(RGPU_CMD_CLEAR_RTV, (uint32_t)h.ptr, &a, sizeof(a));
    o_ClearRTV(This, h, c, nr, r);
}
static void STDMETHODCALLTYPE h_OMSetRenderTargets(ID3D12GraphicsCommandList *This, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rt, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE *ds) {
    InterlockedIncrement(&g_rts);
    rgpu_args_set_render_targets a{n, (n && rt) ? (uint32_t)rt[0].ptr : 0u, ds ? (uint32_t)ds->ptr : 0u};
    tee(RGPU_CMD_SET_RENDER_TARGETS, oid(This), &a, sizeof(a));
    o_OMSetRenderTargets(This, n, rt, single, ds);
}
static void STDMETHODCALLTYPE h_CopyResource(ID3D12GraphicsCommandList *This, ID3D12Resource *dst, ID3D12Resource *src) {
    InterlockedIncrement(&g_copies);
    tee(RGPU_CMD_COPY_RESOURCE, oid(dst), nullptr, 0);
    o_CopyResource(This, dst, src);
}
static HRESULT STDMETHODCALLTYPE h_Close(ID3D12GraphicsCommandList *This) {
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) rgpu_log("FIRST Close hooked call -> list vtable patch is LIVE");
    return o_Close(This); /* Close finalizes a list; the tee to this point is a submittable unit */
}

/* Patch the shared ID3D12GraphicsCommandList vtable (caller owns the list ref). */
static void hook_gcl_vtable(ID3D12GraphicsCommandList *gl) {
    patch_slot(gl, RGPU_SLOT_GCL_DrawInstanced,         (void *)h_DrawInstanced,        (void **)&o_DrawInstanced);
    patch_slot(gl, RGPU_SLOT_GCL_DrawIndexedInstanced,  (void *)h_DrawIndexedInstanced, (void **)&o_DrawIndexedInstanced);
    patch_slot(gl, RGPU_SLOT_GCL_Dispatch,              (void *)h_Dispatch,             (void **)&o_Dispatch);
    patch_slot(gl, RGPU_SLOT_GCL_ResourceBarrier,       (void *)h_ResourceBarrier,      (void **)&o_ResourceBarrier);
    patch_slot(gl, RGPU_SLOT_GCL_ClearRenderTargetView, (void *)h_ClearRTV,             (void **)&o_ClearRTV);
    patch_slot(gl, RGPU_SLOT_GCL_OMSetRenderTargets,    (void *)h_OMSetRenderTargets,   (void **)&o_OMSetRenderTargets);
    patch_slot(gl, RGPU_SLOT_GCL_CopyResource,          (void *)h_CopyResource,         (void **)&o_CopyResource);
    patch_slot(gl, RGPU_SLOT_GCL_Close,                 (void *)h_Close,                (void **)&o_Close);
    rgpu_log("hooked ID3D12GraphicsCommandList recording slots (Draw/Dispatch/Barrier/Clear/OMSetRT/Copy/Close)");
}

/* ---- ID3D12CommandQueue::ExecuteCommandLists hook (submission boundary) --- */
static void STDMETHODCALLTYPE h_ExecuteCommandLists(ID3D12CommandQueue *This, UINT n, ID3D12CommandList *const *pp) {
    LONG e = InterlockedIncrement(&g_exec);
    InterlockedExchangeAdd(&g_lists, (LONG)n);
    uint32_t nn = n; tee(RGPU_CMD_EXECUTE_COMMAND_LISTS, 0, &nn, sizeof(nn));
    o_ExecuteCommandLists(This, n, pp);
    if (e == 1 || (e % 600) == 0) {
        EnterCriticalSection(&g_cs); uint32_t bytes = g_bw.len; LeaveCriticalSection(&g_cs);
        rgpu_log("submit#%ld lists=%ld | tee draws=%ld dispatch=%ld barriers=%ld clears=%ld setRT=%ld copies=%ld | batch=%u bytes",
                 e, g_lists, g_draws, g_dispatch, g_barriers, g_clears, g_rts, g_copies, bytes);
    }
}

/* Patch the ExecuteCommandLists / recording slots on a specific object's vtable,
 * re-arming only when a genuinely new vtable pointer appears (the game may use a
 * different D3D12Core than our throwaway probe). */
static void arm_queue_vtable(ID3D12CommandQueue *q, const char *src) {
    void *v = *(void **)q;
    if (v == g_armed_queue_vtbl) return;
    if (patch_slot(q, RGPU_SLOT_Queue_ExecuteCommandLists, (void *)h_ExecuteCommandLists, (void **)&o_ExecuteCommandLists)) {
        g_armed_queue_vtbl = v;
        rgpu_log("armed ExecuteCommandLists from %s (queue vtbl=%p)", src, v);
    }
}
static void arm_list_vtable(ID3D12GraphicsCommandList *gl, const char *src) {
    void *v = *(void **)gl;
    if (v == g_armed_list_vtbl) return;
    hook_gcl_vtable(gl); g_armed_list_vtbl = v;
    rgpu_log("armed command-list recording from %s (list vtbl=%p)", src, v);
}
static void arm_queue_from_any(void *obj) {
    ID3D12CommandQueue *q = nullptr;
    if (SUCCEEDED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&q)) && q) { arm_queue_vtable(q, "game queue"); q->Release(); }
}
static void arm_list_from_any(void *obj) {
    ID3D12GraphicsCommandList *gl = nullptr;
    if (SUCCEEDED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12GraphicsCommandList), (void **)&gl)) && gl) { arm_list_vtable(gl, "game list"); gl->Release(); }
}

/* Hooks on the DEVICE's object-creation methods, so we patch the vtable of the
 * game's ACTUAL queues/lists - covering the Agility CreateCommandQueue1 /
 * CreateCommandList1 paths UE5 uses (not just the base methods). */
typedef HRESULT (STDMETHODCALLTYPE *fnDevCCQ)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnDevCCQ1)(ID3D12Device9 *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnDevCCL)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnDevCCL1)(ID3D12Device4 *, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void **);
static fnDevCCQ  o_DevCCQ;  static fnDevCCQ1 o_DevCCQ1;  static fnDevCCL o_DevCCL;  static fnDevCCL1 o_DevCCL1;

static HRESULT STDMETHODCALLTYPE h_DevCCQ(ID3D12Device *This, const D3D12_COMMAND_QUEUE_DESC *d, REFIID riid, void **pp) {
    HRESULT hr = o_DevCCQ(This, d, riid, pp); if (SUCCEEDED(hr) && pp && *pp) arm_queue_from_any(*pp); return hr;
}
static HRESULT STDMETHODCALLTYPE h_DevCCQ1(ID3D12Device9 *This, const D3D12_COMMAND_QUEUE_DESC *d, REFIID cr, REFIID riid, void **pp) {
    HRESULT hr = o_DevCCQ1(This, d, cr, riid, pp); if (SUCCEEDED(hr) && pp && *pp) arm_queue_from_any(*pp); return hr;
}
static HRESULT STDMETHODCALLTYPE h_DevCCL(ID3D12Device *This, UINT nm, D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator *a, ID3D12PipelineState *ps, REFIID riid, void **pp) {
    HRESULT hr = o_DevCCL(This, nm, t, a, ps, riid, pp); if (SUCCEEDED(hr) && pp && *pp) arm_list_from_any(*pp); return hr;
}
static HRESULT STDMETHODCALLTYPE h_DevCCL1(ID3D12Device4 *This, UINT nm, D3D12_COMMAND_LIST_TYPE t, D3D12_COMMAND_LIST_FLAGS f, REFIID riid, void **pp) {
    HRESULT hr = o_DevCCL1(This, nm, t, f, riid, pp); if (SUCCEEDED(hr) && pp && *pp) arm_list_from_any(*pp); return hr;
}

static void *g_hooked_dev_vtbl = nullptr;
static void arm_from_device(ID3D12Device *dev, const char *which) {
    /* 1) hook the device's create methods so we patch the game's real objects */
    void *dv = *(void **)dev;
    if (dv != g_hooked_dev_vtbl) {
        g_hooked_dev_vtbl = dv;
        patch_slot(dev, RGPU_SLOT_Device_CreateCommandQueue, (void *)h_DevCCQ, (void **)&o_DevCCQ);
        patch_slot(dev, RGPU_SLOT_Device_CreateCommandList,  (void *)h_DevCCL, (void **)&o_DevCCL);
        ID3D12Device9 *d9 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device9), (void **)&d9)) && d9) {
            patch_slot(dev, RGPU_SLOT_Device9_CreateCommandQueue1, (void *)h_DevCCQ1, (void **)&o_DevCCQ1); d9->Release();
        }
        ID3D12Device4 *d4 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device4), (void **)&d4)) && d4) {
            patch_slot(dev, RGPU_SLOT_Device4_CreateCommandList1, (void *)h_DevCCL1, (void **)&o_DevCCL1); d4->Release();
        }
        rgpu_log("hooked %s device create methods (CCQ/CCL/CCQ1/CCL1) on dev vtbl=%p", which, dv);
    }
    /* 2) also arm immediately from throwaway objects (covers games using base methods) */
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *q = nullptr;
    if (SUCCEEDED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&q)) && q) { arm_queue_vtable(q, "throwaway"); q->Release(); }
    ID3D12CommandAllocator *alloc = nullptr; ID3D12GraphicsCommandList *list = nullptr;
    if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&alloc)) && alloc) {
        if (SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), (void **)&list)) && list) {
            arm_list_vtable(list, "throwaway"); list->Close(); list->Release();
        }
        alloc->Release();
    }
}

static bool adapter_is_warp(IUnknown *adapter) {
    if (!adapter) return false;   /* null == default hardware adapter */
    IDXGIAdapter *a = nullptr;
    if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter), (void **)&a)) && a) {
        DXGI_ADAPTER_DESC d{}; a->GetDesc(&d); a->Release();
        return d.VendorId == 0x1414;   /* Microsoft Basic Render Driver (WARP) */
    }
    return false;
}

/* ID3D12DeviceFactory::CreateDevice hook - modern UE5+Agility titles create their
 * REAL rendering device here (via D3D12GetInterface), not via the plain
 * D3D12CreateDevice export, so this is where we must arm the tee. */
typedef HRESULT (STDMETHODCALLTYPE *fnFactoryCreateDevice)(ID3D12DeviceFactory *, IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static fnFactoryCreateDevice o_FactoryCreateDevice;
static volatile LONG g_hooked_factory = 0;

static HRESULT STDMETHODCALLTYPE h_FactoryCreateDevice(ID3D12DeviceFactory *This, IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **pp) {
    HRESULT hr = o_FactoryCreateDevice(This, adapter, fl, riid, pp);
    bool warp = adapter_is_warp(adapter);
    rgpu_log("ID3D12DeviceFactory::CreateDevice fl=0x%X adapter=%s -> hr=0x%08lX device=%p",
             (unsigned)fl, warp ? "WARP" : "hardware", (unsigned long)hr, pp ? *pp : nullptr);
    if (SUCCEEDED(hr) && pp && *pp && !warp) arm_from_device((ID3D12Device *)*pp, "DeviceFactory");
    return hr;
}

/* ---------------- system d3d12.dll forwarding ----------------------------- */
static HMODULE real_d3d12() {
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n && n < MAX_PATH - 16) { std::strcat(path, "\\d3d12.dll"); g_real = LoadLibraryA(path); }
    }
    return g_real;
}
static FARPROC real(const char *name) { HMODULE m = real_d3d12(); return m ? GetProcAddress(m, name) : nullptr; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
        rgpu_bw_init(&g_bw, g_batch, sizeof(g_batch));
        char exe[MAX_PATH] = {0}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        rgpu_log("ATTACH host=\"%s\" (rgpu d3d12 proxy + command-stream tee)", exe);
        rgpu_log("slots: ExecCmdLists=%u Draw=%u DrawIdx=%u Dispatch=%u Barrier=%u ClearRTV=%u OMSetRT=%u Copy=%u Close=%u FactoryCreateDevice=%u",
                 RGPU_SLOT_Queue_ExecuteCommandLists, RGPU_SLOT_GCL_DrawInstanced, RGPU_SLOT_GCL_DrawIndexedInstanced,
                 RGPU_SLOT_GCL_Dispatch, RGPU_SLOT_GCL_ResourceBarrier, RGPU_SLOT_GCL_ClearRenderTargetView,
                 RGPU_SLOT_GCL_OMSetRenderTargets, RGPU_SLOT_GCL_CopyResource, RGPU_SLOT_GCL_Close, RGPU_SLOT_Factory_CreateDevice);
    }
    return TRUE;
}

/* Exports match d3d12.h's prototypes exactly (signatures come from the header);
 * each forwards to System32\d3d12.dll via real(). decltype(&Name) inside each
 * body yields the exact function-pointer type without a manual typedef. */
extern "C" {

__declspec(dllexport) HRESULT WINAPI D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **ppDevice) {
    auto fn = (decltype(&D3D12CreateDevice))real("D3D12CreateDevice");
    if (!fn) { rgpu_log("D3D12CreateDevice: real d3d12.dll MISSING"); return E_FAIL; }
    HRESULT hr = fn(pAdapter, fl, riid, ppDevice);
    bool warp = adapter_is_warp(pAdapter);
    rgpu_log("D3D12CreateDevice fl=0x%X adapter=%s -> hr=0x%08lX device=%p", (unsigned)fl,
             warp ? "WARP" : "hardware", (unsigned long)hr, ppDevice ? *ppDevice : nullptr);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice && !warp) arm_from_device((ID3D12Device *)*ppDevice, "D3D12CreateDevice");
    return hr;
}
__declspec(dllexport) HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **ppvDebug) {
    auto fn = (decltype(&D3D12GetDebugInterface))real("D3D12GetDebugInterface"); return fn ? fn(riid, ppvDebug) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *d, D3D_ROOT_SIGNATURE_VERSION v, ID3DBlob **b, ID3DBlob **e) {
    auto fn = (decltype(&D3D12SerializeRootSignature))real("D3D12SerializeRootSignature"); return fn ? fn(d, v, b, e) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d, ID3DBlob **b, ID3DBlob **e) {
    auto fn = (decltype(&D3D12SerializeVersionedRootSignature))real("D3D12SerializeVersionedRootSignature"); return fn ? fn(d, b, e) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12CreateRootSignatureDeserializer(LPCVOID p, SIZE_T n, REFIID riid, void **pp) {
    auto fn = (decltype(&D3D12CreateRootSignatureDeserializer))real("D3D12CreateRootSignatureDeserializer"); return fn ? fn(p, n, riid, pp) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(LPCVOID p, SIZE_T n, REFIID riid, void **pp) {
    auto fn = (decltype(&D3D12CreateVersionedRootSignatureDeserializer))real("D3D12CreateVersionedRootSignatureDeserializer"); return fn ? fn(p, n, riid, pp) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT nf, const IID *iids, void *cfg, UINT *sz) {
    auto fn = (decltype(&D3D12EnableExperimentalFeatures))real("D3D12EnableExperimentalFeatures"); return fn ? fn(nf, iids, cfg, sz) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12GetInterface(REFCLSID rclsid, REFIID riid, void **ppvDebug) {
    typedef HRESULT(WINAPI *fn_t)(REFCLSID, REFIID, void **);
    fn_t fn = (fn_t)real("D3D12GetInterface");
    if (!fn) return E_NOTIMPL;
    HRESULT hr = fn(rclsid, riid, ppvDebug);
    if (SUCCEEDED(hr) && ppvDebug && *ppvDebug && riid == __uuidof(ID3D12DeviceFactory)) {
        if (InterlockedCompareExchange(&g_hooked_factory, 1, 0) == 0 &&
            patch_slot(*ppvDebug, RGPU_SLOT_Factory_CreateDevice, (void *)h_FactoryCreateDevice, (void **)&o_FactoryCreateDevice))
            rgpu_log("hooked ID3D12DeviceFactory::CreateDevice (Agility real-device path)");
    }
    return hr;
}

/* Optional same-machine observation of the live tee tallies. */
__declspec(dllexport) void WINAPI rgpu_d3d12_stats(long *execs, long *lists, long *draws, long *dispatch,
                                                   long *barriers, long *clears, long *copies, unsigned *batchBytes) {
    if (execs) *execs = g_exec; if (lists) *lists = g_lists; if (draws) *draws = g_draws;
    if (dispatch) *dispatch = g_dispatch; if (barriers) *barriers = g_barriers;
    if (clears) *clears = g_clears; if (copies) *copies = g_copies;
    if (batchBytes) { EnterCriticalSection(&g_cs); *batchBytes = g_bw.len; LeaveCriticalSection(&g_cs); }
}

} /* extern "C" */
