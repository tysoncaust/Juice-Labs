/* rgpu Phase-1 proof harness: exercises the synthetic DXGI adapter + fail-closed
 * policy the way a game's pre-launch checks and device creation do, and asserts
 * the verification criteria (synthetic remote adapter enumerated; no local
 * hardware device ever created). Compile + run on the RTX host. */
#include "../src/rgpu_synthetic.h"
#include <cstdio>
#include <cwchar>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL: %s\n", msg); failures++; } \
                              else { std::printf("  ok:   %s\n", msg); } } while (0)

int main() {
    std::printf("rgpu Phase-1 interception proof\n------------------------------\n");

    /* 1) The game enumerates adapters -> it must see ONLY the synthetic remote GPU. */
    std::printf("[1] synthetic adapter enumeration\n");
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = rgpu_CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
    CHECK(hr == S_OK && factory, "CreateDXGIFactory1 returns a synthetic factory");

    IDXGIAdapter1 *adapter = nullptr;
    hr = factory->EnumAdapters1(0, &adapter);
    CHECK(hr == S_OK && adapter, "EnumAdapters1(0) returns adapter 0");

    IDXGIAdapter1 *none = nullptr;
    CHECK(factory->EnumAdapters1(1, &none) == DXGI_ERROR_NOT_FOUND, "EnumAdapters1(1) => NOT_FOUND (only one adapter)");

    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    std::printf("      adapter: \"%ls\" vendor=0x%04X vram=%llu MB\n",
                d.Description, d.VendorId, (unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024)));
    CHECK(std::wcsstr(d.Description, L"Remote GPU") != nullptr, "description advertises a Remote GPU");
    CHECK(d.VendorId == 0x10DE, "vendor id is NVIDIA (0x10DE)");
    CHECK(d.DedicatedVideoMemory >= (SIZE_T)15000 * 1024 * 1024, "VRAM reflects the remote GPU (>=15 GB)");

    /* 2) Device creation with remote NOT connected -> fail closed, no local device. */
    std::printf("[2] fail-closed device creation (remote offline)\n");
    g_rgpu_remote_connected = 0;
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    hr = rgpu_D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    CHECK(hr == DXGI_ERROR_DEVICE_REMOVED, "HARDWARE device w/ remote offline => DEVICE_REMOVED (no local fallback)");
    CHECK(dev == nullptr, "no device object handed back");
    CHECK(g_local_hw_devices_created == 0, "localHardwareDevicesCreated == 0");

    /* 3) Remote session live -> counts a remote device, still no local one. */
    std::printf("[3] remote device path\n");
    g_rgpu_remote_connected = 1;
    hr = rgpu_D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    CHECK(g_remote_devices_created == 1, "remoteDevicesCreated == 1");
    CHECK(g_local_hw_devices_created == 0, "localHardwareDevicesCreated still 0");

    /* 4) WARP/software adapter requested -> refused (do not abort the test). */
    std::printf("[4] WARP/software fallback refused\n");
    g_rgpu_policy.debug_abort_on_local_hw = 0; /* observe the refusal instead of aborting */
    IDXGIAdapter *sw = nullptr;
    hr = factory->CreateSoftwareAdapter(nullptr, &sw);
    CHECK(hr == DXGI_ERROR_UNSUPPORTED && sw == nullptr, "CreateSoftwareAdapter (WARP) => UNSUPPORTED");
    CHECK(g_local_hw_attempts_refused == 1, "the WARP attempt was caught + refused");
    CHECK(g_local_hw_devices_created == 0, "localHardwareDevicesCreated STILL 0 (invariant holds)");

    adapter->Release();
    factory->Release();

    std::printf("------------------------------\n");
    std::printf("VERIFICATION: localHardwareDevicesCreated=%ld  remoteDevicesCreated=%ld  refused=%ld\n",
                g_local_hw_devices_created, g_remote_devices_created, g_local_hw_attempts_refused);
    std::printf(failures ? "RESULT: %d FAILURE(S)\n" : "RESULT: all checks passed\n", failures);
    return failures ? 1 : 0;
}
