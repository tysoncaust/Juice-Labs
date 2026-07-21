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
#include <tlhelp32.h>
#include <psapi.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <cstdlib>
#include "rgpu_inlinehook.h"
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

/* Full COM wrapper object-graph (device/queue/list). Included here so its methods
 * can use the tee (tee/oid/counters/rgpu_log) defined above. Retained as the protocol-
 * serialization reference + harness acceptance; the LIVE injection path uses shadow-
 * vtable in-place patching (below), which preserves the runtime's concrete object. */
#include "rgpu_d3d12_wrappers.h"

/* In-place interceptors (defined with the shadow-vtable machinery below). */
static void shadow_patch_device(void *devPtr);
static void shadow_patch_queue(void *q);
static void shadow_patch_list(void *l);

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
    RGPU_SLOT_Device_CreateCommandList, RGPU_SLOT_Device4_CreateCommandList1, RGPU_SLOT_Device9_CreateCommandQueue1,
    RGPU_SLOT_GCL_ExecuteIndirect, RGPU_SLOT_GCL6_DispatchMesh, RGPU_SLOT_GCL7_Barrier,
    RGPU_SLOT_Device_CheckFeatureSupport, RGPU_SLOT_SDKConfig1_CreateDeviceFactory;
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

/* ---- newer UE5 5.x methods: enhanced barriers, mesh dispatch, indirect --- */
typedef void (STDMETHODCALLTYPE *fnBarrier)(ID3D12GraphicsCommandList7 *, UINT32, const void *);
typedef void (STDMETHODCALLTYPE *fnDispatchMesh)(ID3D12GraphicsCommandList6 *, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *fnExecuteIndirect)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64);
static fnBarrier o_Barrier; static fnDispatchMesh o_DispatchMesh; static fnExecuteIndirect o_ExecuteIndirect;

static void STDMETHODCALLTYPE h_Barrier(ID3D12GraphicsCommandList7 *This, UINT32 numGroups, const void *groups) {
    LONG c = InterlockedIncrement(&g_barriers);
    if (c == 1) rgpu_log("FIRST Barrier (enhanced/GCL7) hooked call -> capturing TXR's real barrier stream");
    uint32_t n = numGroups; tee(RGPU_CMD_RESOURCE_BARRIER, oid(This), &n, sizeof(n));
    o_Barrier(This, numGroups, groups);
}
static void STDMETHODCALLTYPE h_DispatchMesh(ID3D12GraphicsCommandList6 *This, UINT x, UINT y, UINT z) {
    if (InterlockedIncrement(&g_dispatch) == 1) rgpu_log("FIRST DispatchMesh (GCL6) hooked call -> capturing Nanite mesh work");
    rgpu_args_dispatch a{x, y, z}; tee(RGPU_CMD_DISPATCH, oid(This), &a, sizeof(a));
    o_DispatchMesh(This, x, y, z);
}
static void STDMETHODCALLTYPE h_ExecuteIndirect(ID3D12GraphicsCommandList *This, ID3D12CommandSignature *sig, UINT maxCount, ID3D12Resource *args, UINT64 argOff, ID3D12Resource *cnt, UINT64 cntOff) {
    if (InterlockedIncrement(&g_draws) == 1) rgpu_log("FIRST ExecuteIndirect hooked call -> capturing GPU-driven draws");
    uint32_t m = maxCount; tee(RGPU_CMD_DRAW, oid(This), &m, sizeof(m));
    o_ExecuteIndirect(This, sig, maxCount, args, argOff, cnt, cntOff);
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
    /* Shadow-patch every submitted list in place: they are real objects reachable only
     * here when the queue came from DXGI (we never saw their creation). Their NEXT Reset
     * re-applies the shadow, so recording from the following frame onward is captured. */
    for (UINT i = 0; i < n; i++) if (pp && pp[i]) shadow_patch_list(pp[i]);
    o_ExecuteCommandLists(This, n, pp);
    if (e == 1 || (e % 600) == 0) {
        EnterCriticalSection(&g_cs); uint32_t bytes = g_bw.len; LeaveCriticalSection(&g_cs);
        rgpu_log("submit#%ld lists=%ld | tee draws=%ld dispatch=%ld barriers=%ld clears=%ld setRT=%ld copies=%ld | batch=%u bytes",
                 e, g_lists, g_draws, g_dispatch, g_barriers, g_clears, g_rts, g_copies, bytes);
    }
}

/* Read the function pointer at a vtable slot of a fresh (un-tampered) object. */
static void *vslot(void *obj, unsigned idx) { return (*(void ***)obj)[idx]; }

/* Inline-hook a target function ONCE (guarded by *orig, which becomes the
 * trampoline that calls the original). INLINE hooking rewrites the function BODY,
 * so a third-party overlay (UE4SS) that re-patches the same vtable slot afterwards
 * cannot bypass us — the reason plain vtable hooking failed to capture TXR. */
static void arm_inline_once(void *fn, void *detour, void **orig, const char *name) {
    if (*orig) return;                    /* already inline-hooked */
    void *tramp = nullptr;
    if (rgpu_inline_hook(fn, detour, &tramp)) { *orig = tramp; rgpu_log("inline-hooked %s @ %p", name, fn); }
    else rgpu_log("inline-hook ABORTED for %s @ %p (prologue not relocatable; left unhooked)", name, fn);
}

/* Suspend every OTHER thread in the process so writing D3D12Core code pages can't
 * race a thread executing the function being patched (the crash that boot-looped
 * TXR). MinHook-style. While frozen we must NOT touch the CRT heap (malloc / fopen)
 * or we could deadlock on a lock a frozen thread holds — so no logging/allocation
 * there; rgpu_inline_hook uses only VirtualAlloc/Protect syscalls. */
static HANDLE g_frozen[4096]; static int g_nfrozen = 0;
static void rgpu_freeze_other_threads() {
    g_nfrozen = 0;
    DWORD pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != tid) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (h) { if (SuspendThread(h) != (DWORD)-1 && g_nfrozen < 4096) g_frozen[g_nfrozen++] = h; else CloseHandle(h); }
        }
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}
static void rgpu_resume_other_threads() {
    for (int i = 0; i < g_nfrozen; i++) { ResumeThread(g_frozen[i]); CloseHandle(g_frozen[i]); }
    g_nfrozen = 0;
}
struct HookItem { void *fn; void *detour; void **orig; const char *name; };
static void install_batch_frozen(HookItem *items, int n) {
    if (!n) return;
    rgpu_freeze_other_threads();
    for (int i = 0; i < n; i++) { void *tr = nullptr; if (rgpu_inline_hook(items[i].fn, items[i].detour, &tr)) *items[i].orig = tr; }
    rgpu_resume_other_threads();
    for (int i = 0; i < n; i++) rgpu_log("%s %s @ %p", *items[i].orig ? "inline-hooked" : "inline-hook ABORTED for", items[i].name, items[i].fn);
}

/* Inline-hook the ExecuteCommandLists / recording functions read from the GAME's
 * ACTUAL objects (their concrete D3D12Core class can differ from a base throwaway
 * list). Installed as a thread-frozen batch so the live runtime isn't corrupted. */
static void arm_queue_functions(ID3D12CommandQueue *q) {
    if (o_ExecuteCommandLists) return;
    HookItem it[1] = {{ vslot(q, RGPU_SLOT_Queue_ExecuteCommandLists), (void *)h_ExecuteCommandLists, (void **)&o_ExecuteCommandLists, "ExecuteCommandLists" }};
    install_batch_frozen(it, 1);
}
static void arm_list_functions(ID3D12GraphicsCommandList *list) {
    HookItem it[20]; int n = 0;
    auto add = [&](unsigned slot, void *det, void **orig, const char *nm) { if (!*orig && n < 20) it[n++] = { vslot(list, slot), det, orig, nm }; };
    add(RGPU_SLOT_GCL_DrawInstanced,         (void *)h_DrawInstanced,        (void **)&o_DrawInstanced,        "DrawInstanced");
    add(RGPU_SLOT_GCL_DrawIndexedInstanced,  (void *)h_DrawIndexedInstanced, (void **)&o_DrawIndexedInstanced, "DrawIndexedInstanced");
    add(RGPU_SLOT_GCL_Dispatch,              (void *)h_Dispatch,             (void **)&o_Dispatch,             "Dispatch");
    add(RGPU_SLOT_GCL_ResourceBarrier,       (void *)h_ResourceBarrier,      (void **)&o_ResourceBarrier,      "ResourceBarrier");
    add(RGPU_SLOT_GCL_ClearRenderTargetView, (void *)h_ClearRTV,             (void **)&o_ClearRTV,             "ClearRenderTargetView");
    add(RGPU_SLOT_GCL_OMSetRenderTargets,    (void *)h_OMSetRenderTargets,   (void **)&o_OMSetRenderTargets,   "OMSetRenderTargets");
    add(RGPU_SLOT_GCL_CopyResource,          (void *)h_CopyResource,         (void **)&o_CopyResource,         "CopyResource");
    add(RGPU_SLOT_GCL_ExecuteIndirect,       (void *)h_ExecuteIndirect,      (void **)&o_ExecuteIndirect,      "ExecuteIndirect");
    ID3D12GraphicsCommandList6 *gl6 = nullptr;
    if (SUCCEEDED(list->QueryInterface(__uuidof(ID3D12GraphicsCommandList6), (void **)&gl6)) && gl6) { add(RGPU_SLOT_GCL6_DispatchMesh, (void *)h_DispatchMesh, (void **)&o_DispatchMesh, "DispatchMesh"); gl6->Release(); }
    ID3D12GraphicsCommandList7 *gl7 = nullptr;
    if (SUCCEEDED(list->QueryInterface(__uuidof(ID3D12GraphicsCommandList7), (void **)&gl7)) && gl7) { add(RGPU_SLOT_GCL7_Barrier, (void *)h_Barrier, (void **)&o_Barrier, "Barrier(enhanced)"); gl7->Release(); }
    install_batch_frozen(it, n);
}
static void arm_queue_from_any(void *o) { ID3D12CommandQueue *q=nullptr; if (o && SUCCEEDED(((IUnknown*)o)->QueryInterface(__uuidof(ID3D12CommandQueue),(void**)&q))&&q){ arm_queue_functions(q); q->Release(); } }
static void arm_list_from_any(void *o)  { ID3D12GraphicsCommandList *l=nullptr; if (o && SUCCEEDED(((IUnknown*)o)->QueryInterface(__uuidof(ID3D12GraphicsCommandList),(void**)&l))&&l){ arm_list_functions(l); l->Release(); } }

/* Vtable hooks on the DEVICE's create methods (device vtable is not contended by
 * overlays) so we see the game's REAL queues/lists and inline-hook THEIR functions. */
typedef HRESULT (STDMETHODCALLTYPE *fnDevCCQ)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnDevCCQ1)(ID3D12Device9 *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnDevCCL)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnDevCCL1)(ID3D12Device4 *, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void **);
static fnDevCCQ o_DevCCQ; static fnDevCCQ1 o_DevCCQ1; static fnDevCCL o_DevCCL; static fnDevCCL1 o_DevCCL1;
static void once_log(volatile LONG *g, const char *m) { if (InterlockedCompareExchange(g, 1, 0) == 0) rgpu_log("%s", m); }
static volatile LONG l_ccq=0, l_ccq1=0, l_ccl=0, l_ccl1=0, l_cfs=0;
typedef HRESULT (STDMETHODCALLTYPE *fnCFS)(ID3D12Device *, D3D12_FEATURE, void *, UINT);
static fnCFS o_CFS;
static HRESULT STDMETHODCALLTYPE h_CFS(ID3D12Device *T, D3D12_FEATURE f, void *d, UINT n){ once_log(&l_cfs,"game USES our hooked device (CheckFeatureSupport fired) -> device object is right"); return o_CFS(T,f,d,n); }
static HRESULT STDMETHODCALLTYPE h_DevCCQ(ID3D12Device *T, const D3D12_COMMAND_QUEUE_DESC *d, REFIID r, void **pp){ once_log(&l_ccq,"game called CreateCommandQueue (base)"); HRESULT hr=o_DevCCQ(T,d,r,pp); if(SUCCEEDED(hr)&&pp)arm_queue_from_any(*pp); return hr; }
static HRESULT STDMETHODCALLTYPE h_DevCCQ1(ID3D12Device9 *T, const D3D12_COMMAND_QUEUE_DESC *d, REFIID c, REFIID r, void **pp){ once_log(&l_ccq1,"game called CreateCommandQueue1 (Agility)"); HRESULT hr=o_DevCCQ1(T,d,c,r,pp); if(SUCCEEDED(hr)&&pp)arm_queue_from_any(*pp); return hr; }
static HRESULT STDMETHODCALLTYPE h_DevCCL(ID3D12Device *T, UINT nm, D3D12_COMMAND_LIST_TYPE ty, ID3D12CommandAllocator *a, ID3D12PipelineState *ps, REFIID r, void **pp){ once_log(&l_ccl,"game called CreateCommandList (base)"); HRESULT hr=o_DevCCL(T,nm,ty,a,ps,r,pp); if(SUCCEEDED(hr)&&pp)arm_list_from_any(*pp); return hr; }
static HRESULT STDMETHODCALLTYPE h_DevCCL1(ID3D12Device4 *T, UINT nm, D3D12_COMMAND_LIST_TYPE ty, D3D12_COMMAND_LIST_FLAGS f, REFIID r, void **pp){ once_log(&l_ccl1,"game called CreateCommandList1 (Agility)"); HRESULT hr=o_DevCCL1(T,nm,ty,f,r,pp); if(SUCCEEDED(hr)&&pp)arm_list_from_any(*pp); return hr; }

/* Inline-hook device methods from the real returned interface. This survives
 * UE4SS/overlay vtable replacement just like the queue/list inline hooks. */
static void arm_from_device(ID3D12Device *dev, const char *which) {
    if (!dev) return;

    ID3D12Device *base = nullptr;
    ID3D12Device4 *d4 = nullptr;
    ID3D12Device9 *d9 = nullptr;
    dev->QueryInterface(__uuidof(ID3D12Device), (void **)&base);
    dev->QueryInterface(__uuidof(ID3D12Device4), (void **)&d4);
    dev->QueryInterface(__uuidof(ID3D12Device9), (void **)&d9);

    void *bv = base ? *(void **)base : nullptr;
    void *v4 = d4 ? *(void **)d4 : nullptr;
    void *v9 = d9 ? *(void **)d9 : nullptr;

    HookItem items[5]; int n = 0;
    auto add = [&](void *obj, unsigned slot, void *detour, void **orig, const char *name) {
        if (obj && !*orig && n < 5) items[n++] = { vslot(obj, slot), detour, orig, name };
    };
    add(base, RGPU_SLOT_Device_CheckFeatureSupport, (void *)h_CFS, (void **)&o_CFS,
        "Device::CheckFeatureSupport");
    add(base, RGPU_SLOT_Device_CreateCommandQueue, (void *)h_DevCCQ, (void **)&o_DevCCQ,
        "Device::CreateCommandQueue");
    add(base, RGPU_SLOT_Device_CreateCommandList, (void *)h_DevCCL, (void **)&o_DevCCL,
        "Device::CreateCommandList");
    add(d4, RGPU_SLOT_Device4_CreateCommandList1, (void *)h_DevCCL1, (void **)&o_DevCCL1,
        "Device4::CreateCommandList1");
    add(d9, RGPU_SLOT_Device9_CreateCommandQueue1, (void *)h_DevCCQ1, (void **)&o_DevCCQ1,
        "Device9::CreateCommandQueue1");
    install_batch_frozen(items, n);

    /* The conservative inline decoder can reject some base-method prologues.
     * Keep a safe vtable fallback for those exact methods; the Agility 1 methods
     * remain inline-hooked and therefore survive later overlay vtable changes. */
    bool fbCFS = false, fbCCQ = false, fbCCL = false, fbCCQ1 = false, fbCCL1 = false;
    if (base && !o_CFS) fbCFS = patch_slot(base, RGPU_SLOT_Device_CheckFeatureSupport,
                                            (void *)h_CFS, (void **)&o_CFS);
    if (base && !o_DevCCQ) fbCCQ = patch_slot(base, RGPU_SLOT_Device_CreateCommandQueue,
                                              (void *)h_DevCCQ, (void **)&o_DevCCQ);
    if (base && !o_DevCCL) fbCCL = patch_slot(base, RGPU_SLOT_Device_CreateCommandList,
                                              (void *)h_DevCCL, (void **)&o_DevCCL);
    if (d9 && !o_DevCCQ1) fbCCQ1 = patch_slot(d9, RGPU_SLOT_Device9_CreateCommandQueue1,
                                               (void *)h_DevCCQ1, (void **)&o_DevCCQ1);
    if (d4 && !o_DevCCL1) fbCCL1 = patch_slot(d4, RGPU_SLOT_Device4_CreateCommandList1,
                                               (void *)h_DevCCL1, (void **)&o_DevCCL1);

    rgpu_log("armed-hybrid %s device input=%p base=%p/vtbl=%p d4=%p/vtbl=%p d9=%p/vtbl=%p originals CFS=%p CCQ=%p CCL=%p CCQ1=%p CCL1=%p fallback=%d/%d/%d/%d/%d",
             which, (void *)dev, (void *)base, bv, (void *)d4, v4, (void *)d9, v9,
             (void *)o_CFS, (void *)o_DevCCQ, (void *)o_DevCCL,
             (void *)o_DevCCQ1, (void *)o_DevCCL1,
             fbCFS, fbCCQ, fbCCL, fbCCQ1, fbCCL1);

    if (d9) d9->Release();
    if (d4) d4->Release();
    if (base) base->Release();
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
static void *g_hooked_factory_vtbl = nullptr;
static volatile LONG g_sdk_config_hooks = 0;
static volatile LONG g_sdk_factory_calls = 0;
static volatile LONG g_factory_device_calls = 0;

static HRESULT STDMETHODCALLTYPE h_FactoryCreateDevice(ID3D12DeviceFactory *This, IUnknown *adapter,
                                                        D3D_FEATURE_LEVEL fl, REFIID riid, void **pp);

static bool hook_factory_object(void *obj) {
    if (!obj) return false;
    ID3D12DeviceFactory *factory = nullptr;
    if (FAILED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12DeviceFactory), (void **)&factory)) || !factory)
        return false;
    void *vtbl = *(void **)factory;
    bool ok = true;
    if (vtbl != g_hooked_factory_vtbl) {
        ok = patch_slot(factory, RGPU_SLOT_Factory_CreateDevice,
                        (void *)h_FactoryCreateDevice, (void **)&o_FactoryCreateDevice);
        if (ok) {
            g_hooked_factory_vtbl = vtbl;
            rgpu_log("hooked ID3D12DeviceFactory::CreateDevice factory=%p vtbl=%p",
                     (void *)factory, vtbl);
        }
    }
    factory->Release();
    return ok;
}

static HRESULT STDMETHODCALLTYPE h_FactoryCreateDevice(ID3D12DeviceFactory *This, IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **pp) {
    InterlockedIncrement(&g_factory_device_calls);
    HRESULT hr = o_FactoryCreateDevice(This, adapter, fl, riid, pp);
    bool warp = adapter_is_warp(adapter);
    rgpu_log("ID3D12DeviceFactory::CreateDevice fl=0x%X adapter=%s -> hr=0x%08lX device=%p",
             (unsigned)fl, warp ? "WARP" : "hardware", (unsigned long)hr, pp ? *pp : nullptr);
    if (SUCCEEDED(hr) && pp && *pp && !warp) shadow_patch_device(*pp);   /* in place */
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *fnSDKCreateDeviceFactory)(ID3D12SDKConfiguration1 *, UINT, LPCSTR, REFIID, void **);
static fnSDKCreateDeviceFactory o_SDKCreateDeviceFactory;
static void *g_hooked_sdkconfig_vtbl = nullptr;

static HRESULT STDMETHODCALLTYPE h_SDKCreateDeviceFactory(ID3D12SDKConfiguration1 *This, UINT sdkVersion,
                                                          LPCSTR sdkPath, REFIID riid, void **ppFactory) {
    InterlockedIncrement(&g_sdk_factory_calls);
    HRESULT hr = o_SDKCreateDeviceFactory(This, sdkVersion, sdkPath, riid, ppFactory);
    rgpu_log("ID3D12SDKConfiguration1::CreateDeviceFactory sdk=%u path=\"%s\" -> hr=0x%08lX factory=%p",
             sdkVersion, sdkPath ? sdkPath : "(null)", (unsigned long)hr,
             ppFactory ? *ppFactory : nullptr);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory_object(*ppFactory);
    return hr;
}

static bool hook_sdk_configuration(void *obj) {
    if (!obj) return false;
    ID3D12SDKConfiguration1 *sdk = nullptr;
    if (FAILED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12SDKConfiguration1), (void **)&sdk)) || !sdk)
        return false;
    void *vtbl = *(void **)sdk;
    bool ok = true;
    if (vtbl != g_hooked_sdkconfig_vtbl) {
        ok = patch_slot(sdk, RGPU_SLOT_SDKConfig1_CreateDeviceFactory,
                        (void *)h_SDKCreateDeviceFactory, (void **)&o_SDKCreateDeviceFactory);
        if (ok) {
            g_hooked_sdkconfig_vtbl = vtbl;
            InterlockedIncrement(&g_sdk_config_hooks);
            rgpu_log("hooked ID3D12SDKConfiguration1::CreateDeviceFactory sdkconfig=%p vtbl=%p",
                     (void *)sdk, vtbl);
        }
    }
    sdk->Release();
    return ok;
}

/* ---- Agility D3D12Core.dll direct interception --------------------------
 * TXR (UE5 + Agility 1.614) loads D3D12Core.dll itself and calls its ONLY export,
 * D3D12Core!D3D12GetInterface, to reach ID3D12SDKConfiguration1::CreateDeviceFactory
 * -> ID3D12DeviceFactory::CreateDevice. That real device is created entirely inside
 * D3D12Core and never touches our app-level d3d12.dll exports (which see only the
 * early FL_1_0_CORE probe devices). So we hook D3D12Core's export directly; from
 * there the existing SDKConfiguration/factory hooks wrap the device the game keeps. */
static void rgpu_after_get_interface(REFCLSID rclsid, void **ppv) {
    if (!ppv || !*ppv) return;
    /* Detect by TYPE, not CLSID: UE5/Agility may request the SDK-configuration or the
     * device-factory under a CLSID we don't hard-code, so QI the returned object for
     * both interfaces and hook whichever it actually is. */
    IUnknown *u = (IUnknown *)*ppv;
    /* Identify the returned object by probing a battery of known D3D12 interfaces
     * (some newer than mingw's headers - defined by GUID here). This tells us which
     * object the Agility bootstrap uses so we can hook its device-creation method. */
    struct { const char *name; GUID iid; } probes[] = {
        { "ID3D12Device",             __uuidof(ID3D12Device) },
        { "ID3D12DeviceFactory",      __uuidof(ID3D12DeviceFactory) },
        { "ID3D12SDKConfiguration",   __uuidof(ID3D12SDKConfiguration) },
        { "ID3D12SDKConfiguration1",  __uuidof(ID3D12SDKConfiguration1) },
        /* ID3D12DeviceConfiguration (Agility) - not in this mingw header */
        { "ID3D12DeviceConfiguration",{0x78dbf87b,0xf766,0x422b,{0xa6,0x1c,0xc8,0xc4,0x46,0xbd,0xb9,0xad}} },
        { "ID3D12Tools",              {0x7071e1f0,0xe84b,0x4b33,{0x97,0x4f,0x12,0xfa,0x49,0xde,0x65,0xc5}} },
    };
    char hits[512]; hits[0] = 0;
    for (auto &pr : probes) {
        void *t = nullptr;
        if (SUCCEEDED(u->QueryInterface(pr.iid, &t)) && t) {
            ((IUnknown *)t)->Release();
            std::strcat(hits, pr.name); std::strcat(hits, " ");
        }
    }
    rgpu_log("after_get_interface clsid={%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X} obj=%p supports=[%s]",
             (unsigned long)rclsid.Data1, rclsid.Data2, rclsid.Data3,
             rclsid.Data4[0], rclsid.Data4[1], rclsid.Data4[2], rclsid.Data4[3],
             rclsid.Data4[4], rclsid.Data4[5], rclsid.Data4[6], rclsid.Data4[7], *ppv, hits[0] ? hits : "(none)");
    ID3D12SDKConfiguration1 *sdk = nullptr; ID3D12DeviceFactory *fac = nullptr;
    if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D12SDKConfiguration1), (void **)&sdk)) && sdk) { sdk->Release(); hook_sdk_configuration(*ppv); }
    if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D12DeviceFactory), (void **)&fac)) && fac) { fac->Release(); hook_factory_object(*ppv); }
}
typedef HRESULT (WINAPI *fnGetInterfaceExport)(REFCLSID, REFIID, void **);
static fnGetInterfaceExport o_CoreGetInterface = nullptr;

/* --- canonical-vtable in-place interception ---------------------------------
 * The device TXR renders with is created by D3D12Core's private ID3D12CoreModule
 * (IID {DFAFDD2C-...}): D3D12GetInterface -> CoreModule -> GetDllExports -> export
 * table entry 0 == the real CreateDevice (D3D12Core.dll+0x6AE20 for 1.614). We must
 * NOT substitute the device pointer (concrete D3D12Core/d3d12.dll code reads non-COM
 * fields at fixed offsets - a foreign wrapper crashes it). We tried a per-object SHADOW
 * vtable (copy + swap the object's vtable pointer), but this D3D12Core RESTORES an
 * object's vtable pointer to its canonical vtable (proven: a command list's Draw hook
 * only fired after we re-applied the shadow on Reset). So instead we patch the CANONICAL
 * vtable IN PLACE (patch_slot): the runtime's restoration points objects back AT that
 * canonical vtable, so our hooks survive it; the object's vtable pointer is unchanged so
 * identity + concrete layout are preserved; and one patch covers every object of the
 * class. Each hook tees then calls the saved original - no wrapper, no unwrap. (This is
 * the PIX/RenderDoc approach; the user's shadow suggestion predates the restore finding.)
 * ID3D12CoreModule layout per RenderDoc: IUnknown 0-2, LOEnter 3, LOLeave 4, LOTryEnter
 * 5, Initialize 6, GetSDKVersion 7, GetDllExports 8. */
static uintptr_t g_core_base = 0, g_core_end = 0;
static bool safe_read_ptr(void *p, void **out) {   /* mingw g++ has no __try/__except */
    if (!p || ((uintptr_t)p & 7)) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    DWORD pr = mbi.Protect;
    if (pr & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (!(pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    *out = *(void **)p; return true;
}
static bool is_code_ptr(void *p) {   /* points into an executable module page */
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
static volatile LONG g_sh_devices = 0, g_sh_queues = 0, g_sh_lists = 0;
/* patch a canonical-vtable slot once (guarded by *orig staying null until patched). */
static bool patch_canonical(void *obj, unsigned slot, void *detour, void **orig) {
    if (*orig) return true;                       /* this vtable already patched */
    return patch_slot(obj, slot, detour, orig);   /* patch_slot saves *orig then writes */
}

/* Queue: intercept ExecuteCommandLists (the submission boundary). */
static void shadow_patch_queue(void *q) {
    if (!q || o_ExecuteCommandLists) return;
    if (patch_canonical(q, RGPU_SLOT_Queue_ExecuteCommandLists, (void *)h_ExecuteCommandLists, (void **)&o_ExecuteCommandLists))
        if (InterlockedIncrement(&g_sh_queues) == 1) rgpu_log("patched canonical QUEUE vtable ExecuteCommandLists (survives vtable restore)");
}
/* Command list: intercept the recording slots + Close. Draw/etc. survive the runtime's
 * Reset-time vtable restore because the canonical vtable itself is patched. */
static void shadow_patch_list(void *l) {
    if (!l || o_DrawInstanced) return;
    patch_canonical(l, RGPU_SLOT_GCL_DrawInstanced,         (void *)h_DrawInstanced,        (void **)&o_DrawInstanced);
    patch_canonical(l, RGPU_SLOT_GCL_DrawIndexedInstanced,  (void *)h_DrawIndexedInstanced, (void **)&o_DrawIndexedInstanced);
    patch_canonical(l, RGPU_SLOT_GCL_Dispatch,              (void *)h_Dispatch,             (void **)&o_Dispatch);
    patch_canonical(l, RGPU_SLOT_GCL_ResourceBarrier,       (void *)h_ResourceBarrier,      (void **)&o_ResourceBarrier);
    patch_canonical(l, RGPU_SLOT_GCL_ClearRenderTargetView, (void *)h_ClearRTV,             (void **)&o_ClearRTV);
    patch_canonical(l, RGPU_SLOT_GCL_OMSetRenderTargets,    (void *)h_OMSetRenderTargets,   (void **)&o_OMSetRenderTargets);
    patch_canonical(l, RGPU_SLOT_GCL_CopyResource,          (void *)h_CopyResource,         (void **)&o_CopyResource);
    patch_canonical(l, RGPU_SLOT_GCL_ExecuteIndirect,       (void *)h_ExecuteIndirect,      (void **)&o_ExecuteIndirect);
    patch_canonical(l, RGPU_SLOT_GCL_Close,                 (void *)h_Close,                (void **)&o_Close);
    /* GCL6/GCL7 slots exist only if the list supports them (UE5 5.x) - QI to confirm. */
    { ID3D12GraphicsCommandList6 *l6=nullptr; if (SUCCEEDED(((IUnknown*)l)->QueryInterface(__uuidof(ID3D12GraphicsCommandList6),(void**)&l6))&&l6){ patch_canonical(l6, RGPU_SLOT_GCL6_DispatchMesh,(void*)h_DispatchMesh,(void**)&o_DispatchMesh); l6->Release(); } }
    { ID3D12GraphicsCommandList7 *l7=nullptr; if (SUCCEEDED(((IUnknown*)l)->QueryInterface(__uuidof(ID3D12GraphicsCommandList7),(void**)&l7))&&l7){ patch_canonical(l7, RGPU_SLOT_GCL7_Barrier,(void*)h_Barrier,(void**)&o_Barrier); l7->Release(); } }
    if (InterlockedIncrement(&g_sh_lists) == 1) rgpu_log("patched canonical LIST vtable recording slots (Draw/Dispatch/Barrier/Clear/Copy/Close)");
}
static void shadow_patch_queue_from_any(void *o) { ID3D12CommandQueue *q=nullptr; if (o && SUCCEEDED(((IUnknown*)o)->QueryInterface(__uuidof(ID3D12CommandQueue),(void**)&q))&&q){ shadow_patch_queue(q); q->Release(); } }
static void shadow_patch_list_from_any(void *o)  { ID3D12GraphicsCommandList *l=nullptr; if (o && SUCCEEDED(((IUnknown*)o)->QueryInterface(__uuidof(ID3D12GraphicsCommandList),(void**)&l))&&l){ shadow_patch_list(l); l->Release(); } }

/* Device create-method detours: create via the original, then patch the returned
 * queue/list's canonical vtable. */
static fnDevCCQ o_sh_DevCCQ; static fnDevCCQ1 o_sh_DevCCQ1; static fnDevCCL o_sh_DevCCL; static fnDevCCL1 o_sh_DevCCL1;
static HRESULT STDMETHODCALLTYPE sh_DevCCQ(ID3D12Device *T, const D3D12_COMMAND_QUEUE_DESC *d, REFIID r, void **pp) { HRESULT hr=o_sh_DevCCQ(T,d,r,pp); if(SUCCEEDED(hr)&&pp&&*pp) shadow_patch_queue_from_any(*pp); return hr; }
static HRESULT STDMETHODCALLTYPE sh_DevCCQ1(ID3D12Device9 *T, const D3D12_COMMAND_QUEUE_DESC *d, REFIID c, REFIID r, void **pp) { HRESULT hr=o_sh_DevCCQ1(T,d,c,r,pp); if(SUCCEEDED(hr)&&pp&&*pp) shadow_patch_queue_from_any(*pp); return hr; }
static HRESULT STDMETHODCALLTYPE sh_DevCCL(ID3D12Device *T, UINT nm, D3D12_COMMAND_LIST_TYPE ty, ID3D12CommandAllocator *a, ID3D12PipelineState *ps, REFIID r, void **pp) { HRESULT hr=o_sh_DevCCL(T,nm,ty,a,ps,r,pp); if(SUCCEEDED(hr)&&pp&&*pp) shadow_patch_list_from_any(*pp); return hr; }
static HRESULT STDMETHODCALLTYPE sh_DevCCL1(ID3D12Device4 *T, UINT nm, D3D12_COMMAND_LIST_TYPE ty, D3D12_COMMAND_LIST_FLAGS f, REFIID r, void **pp) { HRESULT hr=o_sh_DevCCL1(T,nm,ty,f,r,pp); if(SUCCEEDED(hr)&&pp&&*pp) shadow_patch_list_from_any(*pp); return hr; }

/* Hook a device create-method by INLINE-hooking its implementation function (read from
 * the vtable slot) rather than the vtable slot itself. This catches BOTH public callers
 * (vtable slot -> impl -> our inline hook) AND internal callers that jump straight to the
 * impl (D3D11On12 / d3d12.dll create queues without calling the public COM method), and it
 * is immune to the runtime restoring the object's vtable pointer. Falls back to a canonical
 * vtable patch if the impl prologue is not relocatable by our conservative decoder. */
static void hook_dev_method(ID3D12Device *dev, unsigned slot, void *detour, void **orig, const char *name) {
    if (*orig) return;
    void **vt = *(void ***)dev;
    void *impl = vt[slot];
    HookItem it[1] = {{ impl, detour, orig, name }};
    install_batch_frozen(it, 1);              /* inline-hook the impl (thread-frozen) */
    if (!*orig) {
        unsigned char *b = (unsigned char *)impl;
        rgpu_log("%s inline aborted; impl=%p prologue %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                 name, impl, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        patch_canonical(dev, slot, detour, orig);
    }
}
/* Device: intercept queue/list creation so we reach the real game-used queues/lists that
 * carry the per-frame command stream - including internally-created ones. */
static void shadow_patch_device(void *devPtr) {
    if (!devPtr) return;
    ID3D12Device *dev = nullptr;
    if (FAILED(((IUnknown *)devPtr)->QueryInterface(__uuidof(ID3D12Device), (void **)&dev)) || !dev) return;
    if (!o_sh_DevCCQ) {
        patch_canonical(dev, RGPU_SLOT_Device_CheckFeatureSupport, (void *)h_CFS, (void **)&o_CFS); /* diag */
        hook_dev_method(dev, RGPU_SLOT_Device_CreateCommandQueue,   (void *)sh_DevCCQ,  (void **)&o_sh_DevCCQ,  "CreateCommandQueue impl");
        hook_dev_method(dev, RGPU_SLOT_Device_CreateCommandList,    (void *)sh_DevCCL,  (void **)&o_sh_DevCCL,  "CreateCommandList impl");
        hook_dev_method(dev, RGPU_SLOT_Device4_CreateCommandList1,  (void *)sh_DevCCL1, (void **)&o_sh_DevCCL1, "CreateCommandList1 impl");
        hook_dev_method(dev, RGPU_SLOT_Device9_CreateCommandQueue1, (void *)sh_DevCCQ1, (void **)&o_sh_DevCCQ1, "CreateCommandQueue1 impl");
        if (InterlockedIncrement(&g_sh_devices) == 1)
            rgpu_log("hooked DEVICE create-queue/list impls (public + internal callers; CCQ=%p CCL=%p CCL1=%p CCQ1=%p)",
                     (void *)o_sh_DevCCQ, (void *)o_sh_DevCCL, (void *)o_sh_DevCCL1, (void *)o_sh_DevCCQ1);
    }
    dev->Release();
}

/* --- DXGI swap-chain hooks: identify the game's real render QUEUE -----------
 * For D3D12, IDXGIFactory*::CreateSwapChain*'s `pDevice` argument IS the command queue
 * the swap chain presents from - the game's actual render queue. This is the
 * unambiguous way to find it (vs guessing from feature level / call counts): adopt that
 * queue (shadow-patch its ExecuteCommandLists) and its device. */
extern "C" {
extern unsigned RGPU_SLOT_DXGIFactory_CreateSwapChain,
    RGPU_SLOT_DXGIFactory2_CreateSwapChainForHwnd, RGPU_SLOT_DXGIFactory2_CreateSwapChainForComposition;
}
/* If TXR presents via D3D11On12, its rendering may be D3D11 immediate-context draws that
 * D3D11On12 translates to D3D12 internally (which is why no public D3D12 queue/list is
 * ever created). Probe the present D3D11 device's immediate context: hook Draw(13) /
 * DrawIndexed(12). If they fire, the game renders via D3D11 - and we capture it there. */
typedef void (STDMETHODCALLTYPE *fnD11Draw)(ID3D11DeviceContext *, UINT, UINT);
typedef void (STDMETHODCALLTYPE *fnD11DrawIndexed)(ID3D11DeviceContext *, UINT, UINT, INT);
static fnD11Draw o_D11Draw = nullptr; static fnD11DrawIndexed o_D11DrawIndexed = nullptr;
static void STDMETHODCALLTYPE h_D11Draw(ID3D11DeviceContext *c, UINT vc, UINT sv) {
    if (InterlockedIncrement(&g_draws) == 1) rgpu_log("D3D11 Draw fired -> TXR renders via D3D11On12; capturing at the D3D11 layer");
    rgpu_args_draw a{vc, sv, 1, 0}; tee(RGPU_CMD_DRAW, oid(c), &a, sizeof(a));
    o_D11Draw(c, vc, sv);
}
static void STDMETHODCALLTYPE h_D11DrawIndexed(ID3D11DeviceContext *c, UINT ic, UINT si, INT bv) {
    if (InterlockedIncrement(&g_draws) == 1) rgpu_log("D3D11 DrawIndexed fired -> TXR renders via D3D11On12; capturing at the D3D11 layer");
    rgpu_args_draw_indexed a{ic, si, bv, 1, 0}; tee(RGPU_CMD_DRAW_INDEXED, oid(c), &a, sizeof(a));
    o_D11DrawIndexed(c, ic, si, bv);
}
static volatile LONG g_d11_probed = 0;
static void probe_d3d11_context(IUnknown *pDevice) {
    if (!pDevice || InterlockedCompareExchange(&g_d11_probed, 1, 0) != 0) return;
    ID3D11Device *d11 = nullptr;
    if (FAILED(pDevice->QueryInterface(__uuidof(ID3D11Device), (void **)&d11)) || !d11) { g_d11_probed = 0; return; }
    ID3D11DeviceContext *ctx = nullptr; d11->GetImmediateContext(&ctx);
    if (ctx) {
        bool a = patch_slot(ctx, 12, (void *)h_D11DrawIndexed, (void **)&o_D11DrawIndexed);
        bool b = patch_slot(ctx, 13, (void *)h_D11Draw, (void **)&o_D11Draw);
        rgpu_log("hooked D3D11On12 immediate-context %p Draw=%d DrawIndexed=%d (probe: does TXR render via D3D11?)", (void *)ctx, b, a);
        ctx->Release();
    }
    d11->Release();
}
static volatile LONG g_render_queue_found = 0;
static void adopt_render_queue(IUnknown *pDevice) {
    if (!pDevice) return;
    ID3D12CommandQueue *q = nullptr;
    HRESULT hrq = pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&q);
    if (SUCCEEDED(hrq) && q) {
        if (InterlockedIncrement(&g_render_queue_found) == 1)
            rgpu_log("DXGI swap chain -> render QUEUE %p adopted (presentation queue); this is TXR's live queue", (void *)q);
        shadow_patch_queue(q);
        ID3D12Device *dev = nullptr;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), (void **)&dev)) && dev) { shadow_patch_device(dev); dev->Release(); }
        q->Release();
    } else {
        /* Not a command queue by our IID - probe what it actually is. */
        void *vt = nullptr; safe_read_ptr(pDevice, &vt);
        void *caller = __builtin_return_address(0);
        auto modname = [](void *addr, char *buf) { HMODULE m=nullptr; buf[0]='?'; buf[1]=0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)addr,&m)&&m) GetModuleBaseNameA(GetCurrentProcess(),m,buf,MAX_PATH); };
        char vm[MAX_PATH], cm[MAX_PATH]; modname(vt, vm); modname(caller, cm);
        /* Is it an ID3D11Device (D3D11On12) or does it QI to a queue via a device? */
        void *t = nullptr;
        GUID IID_ID3D11Device = {0xdb6f6ddb,0xac77,0x4e88,{0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40}};
        bool is11 = SUCCEEDED(pDevice->QueryInterface(IID_ID3D11Device, &t)) && t; if (t) ((IUnknown*)t)->Release();
        rgpu_log("DXGI pDevice=%p vtbl=%p[%s] caller=%p[%s] QI(Queue)=0x%08lX isID3D11Device=%d - NOT a D3D12 queue",
                 (void *)pDevice, vt, vm, caller, cm, (unsigned long)hrq, is11);
        if (is11) probe_d3d11_context(pDevice);   /* D3D11On12 present -> probe/capture D3D11 draws */
    }
}
typedef HRESULT (STDMETHODCALLTYPE *fnCreateSwapChain)(IUnknown *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *fnCSCForHwnd)(IUnknown *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *fnCSCForComp)(IUnknown *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *, IDXGIOutput *, IDXGISwapChain1 **);
static fnCreateSwapChain o_CreateSwapChain; static fnCSCForHwnd o_CSCForHwnd; static fnCSCForComp o_CSCForComp;
static HRESULT STDMETHODCALLTYPE h_CreateSwapChain(IUnknown *T, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **pp) { rgpu_log("DXGI CreateSwapChain pDevice=%p", (void*)dev); adopt_render_queue(dev); return o_CreateSwapChain(T, dev, d, pp); }
static HRESULT STDMETHODCALLTYPE h_CSCForHwnd(IUnknown *T, IUnknown *dev, HWND h, const DXGI_SWAP_CHAIN_DESC1 *d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fd, IDXGIOutput *o, IDXGISwapChain1 **pp) { rgpu_log("DXGI CreateSwapChainForHwnd pDevice=%p hwnd=%p", (void*)dev, (void*)h); adopt_render_queue(dev); return o_CSCForHwnd(T, dev, h, d, fd, o, pp); }
static HRESULT STDMETHODCALLTYPE h_CSCForComp(IUnknown *T, IUnknown *dev, const DXGI_SWAP_CHAIN_DESC1 *d, IDXGIOutput *o, IDXGISwapChain1 **pp) { rgpu_log("DXGI CreateSwapChainForComposition pDevice=%p", (void*)dev); adopt_render_queue(dev); return o_CSCForComp(T, dev, d, o, pp); }
static void *g_hooked_dxgi_factory_vtbl = nullptr;
static void hook_dxgi_factory(void *factory) {
    if (!factory) return;
    void *vtbl = *(void **)factory; if (vtbl == g_hooked_dxgi_factory_vtbl) return;
    bool a = patch_slot(factory, RGPU_SLOT_DXGIFactory_CreateSwapChain, (void *)h_CreateSwapChain, (void **)&o_CreateSwapChain);
    bool b = false, c = false;
    IDXGIFactory2 *f2 = nullptr;
    if (SUCCEEDED(((IUnknown *)factory)->QueryInterface(__uuidof(IDXGIFactory2), (void **)&f2)) && f2) {
        b = patch_slot(f2, RGPU_SLOT_DXGIFactory2_CreateSwapChainForHwnd, (void *)h_CSCForHwnd, (void **)&o_CSCForHwnd);
        c = patch_slot(f2, RGPU_SLOT_DXGIFactory2_CreateSwapChainForComposition, (void *)h_CSCForComp, (void **)&o_CSCForComp);
        f2->Release();
    }
    g_hooked_dxgi_factory_vtbl = vtbl;
    rgpu_log("hooked DXGI factory %p vtbl=%p slots CreateSwapChain(%u)=%d ForHwnd(%u)=%d ForComp(%u)=%d",
             factory, vtbl, RGPU_SLOT_DXGIFactory_CreateSwapChain, a,
             RGPU_SLOT_DXGIFactory2_CreateSwapChainForHwnd, b, RGPU_SLOT_DXGIFactory2_CreateSwapChainForComposition, c);
}
typedef HRESULT (WINAPI *fnCreateDXGIFactory)(REFIID, void **);
typedef HRESULT (WINAPI *fnCreateDXGIFactory2)(UINT, REFIID, void **);
static fnCreateDXGIFactory o_CreateDXGIFactory, o_CreateDXGIFactory1; static fnCreateDXGIFactory2 o_CreateDXGIFactory2;
static HRESULT WINAPI h_CreateDXGIFactory(REFIID riid, void **pp) { HRESULT hr = o_CreateDXGIFactory(riid, pp); if (SUCCEEDED(hr) && pp && *pp) hook_dxgi_factory(*pp); return hr; }
static HRESULT WINAPI h_CreateDXGIFactory1(REFIID riid, void **pp) { HRESULT hr = o_CreateDXGIFactory1(riid, pp); if (SUCCEEDED(hr) && pp && *pp) hook_dxgi_factory(*pp); return hr; }
static HRESULT WINAPI h_CreateDXGIFactory2(UINT flags, REFIID riid, void **pp) { HRESULT hr = o_CreateDXGIFactory2(flags, riid, pp); if (SUCCEEDED(hr) && pp && *pp) hook_dxgi_factory(*pp); return hr; }
/* D3D11On12CreateDevice: TXR presents via a D3D11On12 swap chain (pDevice is an
 * ID3D11Device), so the real D3D12 render queue is passed here in ppCommandQueues. */
typedef HRESULT (WINAPI *fnD3D11On12CreateDevice)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                                  IUnknown *const *, UINT, UINT, void **, void **, D3D_FEATURE_LEVEL *);
static fnD3D11On12CreateDevice o_D3D11On12CreateDevice = nullptr;
static HRESULT WINAPI h_D3D11On12CreateDevice(IUnknown *dev, UINT flags, const D3D_FEATURE_LEVEL *fl, UINT nfl,
                                              IUnknown *const *ppQ, UINT nQ, UINT node, void **ppDev, void **ppCtx, D3D_FEATURE_LEVEL *pcfl) {
    rgpu_log("D3D11On12CreateDevice: d3d12device=%p numQueues=%u (adopting its backing D3D12 render queue)", (void *)dev, nQ);
    if (dev) shadow_patch_device(dev);
    for (UINT i = 0; i < nQ && ppQ; i++) if (ppQ[i]) shadow_patch_queue_from_any(ppQ[i]);
    return o_D3D11On12CreateDevice(dev, flags, fl, nfl, ppQ, nQ, node, ppDev, ppCtx, pcfl);
}
static volatile LONG g_d3d11on12_hooked = 0;
static void install_d3d11on12_hook() {
    if (o_D3D11On12CreateDevice) return;
    HMODULE d11 = GetModuleHandleA("d3d11.dll"); if (!d11) return;
    void *fn = (void *)GetProcAddress(d11, "D3D11On12CreateDevice"); if (!fn) return;
    if (InterlockedCompareExchange(&g_d3d11on12_hooked, 1, 0) != 0) return;
    arm_inline_once(fn, (void *)h_D3D11On12CreateDevice, (void **)&o_D3D11On12CreateDevice, "d3d11!D3D11On12CreateDevice");
    rgpu_log("D3D11On12CreateDevice hook %s @ %p", o_D3D11On12CreateDevice ? "installed" : "FAILED", fn);
}
static volatile LONG g_dxgi_hooked = 0;
static void install_dxgi_hooks() {
    install_d3d11on12_hook();   /* the D3D11On12 present path carries the render queue */
    if (g_dxgi_hooked) return;
    HMODULE dxgi = GetModuleHandleA("dxgi.dll"); if (!dxgi) return;
    if (InterlockedCompareExchange(&g_dxgi_hooked, 1, 0) != 0) return;
    void *f0 = (void *)GetProcAddress(dxgi, "CreateDXGIFactory");
    void *f1 = (void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
    void *f2 = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");
    if (f0) arm_inline_once(f0, (void *)h_CreateDXGIFactory,  (void **)&o_CreateDXGIFactory,  "dxgi!CreateDXGIFactory");
    if (f1) arm_inline_once(f1, (void *)h_CreateDXGIFactory1, (void **)&o_CreateDXGIFactory1, "dxgi!CreateDXGIFactory1");
    if (f2) arm_inline_once(f2, (void *)h_CreateDXGIFactory2, (void **)&o_CreateDXGIFactory2, "dxgi!CreateDXGIFactory2");
    rgpu_log("DXGI export hooks installed (CreateDXGIFactory/1/2) -> render-queue acquisition armed");
}

/* --- ID3D12CoreModule export-table interception ----------------------------
 * The export table (from GetDllExports / the versioned {FC454290} slot 11) is D3D12Core's
 * internal dispatch: entry 0 is CreateDevice, and other entries are the internal
 * command-queue / command-list creators the runtime + D3D11On12 invoke DIRECTLY (TXR never
 * calls the public ID3D12Device COM methods). We hook every code-pointer entry with a
 * generic 8-arg forwarder (safe superset on Win64) that snapshots each pointer arg before
 * the call and, after, canonical-patches any FRESHLY-created queue/list/device found in an
 * out-param - reaching the real render queue/lists regardless of how they are made. Once a
 * queue AND a list are found we restore the table to drop the steady-state overhead. */
static bool looks_like_com(void *obj) {   /* real COM object with a D3D12Core vtable */
    void *vt; if (!safe_read_ptr(obj, &vt)) return false;
    uintptr_t v = (uintptr_t)vt; if (v < g_core_base || v >= g_core_end) return false;
    void **vtbl = (void **)vt; void *f;
    for (int s = 0; s < 3; s++) if (!safe_read_ptr(&vtbl[s], &f) || !is_code_ptr(f)) return false;
    return true;   /* vtable in D3D12Core + QI/AddRef/Release are code pointers */
}
static uint64_t *g_cet_table = nullptr; static int g_cet_count = 0; static void *g_cet_orig[32];
static volatile LONG g_cet_q = 0, g_cet_l = 0, g_cet_d = 0;
static void restore_core_export_table() {
    if (!g_cet_table || !g_cet_count) return;
    DWORD oldp = 0;
    if (VirtualProtect(g_cet_table, g_cet_count * sizeof(void *), PAGE_READWRITE, &oldp)) {
        for (int i = 0; i < g_cet_count; i++) g_cet_table[i] = (uint64_t)(uintptr_t)g_cet_orig[i];
        DWORD ign = 0; VirtualProtect(g_cet_table, g_cet_count * sizeof(void *), oldp, &ign);
        FlushInstructionCache(GetCurrentProcess(), g_cet_table, g_cet_count * sizeof(void *));
    }
    g_cet_count = 0;
    rgpu_log("CORE_EXPORT_TABLE restored (queue+list captured; overhead removed)");
}
static void rgpu_cet_scan_out(int entry, void *created) {
    IUnknown *u = (IUnknown *)created;
    ID3D12CommandQueue *q = nullptr; ID3D12GraphicsCommandList *l = nullptr; ID3D12Device *d = nullptr;
    if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&q)) && q) {
        if (InterlockedIncrement(&g_cet_q) == 1) rgpu_log("CORE_EXPORT_TABLE[%d] produced a command QUEUE %p -> patching", entry, created);
        shadow_patch_queue(q); q->Release();
    } else if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D12GraphicsCommandList), (void **)&l)) && l) {
        if (InterlockedIncrement(&g_cet_l) == 1) rgpu_log("CORE_EXPORT_TABLE[%d] produced a command LIST %p -> patching", entry, created);
        shadow_patch_list(l); l->Release();
    } else if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D12Device), (void **)&d)) && d) {
        if (InterlockedIncrement(&g_cet_d) == 1) rgpu_log("CORE_EXPORT_TABLE[%d] produced a DEVICE %p -> patching", entry, created);
        shadow_patch_device(created); d->Release();
    }
    if (g_cet_d || (g_cet_q && g_cet_l)) restore_core_export_table();   /* device found via entry 0; queues aren't module exports */
}
typedef uint64_t (WINAPI *rgpu_cet_fn)(void *, void *, void *, void *, void *, void *, void *, void *);
static uint64_t rgpu_cet_dispatch(int i, void *a0, void *a1, void *a2, void *a3, void *a4, void *a5, void *a6, void *a7) {
    void *args[8] = {a0, a1, a2, a3, a4, a5, a6, a7}; void *before[8];
    for (int k = 0; k < 8; k++) if (!safe_read_ptr(args[k], &before[k])) before[k] = (void *)~(uintptr_t)0;
    uint64_t r = ((rgpu_cet_fn)g_cet_orig[i])(a0, a1, a2, a3, a4, a5, a6, a7);
    for (int k = 0; k < 8; k++) {
        void *after;
        if (safe_read_ptr(args[k], &after) && after != before[k] && looks_like_com(after)) rgpu_cet_scan_out(i, after);
    }
    return r;
}
#define CET_THUNK(n) static uint64_t WINAPI rgpu_cet_thunk_##n(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5, void *a6, void *a7) { return rgpu_cet_dispatch(n, a0, a1, a2, a3, a4, a5, a6, a7); }
CET_THUNK(0)  CET_THUNK(1)  CET_THUNK(2)  CET_THUNK(3)  CET_THUNK(4)  CET_THUNK(5)  CET_THUNK(6)  CET_THUNK(7)
CET_THUNK(8)  CET_THUNK(9)  CET_THUNK(10) CET_THUNK(11) CET_THUNK(12) CET_THUNK(13) CET_THUNK(14) CET_THUNK(15)
CET_THUNK(16) CET_THUNK(17) CET_THUNK(18) CET_THUNK(19) CET_THUNK(20) CET_THUNK(21) CET_THUNK(22) CET_THUNK(23)
static void *g_cet_thunks[24] = {
    (void *)rgpu_cet_thunk_0,(void *)rgpu_cet_thunk_1,(void *)rgpu_cet_thunk_2,(void *)rgpu_cet_thunk_3,
    (void *)rgpu_cet_thunk_4,(void *)rgpu_cet_thunk_5,(void *)rgpu_cet_thunk_6,(void *)rgpu_cet_thunk_7,
    (void *)rgpu_cet_thunk_8,(void *)rgpu_cet_thunk_9,(void *)rgpu_cet_thunk_10,(void *)rgpu_cet_thunk_11,
    (void *)rgpu_cet_thunk_12,(void *)rgpu_cet_thunk_13,(void *)rgpu_cet_thunk_14,(void *)rgpu_cet_thunk_15,
    (void *)rgpu_cet_thunk_16,(void *)rgpu_cet_thunk_17,(void *)rgpu_cet_thunk_18,(void *)rgpu_cet_thunk_19,
    (void *)rgpu_cet_thunk_20,(void *)rgpu_cet_thunk_21,(void *)rgpu_cet_thunk_22,(void *)rgpu_cet_thunk_23,
};
static volatile LONG g_cet_hooked = 0;
static void hook_core_export_table(void *table) {
    if (!table || InterlockedCompareExchange(&g_cet_hooked, 1, 0) != 0) return;
    uint64_t *q = (uint64_t *)table;
    int count = 0;
    for (int i = 0; i < 24; i++) { void *e; if (!safe_read_ptr(&q[i], &e) || !is_code_ptr(e)) break; count++; }
    DWORD oldp = 0;
    if (count && VirtualProtect(q, count * sizeof(void *), PAGE_READWRITE, &oldp)) {
        for (int i = 0; i < count; i++) { g_cet_orig[i] = (void *)(uintptr_t)q[i]; q[i] = (uint64_t)(uintptr_t)g_cet_thunks[i]; }
        DWORD ign = 0; VirtualProtect(q, count * sizeof(void *), oldp, &ign);
        FlushInstructionCache(GetCurrentProcess(), q, count * sizeof(void *));
        g_cet_table = q; g_cet_count = count;
    }
    rgpu_log("CORE_EXPORT_TABLE hooked %d entries (generic queue/list/device detection)", count);
}
typedef uintptr_t (STDMETHODCALLTYPE *fnCoreVersionedExports)(IUnknown *, void *);
static fnCoreVersionedExports o_CoreVersioned11 = nullptr;
static uintptr_t STDMETHODCALLTYPE h_CoreVersioned11(IUnknown *self, void *exportsTable) {
    uintptr_t r = o_CoreVersioned11(self, exportsTable);
    hook_core_export_table(exportsTable);
    return r;
}
typedef HRESULT (STDMETHODCALLTYPE *fnCoreGetDllExports)(IUnknown *, void *);
static fnCoreGetDllExports o_CoreGetDllExports = nullptr;
static HRESULT STDMETHODCALLTYPE h_CoreGetDllExports(IUnknown *self, void *table) {
    HRESULT hr = o_CoreGetDllExports(self, table);
    if (SUCCEEDED(hr)) hook_core_export_table(table);
    return hr;
}
typedef HRESULT (STDMETHODCALLTYPE *fnCoreQI)(IUnknown *, REFIID, void **);
static fnCoreQI o_CoreQI = nullptr;
static HRESULT STDMETHODCALLTYPE h_CoreQI(IUnknown *self, REFIID riid, void **ppv) {
    HRESULT hr = o_CoreQI(self, riid, ppv);
    /* The loader QIs a versioned interface {FC454290-...} then calls its slot 11 to get
     * the export table; hook that slot on the returned interface before it is used. */
    if (SUCCEEDED(hr) && ppv && *ppv && riid.Data1 == 0xFC454290 && riid.Data2 == 0x1B19 && riid.Data3 == 0x48C4 && !o_CoreVersioned11)
        patch_slot(*ppv, 11, (void *)h_CoreVersioned11, (void **)&o_CoreVersioned11);
    return hr;
}
static volatile LONG g_coremodule_hooked = 0;
static void hook_core_module_object(void *obj) {
    if (!obj || InterlockedCompareExchange(&g_coremodule_hooked, 1, 0) != 0) return;
    HMODULE core = GetModuleHandleA("D3D12Core.dll");
    MODULEINFO mi{}; if (core && GetModuleInformation(GetCurrentProcess(), core, &mi, sizeof(mi))) {
        g_core_base = (uintptr_t)mi.lpBaseOfDll; g_core_end = g_core_base + mi.SizeOfImage;
    }
    bool qiOk = patch_slot(obj, 0, (void *)h_CoreQI, (void **)&o_CoreQI);            /* QueryInterface */
    bool exOk = patch_slot(obj, 8, (void *)h_CoreGetDllExports, (void **)&o_CoreGetDllExports); /* GetDllExports */
    rgpu_log("ID3D12CoreModule hooked object=%p vtbl=%p QI=%d GetDllExports=%d (export-table CreateDevice route)",
             obj, *(void **)obj, qiOk, exOk);
}
static HRESULT WINAPI h_CoreGetInterface(REFCLSID rclsid, REFIID riid, void **ppv) {
    HRESULT hr = o_CoreGetInterface(rclsid, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        /* ID3D12CoreModule IID {DFAFDD2C-355F-4CB3-A8B2-EA7F9260148B}: the device is born
         * via its GetDllExports -> export table entry 0 CreateDevice (RenderDoc-confirmed
         * interface). Not SDKConfiguration/DeviceFactory - those are handled below. */
        if (riid.Data1 == 0xDFAFDD2C && riid.Data2 == 0x355F && riid.Data3 == 0x4CB3) {
            rgpu_log("D3D12Core!D3D12GetInterface -> ID3D12CoreModule obj=%p", *ppv);
            hook_core_module_object(*ppv);
        }
        rgpu_after_get_interface(rclsid, ppv);
    }
    return hr;
}
static volatile LONG g_core_hooked = 0;
/* Hook a SPECIFIC D3D12Core module (by base). The Agility loader maps its OWN copy
 * of D3D12Core at a private base (distinct from a plain LoadLibrary), so we must hook
 * the exact module the runtime uses - passing its base explicitly, not GetModuleHandle
 * (which is ambiguous when two copies are mapped). */
static bool hook_d3d12core_mod(HMODULE core) {
    if (o_CoreGetInterface) return true;
    if (!core) core = GetModuleHandleA("D3D12Core.dll");
    if (!core) return false;
    void *fn = (void *)GetProcAddress(core, "D3D12GetInterface");
    if (!fn) { rgpu_log("D3D12Core %p has no D3D12GetInterface export?!", (void *)core); return false; }
    if (InterlockedCompareExchange(&g_core_hooked, 1, 0) != 0) return true;
    /* Non-frozen inline hook: we install this at the moment D3D12Core loads (via the
     * DLL-load notification), before ANY thread has called D3D12GetInterface, so there
     * is nothing executing in the target to race with. */
    arm_inline_once(fn, (void *)h_CoreGetInterface, (void **)&o_CoreGetInterface, "D3D12Core!D3D12GetInterface");
    rgpu_log("D3D12Core HOOK %s: module=%p D3D12GetInterface @ %p trampoline=%p",
             o_CoreGetInterface ? "installed" : "FAILED", (void *)core, fn, (void *)o_CoreGetInterface);
    return o_CoreGetInterface != nullptr;
}
static bool hook_d3d12core_now() { return hook_d3d12core_mod(nullptr); }
/* Race-free capture: hook ntdll!LdrLoadDll - EVERY module load funnels through it,
 * including the Agility loader's private D3D12Core mapping (which bypasses
 * kernel32!LoadLibraryExW). When D3D12Core finishes loading we hook its
 * D3D12GetInterface using the EXACT base LdrLoadDll just returned, before the caller
 * (the Agility bootstrap) proceeds to invoke it. */
typedef struct _RGPU_UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } RGPU_UNICODE_STRING;
static bool uname_is_d3d12core(const RGPU_UNICODE_STRING *u) {
    if (!u || !u->Buffer) return false;
    size_t len = u->Length / sizeof(wchar_t);
    const wchar_t *b = u->Buffer, *want = L"d3d12core.dll"; const size_t wl = 13;
    if (len < wl) return false;
    const wchar_t *tail = b + (len - wl);
    for (size_t i = 0; i < wl; i++) { wchar_t c = tail[i]; if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a'); if (c != want[i]) return false; }
    if (len > wl) { wchar_t p = tail[-1]; if (p != L'\\' && p != L'/') return false; }
    return true;
}
/* ntdll DLL-load notification: the documented, hook-free way to observe module loads.
 * The callback fires synchronously while the loader still holds control (before
 * LdrLoadDll returns to the Agility bootstrap), giving us the D3D12Core base address so
 * we hook its D3D12GetInterface BEFORE the bootstrap invokes it. No prologue decoding of
 * the loader (LdrLoadDll's prologue is not relocatable by our conservative decoder). */
typedef struct _RGPU_LDR_DLL_NOTIFICATION_DATA {
    ULONG Flags;
    const RGPU_UNICODE_STRING *FullDllName;
    const RGPU_UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} RGPU_LDR_DLL_NOTIFICATION_DATA;
typedef VOID (CALLBACK *PRGPU_LDR_NOTIFY)(ULONG, const RGPU_LDR_DLL_NOTIFICATION_DATA *, PVOID);
typedef LONG (NTAPI *fnLdrRegisterDllNotification)(ULONG, PRGPU_LDR_NOTIFY, PVOID, PVOID *);
static PVOID g_ldr_cookie = nullptr;
static VOID CALLBACK rgpu_ldr_notify(ULONG reason, const RGPU_LDR_DLL_NOTIFICATION_DATA *data, PVOID) {
    /* reason 1 = LDR_DLL_NOTIFICATION_REASON_LOADED */
    if (reason != 1) return;
    if (data && data->DllBase && !o_CoreGetInterface && uname_is_d3d12core(data->BaseDllName)) {
        rgpu_log("DLL-notify: D3D12Core LOADED base=%p; hooking D3D12GetInterface before first use", data->DllBase);
        hook_d3d12core_mod((HMODULE)data->DllBase);
    }
    install_dxgi_hooks();   /* arm render-queue acquisition once dxgi.dll is present */
}
static void install_ldrloaddll_hook() {
    if (g_ldr_cookie) return;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    fnLdrRegisterDllNotification reg = nt ? (fnLdrRegisterDllNotification)GetProcAddress(nt, "LdrRegisterDllNotification") : nullptr;
    if (!reg) { rgpu_log("LdrRegisterDllNotification unavailable; relying on poll fallback"); return; }
    LONG st = reg(0, rgpu_ldr_notify, nullptr, &g_ldr_cookie);
    rgpu_log("LdrRegisterDllNotification -> st=0x%08lX cookie=%p (catches the Agility D3D12Core load)",
             (unsigned long)st, g_ldr_cookie);
}
static DWORD WINAPI rgpu_core_watcher(LPVOID) {
    hook_d3d12core_now();              /* in case it was already mapped before we hooked */
    for (int i = 0; i < 4000 && !o_CoreGetInterface; i++) Sleep(15);   /* keep alive as fallback */
    if (!o_CoreGetInterface) rgpu_log("core-watcher: D3D12Core!D3D12GetInterface never hooked within timeout");
    return 0;
}

extern "C" __declspec(dllexport) void WINAPI rgpu_d3d12_arm_external_device(ID3D12Device *dev, const char *source) {
    rgpu_log("external device arm source=%s device=%p vtbl=%p",
             source ? source : "unknown", (void *)dev, dev ? *(void **)dev : nullptr);
    arm_from_device(dev, source ? source : "external");
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
        /* Hook ntdll!LdrLoadDll NOW (synchronously, before the game calls any d3d12
         * export that triggers the Agility D3D12Core auto-load) so we catch that load
         * and hook D3D12Core!D3D12GetInterface - the real device-creation entry point
         * for UE5 titles like TXR - before its first use. The watcher is only a
         * best-effort fallback for the already-loaded case. */
        install_ldrloaddll_hook();
        install_dxgi_hooks();   /* dxgi.dll is imported early by D3D12 games; hook it now */
        HANDLE w = CreateThread(nullptr, 0, rgpu_core_watcher, nullptr, 0, nullptr);
        if (w) CloseHandle(w);
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
    rgpu_log("D3D12CreateDevice riid={%08lX-%04X-%04X-...} fl=0x%X adapter=%s -> hr=0x%08lX device=%p vtbl=%p",
             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3, (unsigned)fl,
             warp ? "WARP" : "hardware", (unsigned long)hr, ppDevice ? *ppDevice : nullptr,
             (ppDevice && *ppDevice) ? *(void **)*ppDevice : nullptr);
    /* SHADOW-patch every real (non-WARP) device in place (idempotent - Agility devices
     * are already patched at the CoreModule CreateDevice boundary; this covers the
     * classic runtime path). We return the UNCHANGED pointer so internal code keeps the
     * concrete object. NOTE: `fl` is the MINIMUM feature level (a floor), not the
     * device's capability, so we never gate on it. */
    if (SUCCEEDED(hr) && ppDevice && *ppDevice && !warp) shadow_patch_device(*ppDevice);
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
    /* Normal Agility path via the app-level d3d12.dll:
     * D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_ID3D12SDKConfiguration1)
     *   -> ID3D12SDKConfiguration1::CreateDeviceFactory -> ID3D12DeviceFactory::CreateDevice.
     * (TXR skips this and calls D3D12Core!D3D12GetInterface directly - see the core hook.) */
    if (SUCCEEDED(hr)) rgpu_after_get_interface(rclsid, ppvDebug);
    return hr;
}

__declspec(dllexport) void WINAPI rgpu_d3d12_path_stats(long *sdkConfigHooks, long *sdkFactoryCalls,
                                                        long *factoryDeviceCalls) {
    if (sdkConfigHooks) *sdkConfigHooks = g_sdk_config_hooks;
    if (sdkFactoryCalls) *sdkFactoryCalls = g_sdk_factory_calls;
    if (factoryDeviceCalls) *factoryDeviceCalls = g_factory_device_calls;
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
