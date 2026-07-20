/* rgpu d3d11 proxy DLL — the game-facing injection point (Phase-1, pass-through+tee).
 *
 * Built as `d3d11.dll` and placed next to a game's .exe, Windows' DLL search
 * order loads it before System32\d3d11.dll. It intercepts the device-creation
 * exports (D3D11CreateDevice / D3D11CreateDeviceAndSwapChain), calls the REAL
 * d3d11 to build a genuine device on the local GPU, then hands the game an
 * RgpuD3D11Device WRAPPER around it. The game boots and runs normally, but every
 * ID3D11Device call now passes through our layer, and the covered creates are
 * tee'd into the rgpu protocol. This is the reference configuration that proves
 * a real game accepts our device without incompatible-driver errors or kicks.
 *
 * NOTE: this is the pass-through set. It does NOT inject the synthetic-adapter
 * dxgi proxy — that is the separate fail-closed enumeration configuration. Here
 * the goal is: real game + our COM layer + local GPU, no refusal. The production
 * build swaps the real device for the remote renderer over the transport.
 *
 * (Test build is rgpu_d3d11.dll loaded explicitly; deployment renames to
 * d3d11.dll beside the game.) */
#include "rgpu_d3d11_device.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#ifndef D3D11_CREATE_DEVICE_DEBUG
#define D3D11_CREATE_DEVICE_DEBUG 0x2
#endif

static HMODULE g_real = nullptr;

/* Evidence trail so a real game's device creation is observable from outside the
 * process. Written to %TEMP%\rgpu_proxy.log (always writable, unlike a Program
 * Files game dir). This is how we confirm a game actually booted THROUGH us. */
static void rgpu_log(const char *fmt, ...) {
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (!n || n >= MAX_PATH - 20) return;
    std::strcat(dir, "rgpu_proxy.log");
    FILE *f = std::fopen(dir, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); std::vfprintf(f, fmt, ap); va_end(ap);
    std::fprintf(f, "\n");
    std::fclose(f);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n && n < MAX_PATH - 16) {
            std::strcat(path, "\\d3d11.dll");
            g_real = LoadLibraryA(path);   // the genuine d3d11 for the reference device
        }
        char exe[MAX_PATH] = {0}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        rgpu_log("ATTACH host=\"%s\" real_d3d11=%s", exe, g_real ? "loaded" : "MISSING");
    }
    return TRUE;
}

static FARPROC real(const char *name) { return g_real ? GetProcAddress(g_real, name) : nullptr; }

/* Wrap a freshly-created real device in-place so the caller receives the proxy. */
static void wrap_device(ID3D11Device **ppDevice) {
    if (ppDevice && *ppDevice) {
        ID3D11Device *realDev = *ppDevice;
        *ppDevice = new RgpuD3D11Device(realDev); // AddRefs realDev
        realDev->Release();                        // wrapper now owns the reference
        g_rgpu_dev_wrapped++;
        rgpu_log("WRAP device #%ld -> game now runs through rgpu ID3D11Device wrapper", g_rgpu_dev_wrapped);
    }
}

/* Wrap the device AND substitute the immediate context the create call returned
 * with the wrapper's (same instance GetImmediateContext hands out), so a game
 * that keeps the context from CreateDevice still runs through our context vtable. */
static void wrap_device_and_context(ID3D11Device **ppDevice, ID3D11DeviceContext **ppCtx) {
    wrap_device(ppDevice);
    if (ppDevice && *ppDevice && ppCtx && *ppCtx) {
        (*ppCtx)->Release();                        // drop the real immediate context the runtime returned
        (*ppDevice)->GetImmediateContext(ppCtx);    // hand back the wrapped one
    }
}

typedef HRESULT(WINAPI *pD3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *pD3D11CreateDeviceAndSwapChain)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

extern "C" {

__declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext) {
    pD3D11CreateDevice fn = (pD3D11CreateDevice)real("D3D11CreateDevice");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                    SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
    if (FAILED(hr) && (Flags & D3D11_CREATE_DEVICE_DEBUG)) {          // debug layer absent → retry clean
        hr = fn(pAdapter, DriverType, Software, Flags & ~D3D11_CREATE_DEVICE_DEBUG, pFeatureLevels,
                FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
    }
    if (SUCCEEDED(hr)) wrap_device_and_context(ppDevice, ppImmediateContext);
    return hr;
}

__declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
    const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain,
    ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext) {
    pD3D11CreateDeviceAndSwapChain fn = (pD3D11CreateDeviceAndSwapChain)real("D3D11CreateDeviceAndSwapChain");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                    pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    if (FAILED(hr) && (Flags & D3D11_CREATE_DEVICE_DEBUG)) {
        hr = fn(pAdapter, DriverType, Software, Flags & ~D3D11_CREATE_DEVICE_DEBUG, pFeatureLevels,
                FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
                ppImmediateContext);
    }
    if (SUCCEEDED(hr)) wrap_device_and_context(ppDevice, ppImmediateContext);
    return hr;
}

/* Rarely game-imported core/layered exports — forward verbatim so the export
 * table is complete for any loader that references them. */
__declspec(dllexport) HRESULT WINAPI D3D11CoreCreateDevice(
    void *pFactory, IDXGIAdapter *pAdapter, UINT Flags,
    const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, ID3D11Device **ppDevice) {
    typedef HRESULT(WINAPI *fn_t)(void *, IDXGIAdapter *, UINT, const D3D_FEATURE_LEVEL *, UINT, ID3D11Device **);
    fn_t fn = (fn_t)real("D3D11CoreCreateDevice");
    if (!fn) return E_NOTIMPL;
    HRESULT hr = fn(pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevels, ppDevice);
    if (SUCCEEDED(hr)) wrap_device(ppDevice);
    return hr;
}
__declspec(dllexport) SIZE_T WINAPI D3D11CoreGetLayeredDeviceSize(const void *pLayers, UINT NumLayers) {
    typedef SIZE_T(WINAPI *fn_t)(const void *, UINT);
    fn_t fn = (fn_t)real("D3D11CoreGetLayeredDeviceSize");
    return fn ? fn(pLayers, NumLayers) : 0;
}
__declspec(dllexport) HRESULT WINAPI D3D11CoreRegisterLayers(const void *pLayers, UINT NumLayers) {
    typedef HRESULT(WINAPI *fn_t)(const void *, UINT);
    fn_t fn = (fn_t)real("D3D11CoreRegisterLayers");
    return fn ? fn(pLayers, NumLayers) : E_NOTIMPL;
}

/* Introspection for the harness: how many real devices/contexts we wrapped and
 * how much of the game's device + per-frame activity the tee serialized. */
__declspec(dllexport) void WINAPI rgpu_proxy_stats(
    long *devWrapped, long *teeTex2d, long *teeRtv, unsigned *teeCommands, unsigned *teeBatchBytes) {
    if (devWrapped)   *devWrapped   = g_rgpu_dev_wrapped;
    if (teeTex2d)     *teeTex2d     = g_rgpu_tee_texture2d;
    if (teeRtv)       *teeRtv       = g_rgpu_tee_rtv;
    if (teeCommands)  *teeCommands  = rgpu_tee().commands_recorded();
    if (teeBatchBytes)*teeBatchBytes= (unsigned)rgpu_tee().BuildBatch().size();
}

__declspec(dllexport) void WINAPI rgpu_proxy_ctx_stats(
    long *ctxWrapped, long *teeDraws, long *teeClears, long *teeState) {
    if (ctxWrapped) *ctxWrapped = g_rgpu_ctx_wrapped;
    if (teeDraws)   *teeDraws   = g_rgpu_tee_draws;
    if (teeClears)  *teeClears  = g_rgpu_tee_clears;
    if (teeState)   *teeState   = g_rgpu_tee_state;
}

} /* extern "C" */
