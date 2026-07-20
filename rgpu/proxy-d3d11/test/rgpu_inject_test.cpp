/* rgpu injection proof: loads the proxy DLL exactly as a game would resolve
 * dxgi.dll, calls its exported CreateDXGIFactory1, and verifies the synthetic
 * remote adapter comes back across the DLL boundary. */
#include <windows.h>
#include <dxgi.h>
#include <cstdio>
#include <cwchar>

int main() {
    std::printf("rgpu injection proof (proxy dxgi.dll)\n-------------------------------------\n");
    HMODULE dll = LoadLibraryA("rgpu_dxgi.dll");
    if (!dll) { std::printf("FAIL: could not load rgpu_dxgi.dll (err %lu)\n", GetLastError()); return 1; }

    typedef HRESULT(WINAPI *CreateFactory1_t)(REFIID, void **);
    auto CreateFactory1 = (CreateFactory1_t)GetProcAddress(dll, "CreateDXGIFactory1");
    if (!CreateFactory1) { std::printf("FAIL: CreateDXGIFactory1 export missing\n"); return 1; }
    std::printf("  ok: proxy DLL loaded, CreateDXGIFactory1 exported\n");

    IDXGIFactory1 *f = nullptr;
    HRESULT hr = CreateFactory1(__uuidof(IDXGIFactory1), (void **)&f);
    if (FAILED(hr) || !f) { std::printf("FAIL: CreateDXGIFactory1 hr=0x%08lX\n", hr); return 1; }

    IDXGIAdapter1 *a = nullptr;
    if (FAILED(f->EnumAdapters1(0, &a))) { std::printf("FAIL: EnumAdapters1\n"); return 1; }
    DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
    std::printf("  game would see: \"%ls\" vendor=0x%04X vram=%llu MB\n",
                d.Description, d.VendorId, (unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024)));
    int ok = (std::wcsstr(d.Description, L"Remote GPU") != nullptr) && d.VendorId == 0x10DE;
    a->Release(); f->Release(); FreeLibrary(dll);

    std::printf("-------------------------------------\n");
    std::printf(ok ? "RESULT: synthetic adapter served through the injected DLL\n"
                   : "RESULT: FAIL (did not get the synthetic remote adapter)\n");
    return ok ? 0 : 1;
}
