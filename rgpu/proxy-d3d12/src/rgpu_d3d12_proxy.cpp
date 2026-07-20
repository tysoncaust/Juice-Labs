/* rgpu d3d12 proxy DLL — the D3D12 injection point (Phase-1, transparent pass-through).
 *
 * Built as `d3d12.dll` and placed next to a D3D12 game's .exe (e.g. Tokyo Xtreme
 * Racer, UE5). Windows' app-dir DLL precedence loads this before System32\d3d12.dll
 * (d3d12.dll is NOT a KnownDLL). It exports the loader-surface functions a D3D12
 * game imports and forwards each to the REAL System32\d3d12.dll. The two
 * device-creation entry points (D3D12CreateDevice, D3D12GetInterface) are logged
 * so we can see the game create its device THROUGH our layer.
 *
 * AGILITY SDK: TXR ships the Agility runtime (its exe exports D3D12SDKVersion/
 * D3D12SDKPath and carries D3D12\x64\D3D12Core.dll). We do NOT touch D3D12Core.dll
 * or the SDK selection. Because we forward D3D12CreateDevice/D3D12GetInterface to
 * the system d3d12.dll, that loader still reads the EXE's exports and boots the
 * Agility runtime exactly as before — the SDK selection is preserved.
 *
 * This is step 1 of the user's sequence: forward unchanged, prove TXR boots
 * through the D3D12 layer. Step 2 (wrap device/queue/command-list/swap-chain and
 * serialize) builds on this once boot is confirmed.
 *
 * (Test build is rgpu_d3d12.dll loaded explicitly; deployment renames to
 * d3d12.dll beside the game.) */
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static HMODULE g_real = nullptr;

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

/* Lazily load the genuine System32\d3d12.dll (NOT in DllMain — avoids loader lock). */
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
        char exe[MAX_PATH] = {0}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        rgpu_log("ATTACH host=\"%s\" (rgpu d3d12 pass-through proxy)", exe);
    }
    return TRUE;
}

extern "C" {

/* ---- intercepted device-creation entry points (log + forward) ----------- */
__declspec(dllexport) HRESULT WINAPI D3D12CreateDevice(
    void *pAdapter, int MinimumFeatureLevel, const GUID &riid, void **ppDevice) {
    typedef HRESULT(WINAPI *fn_t)(void *, int, const GUID &, void **);
    fn_t fn = (fn_t)real("D3D12CreateDevice");
    if (!fn) { rgpu_log("D3D12CreateDevice: real d3d12.dll MISSING"); return E_FAIL; }
    HRESULT hr = fn(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    rgpu_log("D3D12CreateDevice fl=0x%X -> hr=0x%08lX device=%p (game creating its D3D12 device through rgpu)",
             (unsigned)MinimumFeatureLevel, (unsigned long)hr, ppDevice ? *ppDevice : nullptr);
    return hr; /* step 1: return the real device unchanged (pass-through) */
}

__declspec(dllexport) HRESULT WINAPI D3D12GetInterface(const GUID &rclsid, const GUID &riid, void **ppvDebug) {
    typedef HRESULT(WINAPI *fn_t)(const GUID &, const GUID &, void **);
    fn_t fn = (fn_t)real("D3D12GetInterface");
    if (!fn) return E_NOTIMPL;
    HRESULT hr = fn(rclsid, riid, ppvDebug);
    rgpu_log("D3D12GetInterface -> hr=0x%08lX obj=%p (Agility DeviceFactory/SDKConfiguration path)",
             (unsigned long)hr, ppvDebug ? *ppvDebug : nullptr);
    return hr;
}

/* ---- forwarded loader-surface exports a D3D12 game imports by name -------- */
__declspec(dllexport) HRESULT WINAPI D3D12GetDebugInterface(const GUID &riid, void **ppvDebug) {
    typedef HRESULT(WINAPI *fn_t)(const GUID &, void **);
    fn_t fn = (fn_t)real("D3D12GetDebugInterface"); return fn ? fn(riid, ppvDebug) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12SerializeRootSignature(
    const void *pRootSignature, int Version, void **ppBlob, void **ppErrorBlob) {
    typedef HRESULT(WINAPI *fn_t)(const void *, int, void **, void **);
    fn_t fn = (fn_t)real("D3D12SerializeRootSignature"); return fn ? fn(pRootSignature, Version, ppBlob, ppErrorBlob) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const void *pRootSignature, void **ppBlob, void **ppErrorBlob) {
    typedef HRESULT(WINAPI *fn_t)(const void *, void **, void **);
    fn_t fn = (fn_t)real("D3D12SerializeVersionedRootSignature"); return fn ? fn(pRootSignature, ppBlob, ppErrorBlob) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    const void *p, SIZE_T n, const GUID &riid, void **pp) {
    typedef HRESULT(WINAPI *fn_t)(const void *, SIZE_T, const GUID &, void **);
    fn_t fn = (fn_t)real("D3D12CreateRootSignatureDeserializer"); return fn ? fn(p, n, riid, pp) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    const void *p, SIZE_T n, const GUID &riid, void **pp) {
    typedef HRESULT(WINAPI *fn_t)(const void *, SIZE_T, const GUID &, void **);
    fn_t fn = (fn_t)real("D3D12CreateVersionedRootSignatureDeserializer"); return fn ? fn(p, n, riid, pp) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT NumFeatures, const GUID *pIIDs, void *pConfigs, UINT *pConfigsSizes) {
    typedef HRESULT(WINAPI *fn_t)(UINT, const GUID *, void *, UINT *);
    fn_t fn = (fn_t)real("D3D12EnableExperimentalFeatures"); return fn ? fn(NumFeatures, pIIDs, pConfigs, pConfigsSizes) : E_NOTIMPL;
}

} /* extern "C" */
