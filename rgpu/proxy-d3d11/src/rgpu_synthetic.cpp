/* rgpu synthetic DXGI adapter + fail-closed policy — implementation. */
#include "rgpu_synthetic.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

rgpu_policy   g_rgpu_policy = {1, 0, 0, 1};
volatile long g_local_hw_devices_created = 0;   /* invariant: never incremented */
volatile long g_local_hw_attempts_refused = 0;
volatile long g_remote_devices_created = 0;
int           g_rgpu_remote_connected = 0;

static rgpu_caps g_caps;
static int       g_caps_set = 0;

rgpu_caps rgpu_default_caps(void) {
    rgpu_caps c;
    std::memset(&c, 0, sizeof(c));
    std::wcscpy(c.description, L"Remote GPU - NVIDIA T4");
    c.vendor_id = 0x10DE;              /* NVIDIA */
    c.device_id = 0x1EB8;             /* T4 */
    c.dedicated_vram = (uint64_t)15360 * 1024 * 1024;
    c.feature_level = D3D_FEATURE_LEVEL_11_1;
    c.location_remote = 1;
    return c;
}

void rgpu_set_caps(const rgpu_caps *caps) { g_caps = *caps; g_caps_set = 1; }
static const rgpu_caps &caps() { if (!g_caps_set) { g_caps = rgpu_default_caps(); g_caps_set = 1; } return g_caps; }

void rgpu_guard_local_hardware(const char *where) {
    /* A code path tried to touch local hardware -> refuse it (fail closed).
     * g_local_hw_devices_created is NEVER incremented: no local device is created. */
    g_local_hw_attempts_refused++;
    std::fprintf(stderr, "[rgpu] FAIL-CLOSED: refused local hardware device at %s "
                         "(remote_required=%d, local_fallback=%d, warp_fallback=%d)\n",
                 where, g_rgpu_policy.remote_required, g_rgpu_policy.local_hardware_fallback,
                 g_rgpu_policy.warp_game_fallback);
    if (g_rgpu_policy.debug_abort_on_local_hw) std::abort();
}

/* ---------------- synthetic IDXGIAdapter1 -------------------------------- */
class RgpuAdapter : public IDXGIAdapter1 {
    long ref_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1)) {
            *ppv = static_cast<IDXGIAdapter1 *>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { long r = --ref_; if (r == 0) delete this; return (ULONG)r; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **ppParent) override { *ppParent = nullptr; return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT, IDXGIOutput **ppOutput) override { *ppOutput = nullptr; return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *pDesc) override {
        std::memset(pDesc, 0, sizeof(*pDesc));
        std::wcsncpy(pDesc->Description, caps().description, 127);
        pDesc->VendorId = caps().vendor_id; pDesc->DeviceId = caps().device_id;
        pDesc->DedicatedVideoMemory = (SIZE_T)caps().dedicated_vram;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER *pUMD) override {
        if (pUMD) pUMD->QuadPart = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *pDesc) override {
        std::memset(pDesc, 0, sizeof(*pDesc));
        std::wcsncpy(pDesc->Description, caps().description, 127);
        pDesc->VendorId = caps().vendor_id; pDesc->DeviceId = caps().device_id;
        pDesc->DedicatedVideoMemory = (SIZE_T)caps().dedicated_vram;
        pDesc->Flags = 0; /* not a software/remote-hidden adapter from the game's view */
        return S_OK;
    }
};

/* ---------------- synthetic IDXGIFactory1 ------------------------------- */
class RgpuFactory : public IDXGIFactory1 {
    long ref_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1)) {
            *ppv = static_cast<IDXGIFactory1 *>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { long r = --ref_; if (r == 0) delete this; return (ULONG)r; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **ppParent) override { *ppParent = nullptr; return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter **ppAdapter) override {
        if (i != 0) { *ppAdapter = nullptr; return DXGI_ERROR_NOT_FOUND; }
        *ppAdapter = new RgpuAdapter(); return S_OK;   /* ONLY the synthetic remote adapter */
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pHwnd) override { if (pHwnd) *pHwnd = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **ppSC) override {
        *ppSC = nullptr; return E_NOTIMPL;  /* swapchain -> remote present, wired in the renderer phase */
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter **ppAdapter) override {
        /* WARP / software adapter requested -> fail closed. */
        rgpu_guard_local_hardware("IDXGIFactory::CreateSoftwareAdapter");
        *ppAdapter = nullptr; return DXGI_ERROR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1 **ppAdapter) override {
        if (i != 0) { *ppAdapter = nullptr; return DXGI_ERROR_NOT_FOUND; }
        *ppAdapter = new RgpuAdapter(); return S_OK;
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return TRUE; }
};

extern "C" {

HRESULT rgpu_CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    RgpuFactory *f = new RgpuFactory();
    HRESULT hr = f->QueryInterface(riid, ppFactory);
    f->Release();
    return hr;
}
HRESULT rgpu_CreateDXGIFactory(REFIID riid, void **ppFactory) { return rgpu_CreateDXGIFactory1(riid, ppFactory); }

HRESULT rgpu_D3D11CreateDevice(
    IDXGIAdapter *, D3D_DRIVER_TYPE DriverType, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext) {
    if (ppDevice) *ppDevice = nullptr;
    if (ppImmediateContext) *ppImmediateContext = nullptr;
    /* Fail closed: a hardware or WARP driver type must NEVER create a local
     * device. Only the remote renderer may back a device. */
    if (DriverType == D3D_DRIVER_TYPE_HARDWARE || DriverType == D3D_DRIVER_TYPE_WARP ||
        DriverType == D3D_DRIVER_TYPE_REFERENCE) {
        if (!g_rgpu_policy.local_hardware_fallback) {
            /* We do NOT call the system D3D11CreateDevice. */
            if (!g_rgpu_remote_connected) return DXGI_ERROR_DEVICE_REMOVED; /* remote required */
        } else {
            rgpu_guard_local_hardware("rgpu_D3D11CreateDevice(hardware)");
        }
    }
    if (!g_rgpu_remote_connected) return DXGI_ERROR_DEVICE_REMOVED;
    /* Remote session live -> a remote-backed device would be constructed here
     * (command serialization, next phase). Count it and report the feature level. */
    g_remote_devices_created++;
    if (pFeatureLevel) *pFeatureLevel = (D3D_FEATURE_LEVEL)caps().feature_level;
    return E_NOTIMPL; /* remote device object not yet implemented (renderer phase) */
}

} /* extern "C" */
