/* Controlled proof for the early DXGI -> AMD AGS -> D3D12 external-device path. */
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <cstdio>

struct AGSContext;
typedef int AGSReturnCode;
struct AGSDX12DeviceCreationParams {
    IDXGIAdapter *pAdapter;
    IID iid;
    D3D_FEATURE_LEVEL FeatureLevel;
};
struct AGSDX12ExtensionParams {
    const WCHAR *pAppName;
    const WCHAR *pEngineName;
    unsigned int appVersion;
    unsigned int engineVersion;
    unsigned int uavSlot;
};
struct AGSDX12ReturnedParams {
    ID3D12Device *pDevice;
    unsigned int extensionsSupported;
};

typedef HRESULT (WINAPI *pD3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef BOOL (WINAPI *pInstallHook)();
typedef void (WINAPI *pEarlyStats)(long *, long *, long *, long *, long *, long *, long *);
typedef void (WINAPI *pSystemStats)(long *, long *, long *);
typedef void (WINAPI *pCoreTableStats)(long *, long *);

static pD3D12CreateDevice g_create = nullptr;

extern "C" __declspec(dllexport) __attribute__((noinline))
AGSReturnCode WINAPI agsDriverExtensionsDX12_CreateDevice(
    AGSContext *, const AGSDX12DeviceCreationParams *creation,
    const AGSDX12ExtensionParams *, AGSDX12ReturnedParams *returned) {
    if (!g_create || !creation || !returned) return -1;
    returned->pDevice = nullptr;
    returned->extensionsSupported = 0;
    HRESULT hr = g_create(creation->pAdapter, creation->FeatureLevel,
                          creation->iid, (void **)&returned->pDevice);
    return SUCCEEDED(hr) && returned->pDevice ? 0 : -2;
}

int main() {
    std::printf("rgpu early DXGI / AMD AGS harness\n-------------------------------------\n");

    HMODULE d3d = LoadLibraryA("rgpu_d3d12.dll");
    if (!d3d) { std::printf("FAIL: load rgpu_d3d12.dll err=%lu\n", GetLastError()); return 1; }
    g_create = (pD3D12CreateDevice)GetProcAddress(d3d, "D3D12CreateDevice");
    if (!g_create || !GetProcAddress(d3d, "rgpu_d3d12_arm_external_device")) {
        std::printf("FAIL: D3D12 proxy missing required exports\n"); return 1;
    }

    HMODULE dxgi = LoadLibraryA("rgpu_dxgi_early.dll");
    if (!dxgi) { std::printf("FAIL: load rgpu_dxgi_early.dll err=%lu\n", GetLastError()); return 1; }
    pInstallHook install = (pInstallHook)GetProcAddress(dxgi, "rgpu_dxgi_install_ags_hook");
    pEarlyStats stats = (pEarlyStats)GetProcAddress(dxgi, "rgpu_dxgi_early_stats");
    pSystemStats systemStats = (pSystemStats)GetProcAddress(dxgi, "rgpu_dxgi_system_create_stats");
    pCoreTableStats coreTableStats = (pCoreTableStats)GetProcAddress(dxgi, "rgpu_dxgi_core_table_create_stats");
    if (!install || !stats || !systemStats || !coreTableStats) { std::printf("FAIL: early DXGI exports missing\n"); return 1; }

    FARPROC hostExport = GetProcAddress(GetModuleHandleA(nullptr), "agsDriverExtensionsDX12_CreateDevice");
    std::printf("host AGS export: %p\n", (void *)hostExport);
    if (!hostExport || !install()) { std::printf("FAIL: AGS hook not installed\n"); return 1; }

    AGSDX12DeviceCreationParams cp{};
    cp.pAdapter = nullptr;
    cp.iid = __uuidof(ID3D12Device);
    cp.FeatureLevel = D3D_FEATURE_LEVEL_11_0;
    AGSDX12ReturnedParams rp{};

    AGSReturnCode rc = agsDriverExtensionsDX12_CreateDevice(nullptr, &cp, nullptr, &rp);
    long installed=0, calls=0, arms=0;
    long coreInstalled=0, coreCalls=0, sdkFactoryCalls=0, factoryDeviceCalls=0;
    stats(&installed, &calls, &arms, &coreInstalled, &coreCalls, &sdkFactoryCalls, &factoryDeviceCalls);
    long systemInstalled=0, systemCalls=0, systemDevices=0;
    long coreTableCalls=0, coreTableDevices=0;
    systemStats(&systemInstalled, &systemCalls, &systemDevices);
    coreTableStats(&coreTableCalls, &coreTableDevices);
    std::printf("AGS rc=%d device=%p | AGS hook=%ld calls=%ld | system hook=%ld calls=%ld devices=%ld | Core table calls=%ld devices=%ld | external-arms=%ld | core-hook=%ld core-calls=%ld\n",
                rc, (void *)rp.pDevice, installed, calls, systemInstalled, systemCalls, systemDevices,
                coreTableCalls, coreTableDevices, arms, coreInstalled, coreCalls);

    bool ok = rc == 0 && rp.pDevice && installed == 1 && calls == 1 &&
              systemInstalled == 1 && systemCalls >= 1 && systemDevices >= 1 &&
              coreTableCalls >= 1 && coreTableDevices >= 1 && arms >= 3;
    if (rp.pDevice) rp.pDevice->Release();
    std::printf("-------------------------------------\n");
    std::printf(ok ? "RESULT: early AGS device path captured and armed\n" : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
