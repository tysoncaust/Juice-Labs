/* rgpu dxgi proxy DLL — the injection mechanism.
 *
 * Built as `dxgi.dll` and placed next to a game's .exe, Windows' DLL search
 * order loads it before System32\dxgi.dll. It routes the factory-creation
 * exports the game calls (CreateDXGIFactory / 1 / 2) to the synthetic remote
 * adapter, and forwards every other dxgi export to the real system dxgi.dll so
 * the game keeps working. (Test build is named rgpu_dxgi.dll and loaded
 * explicitly; deployment renames it to dxgi.dll beside the game.) */
#include "rgpu_synthetic.h"
#include <windows.h>
#include <cstring>

static HMODULE g_real = nullptr;

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n && n < MAX_PATH - 16) {
            std::strcat(path, "\\dxgi.dll");
            g_real = LoadLibraryA(path);   // the genuine dxgi for pass-through
        }
    }
    return TRUE;
}

static FARPROC real(const char *name) { return g_real ? GetProcAddress(g_real, name) : nullptr; }

extern "C" {

/* ---- intercepted: return the synthetic remote adapter ------------------- */
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory) {
    return rgpu_CreateDXGIFactory1(riid, ppFactory);
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    return rgpu_CreateDXGIFactory1(riid, ppFactory);
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT, REFIID riid, void **ppFactory) {
    return rgpu_CreateDXGIFactory1(riid, ppFactory);
}

/* ---- forwarded: pass through to the real dxgi.dll ----------------------- */
__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug) {
    typedef HRESULT(WINAPI *fn_t)(UINT, REFIID, void **);
    fn_t fn = (fn_t)real("DXGIGetDebugInterface1");
    return fn ? fn(Flags, riid, pDebug) : E_NOTIMPL;
}
__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    typedef HRESULT(WINAPI *fn_t)();
    fn_t fn = (fn_t)real("DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : S_OK;
}

} /* extern "C" */
