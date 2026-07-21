/* rgpu D3D12 COM wrapper object-graph (the deterministic interceptor).
 *
 * Vtable/inline hooking cannot capture Tokyo Xtreme Racer: the game creates its
 * real RHI device through a path that never touches the device objects we hook, so
 * none of our hooked methods fire. The fix is to hand the game OUR object. From
 * D3D12CreateDevice (and the Agility ID3D12DeviceFactory::CreateDevice) we return an
 * RgpuD3D12Device wrapper. Because the game holds the wrapper, every call is
 * guaranteed to pass through us; there is no path to route around it.
 *
 * IDENTITY is preserved so the game cannot obtain a raw underlying interface:
 *   - QueryInterface for any supported device revision (ID3D12Device .. Device10)
 *     returns `this` (the same wrapper), never the real pointer. We verify the real
 *     object actually supports the revision before claiming it, so behaviour matches.
 *   - Every object the device hands back is itself wrapped: CreateCommandQueue/1 and
 *     CreateCommandList/1 return RgpuD3D12CommandQueue / RgpuD3D12GraphicsCommandList;
 *     GetDevice on a queue/list returns the device wrapper, not the raw device.
 *
 * CAPTURE happens at the natural D3D12 boundaries: recording methods on the list
 * wrapper tee each call into the rgpu protocol as the app builds the list; Close
 * finalizes it; the queue wrapper tees + submits at ExecuteCommandLists. Because the
 * real runtime must receive REAL command lists, ExecuteCommandLists UNWRAPS each list
 * (via a private IID) back to the underlying object before forwarding.
 *
 * This is pass 1: local pass-through + tee (wrap device/queue/list; resources, heaps,
 * fences, PSOs still pass through as real objects — they are only referenced, never
 * re-entered, so pass-through is safe). Handle translation + resource wrapping (for
 * remote replay) is the next pass; see README.
 *
 * ABI NOTE: compiled with WIDL_EXPLICIT_AGGREGATE_RETURNS so aggregate-return vtable
 * slots (GetAdapterLuid/GetResourceAllocationInfo/GetCustomHeapProperties) use the
 * hidden *__ret pointer form that matches the MSVC-built D3D12Core exactly. The
 * forwarders come from tools/gen_wrappers.py -> rgpu_d3d12_wrappers_gen.h. */
#ifndef RGPU_D3D12_WRAPPERS_H
#define RGPU_D3D12_WRAPPERS_H
#include "rgpu_d3d12_wrappers_gen.h"

/* Private IID: QI(IID_RgpuUnwrap) on any of our wrappers yields the real underlying
 * pointer (no AddRef) so we can translate wrapper -> real before calling the runtime. */
static const GUID IID_RgpuUnwrap =
    {0x52475055, 0x0d12, 0x4711, {0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};

static inline bool rgpu_real_supports(IUnknown *real, REFIID riid) {
    void *t = nullptr;
    if (real && SUCCEEDED(real->QueryInterface(riid, &t)) && t) { ((IUnknown *)t)->Release(); return true; }
    return false;
}
static inline void rgpu_first(volatile LONG *g, const char *msg) {
    if (InterlockedCompareExchange(g, 1, 0) == 0) rgpu_log("%s", msg);
}
static volatile LONG g_dev_qi_leak = 0;   /* diagnostic: device QI fall-throughs (raw leaks) */
static volatile LONG g_dev_qi_hit = 0;    /* diagnostic: device QI served by the wrapper */

class RgpuD3D12Device;   /* forward */

/* ============================ command-list wrapper ======================== */
class RgpuD3D12GraphicsCommandList : public ID3D12GraphicsCommandList7 {
    ID3D12GraphicsCommandList7 *real_;   /* stored as the top type; higher-version
                                          * methods are only reachable after a gated QI */
    ID3D12Device *owner_;                /* the device WRAPPER (is-a ID3D12Device) */
    volatile LONG ref_;
public:
    RgpuD3D12GraphicsCommandList(void *real, ID3D12Device *owner)
        : real_((ID3D12GraphicsCommandList7 *)real), owner_(owner), ref_(1) {}

    ID3D12CommandList *rgpu_real_list() { return (ID3D12CommandList *)real_; }

    static bool is_list_iid(REFIID r) {
        return IsEqualGUID(r, __uuidof(IUnknown)) || IsEqualGUID(r, __uuidof(ID3D12Object)) ||
               IsEqualGUID(r, __uuidof(ID3D12DeviceChild)) || IsEqualGUID(r, __uuidof(ID3D12CommandList)) ||
               IsEqualGUID(r, __uuidof(ID3D12GraphicsCommandList))  || IsEqualGUID(r, __uuidof(ID3D12GraphicsCommandList1)) ||
               IsEqualGUID(r, __uuidof(ID3D12GraphicsCommandList2)) || IsEqualGUID(r, __uuidof(ID3D12GraphicsCommandList3)) ||
               IsEqualGUID(r, __uuidof(ID3D12GraphicsCommandList4)) || IsEqualGUID(r, __uuidof(ID3D12GraphicsCommandList5)) ||
               IsEqualGUID(r, __uuidof(ID3D12GraphicsCommandList6)) || IsEqualGUID(r, __uuidof(ID3D12GraphicsCommandList7));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_RgpuUnwrap)) { *ppv = (ID3D12CommandList *)real_; return S_OK; }
        if (is_list_iid(riid) && rgpu_real_supports(real_, riid)) {
            AddRef(); *ppv = static_cast<ID3D12GraphicsCommandList7 *>(this); return S_OK;
        }
        return real_->QueryInterface(riid, ppv);   /* non-chain (debug/introspection) */
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&ref_);
        if (c == 0) { real_->Release(); delete this; }
        return (ULONG)c;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppv) override {
        return owner_ ? owner_->QueryInterface(riid, ppv) : real_->GetDevice(riid, ppv);
    }

    /* ---- recording tee (mirror of the proven hook bodies) ---- */
    void STDMETHODCALLTYPE DrawInstanced(UINT vc, UINT ic, UINT sv, UINT si) override {
        if (InterlockedIncrement(&g_draws) == 1) rgpu_log("WRAPPER FIRST DrawInstanced -> live list capture is ON");
        rgpu_args_draw a{vc, sv, ic, si}; tee(RGPU_CMD_DRAW, oid(this), &a, sizeof(a));
        real_->DrawInstanced(vc, ic, sv, si);
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT ic, UINT inst, UINT si, INT bv, UINT sinst) override {
        InterlockedIncrement(&g_draws);
        rgpu_args_draw_indexed a{ic, si, bv, inst, sinst}; tee(RGPU_CMD_DRAW_INDEXED, oid(this), &a, sizeof(a));
        real_->DrawIndexedInstanced(ic, inst, si, bv, sinst);
    }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override {
        InterlockedIncrement(&g_dispatch);
        rgpu_args_dispatch a{x, y, z}; tee(RGPU_CMD_DISPATCH, oid(this), &a, sizeof(a));
        real_->Dispatch(x, y, z);
    }
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *db, UINT64 doff, ID3D12Resource *sb, UINT64 soff, UINT64 n) override {
        InterlockedIncrement(&g_copies); tee(RGPU_CMD_COPY_RESOURCE, oid(db), nullptr, 0);
        real_->CopyBufferRegion(db, doff, sb, soff, n);
    }
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *d, UINT dx, UINT dy, UINT dz, const D3D12_TEXTURE_COPY_LOCATION *s, const D3D12_BOX *b) override {
        InterlockedIncrement(&g_copies); tee(RGPU_CMD_COPY_RESOURCE, 0, nullptr, 0);
        real_->CopyTextureRegion(d, dx, dy, dz, s, b);
    }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource *dst, ID3D12Resource *src) override {
        InterlockedIncrement(&g_copies); tee(RGPU_CMD_COPY_RESOURCE, oid(dst), nullptr, 0);
        real_->CopyResource(dst, src);
    }
    void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER *b) override {
        InterlockedIncrement(&g_barriers); uint32_t nn = n; tee(RGPU_CMD_RESOURCE_BARRIER, oid(this), &nn, sizeof(nn));
        real_->ResourceBarrier(n, b);
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rt, WINBOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE *ds) override {
        InterlockedIncrement(&g_rts);
        rgpu_args_set_render_targets a{n, (n && rt) ? (uint32_t)rt[0].ptr : 0u, ds ? (uint32_t)ds->ptr : 0u};
        tee(RGPU_CMD_SET_RENDER_TARGETS, oid(this), &a, sizeof(a));
        real_->OMSetRenderTargets(n, rt, single, ds);
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c[4], UINT nr, const D3D12_RECT *r) override {
        InterlockedIncrement(&g_clears);
        rgpu_args_clear_rtv a; std::memcpy(a.rgba, c, sizeof(a.rgba)); tee(RGPU_CMD_CLEAR_RTV, (uint32_t)h.ptr, &a, sizeof(a));
        real_->ClearRenderTargetView(h, c, nr, r);
    }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *sig, UINT maxCount, ID3D12Resource *args, UINT64 ao, ID3D12Resource *cnt, UINT64 co) override {
        InterlockedIncrement(&g_draws); uint32_t m = maxCount; tee(RGPU_CMD_DRAW, oid(this), &m, sizeof(m));
        real_->ExecuteIndirect(sig, maxCount, args, ao, cnt, co);
    }
    void STDMETHODCALLTYPE DispatchMesh(UINT x, UINT y, UINT z) override {
        if (InterlockedIncrement(&g_dispatch) == 1) rgpu_log("WRAPPER FIRST DispatchMesh -> capturing Nanite/mesh work");
        rgpu_args_dispatch a{x, y, z}; tee(RGPU_CMD_DISPATCH, oid(this), &a, sizeof(a));
        real_->DispatchMesh(x, y, z);
    }
    void STDMETHODCALLTYPE Barrier(UINT32 nGroups, const D3D12_BARRIER_GROUP *groups) override {
        if (InterlockedIncrement(&g_barriers) == 1) rgpu_log("WRAPPER FIRST Barrier (enhanced/GCL7) -> capturing UE5 barrier stream");
        uint32_t n = nGroups; tee(RGPU_CMD_RESOURCE_BARRIER, oid(this), &n, sizeof(n));
        real_->Barrier(nGroups, groups);
    }
    HRESULT STDMETHODCALLTYPE Close() override {
        rgpu_first(&close_once_, "WRAPPER FIRST Close -> list finalized (a submittable unit tee'd)");
        return real_->Close();
    }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso) override {
        return real_->Reset(alloc, pso);   /* allocator/pso are real (unwrapped) */
    }

    RGPU_GCL_FORWARDERS   /* all remaining methods: forward verbatim to real_ */
private:
    volatile LONG close_once_ = 0;
};

/* ============================ command-queue wrapper ======================= */
class RgpuD3D12CommandQueue : public ID3D12CommandQueue {
    ID3D12CommandQueue *real_;
    ID3D12Device *owner_;
    volatile LONG ref_;
public:
    RgpuD3D12CommandQueue(void *real, ID3D12Device *owner)
        : real_((ID3D12CommandQueue *)real), owner_(owner), ref_(1) {}

    static bool is_queue_iid(REFIID r) {
        return IsEqualGUID(r, __uuidof(IUnknown)) || IsEqualGUID(r, __uuidof(ID3D12Object)) ||
               IsEqualGUID(r, __uuidof(ID3D12DeviceChild)) || IsEqualGUID(r, __uuidof(ID3D12Pageable)) ||
               IsEqualGUID(r, __uuidof(ID3D12CommandQueue));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_RgpuUnwrap)) { *ppv = real_; return S_OK; }
        if (is_queue_iid(riid) && rgpu_real_supports(real_, riid)) {
            AddRef(); *ppv = static_cast<ID3D12CommandQueue *>(this); return S_OK;
        }
        return real_->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&ref_);
        if (c == 0) { real_->Release(); delete this; }
        return (ULONG)c;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppv) override {
        return owner_ ? owner_->QueryInterface(riid, ppv) : real_->GetDevice(riid, ppv);
    }

    /* THE submission boundary. Translate our list wrappers back to real lists (the
     * runtime must receive real objects) and tee the submission. */
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n, ID3D12CommandList *const *pp) override {
        LONG e = InterlockedIncrement(&g_exec);
        InterlockedExchangeAdd(&g_lists, (LONG)n);
        uint32_t nn = n; tee(RGPU_CMD_EXECUTE_COMMAND_LISTS, 0, &nn, sizeof(nn));

        ID3D12CommandList *stackbuf[16];
        ID3D12CommandList **real_lists = (n <= 16) ? stackbuf
                                        : (ID3D12CommandList **)malloc(sizeof(void *) * n);
        if (real_lists) {
            for (UINT i = 0; i < n; i++) {
                ID3D12CommandList *rl = pp[i]; void *raw = nullptr;
                if (pp[i] && SUCCEEDED(pp[i]->QueryInterface(IID_RgpuUnwrap, &raw)) && raw)
                    rl = (ID3D12CommandList *)raw;   /* wrapper -> real */
                real_lists[i] = rl;
            }
            real_->ExecuteCommandLists(n, real_lists);
            if (real_lists != stackbuf) free(real_lists);
        } else {
            real_->ExecuteCommandLists(n, pp);   /* alloc failure: submit as given */
        }
        if (e == 1 || (e % 600) == 0) {
            EnterCriticalSection(&g_cs); uint32_t bytes = g_bw.len; LeaveCriticalSection(&g_cs);
            rgpu_log("WRAPPER submit#%ld lists=%ld | tee draws=%ld dispatch=%ld barriers=%ld clears=%ld setRT=%ld copies=%ld | batch=%u bytes",
                     e, g_lists, g_draws, g_dispatch, g_barriers, g_clears, g_rts, g_copies, bytes);
        }
    }

    RGPU_QUEUE_FORWARDERS
};

/* ============================ device wrapper ============================== */
class RgpuD3D12Device : public ID3D12Device10 {
    ID3D12Device10 *real_;   /* stored as top type; gated QI protects higher methods */
    volatile LONG ref_;

    HRESULT wrap_queue(HRESULT hr, void **pp) {
        if (SUCCEEDED(hr) && pp && *pp) {
            rgpu_first(&q_once_, "WRAPPER device created a wrapped command QUEUE");
            *pp = static_cast<ID3D12CommandQueue *>(new RgpuD3D12CommandQueue(*pp, this));
        }
        return hr;
    }
    HRESULT wrap_list(HRESULT hr, void **pp) {
        if (SUCCEEDED(hr) && pp && *pp) {
            rgpu_first(&l_once_, "WRAPPER device created a wrapped command LIST");
            *pp = static_cast<ID3D12GraphicsCommandList7 *>(new RgpuD3D12GraphicsCommandList(*pp, this));
        }
        return hr;
    }
public:
    RgpuD3D12Device(void *real) : real_((ID3D12Device10 *)real), ref_(1) {}

    static bool is_device_iid(REFIID r) {
        return IsEqualGUID(r, __uuidof(IUnknown))  || IsEqualGUID(r, __uuidof(ID3D12Object)) ||
               IsEqualGUID(r, __uuidof(ID3D12Device))  || IsEqualGUID(r, __uuidof(ID3D12Device1)) ||
               IsEqualGUID(r, __uuidof(ID3D12Device2)) || IsEqualGUID(r, __uuidof(ID3D12Device3)) ||
               IsEqualGUID(r, __uuidof(ID3D12Device4)) || IsEqualGUID(r, __uuidof(ID3D12Device5)) ||
               IsEqualGUID(r, __uuidof(ID3D12Device6)) || IsEqualGUID(r, __uuidof(ID3D12Device7)) ||
               IsEqualGUID(r, __uuidof(ID3D12Device8)) || IsEqualGUID(r, __uuidof(ID3D12Device9)) ||
               IsEqualGUID(r, __uuidof(ID3D12Device10));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_RgpuUnwrap)) { *ppv = real_; return S_OK; }
        if (is_device_iid(riid) && rgpu_real_supports(real_, riid)) {
            LONG h = InterlockedIncrement(&g_dev_qi_hit);
            if (h <= 16) rgpu_log("DEVICE QI HIT #%ld this=%p riid.Data1=%08lX (game IS using our wrapper)", h, (void *)this, (unsigned long)riid.Data1);
            AddRef(); *ppv = static_cast<ID3D12Device10 *>(this); return S_OK;
        }
        /* A device revision newer than ID3D12Device10 (mingw's max) is forwarded raw
         * here - the bypass on recent Agility SDKs, where UE5 QIs for Device11-14. Log
         * the first several so we know exactly which revisions to cover (needs newer
         * headers - vendor DirectX-Headers; see README). */
        LONG k = InterlockedIncrement(&g_dev_qi_leak);
        if (k <= 24) {
            bool sup = rgpu_real_supports(real_, riid);
            rgpu_log("DEVICE QI LEAK #%ld riid={%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X} real_supports=%d",
                     k, (unsigned long)riid.Data1, riid.Data2, riid.Data3,
                     riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                     riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7], sup);
        }
        return real_->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&ref_);
        if (c == 0) { real_->Release(); delete this; }
        return (ULONG)c;
    }

    /* ---- wrap everything on the command path ---- */
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *d, REFIID riid, void **pp) override {
        return wrap_queue(real_->CreateCommandQueue(d, riid, pp), pp);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC *d, REFIID creator, REFIID riid, void **pp) override {
        return wrap_queue(real_->CreateCommandQueue1(d, creator, riid, pp), pp);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nm, D3D12_COMMAND_LIST_TYPE ty, ID3D12CommandAllocator *a, ID3D12PipelineState *ps, REFIID riid, void **pp) override {
        return wrap_list(real_->CreateCommandList(nm, ty, a, ps, riid, pp), pp);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT nm, D3D12_COMMAND_LIST_TYPE ty, D3D12_COMMAND_LIST_FLAGS f, REFIID riid, void **pp) override {
        return wrap_list(real_->CreateCommandList1(nm, ty, f, riid, pp), pp);
    }

    RGPU_DEVICE_FORWARDERS
private:
    volatile LONG q_once_ = 0, l_once_ = 0;
};

/* Wrap a freshly-created real device, returning the wrapper via the requested riid.
 * Single inheritance makes the wrapper pointer valid for any ID3D12Device revision. */
static void *rgpu_wrap_device(void *realDevice, REFIID riid) {
    (void)riid;
    return static_cast<ID3D12Device10 *>(new RgpuD3D12Device(realDevice));
}

#endif /* RGPU_D3D12_WRAPPERS_H */
