#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3d12umddi.h>

#include <iostream>

using OpenAdapter12Fn = HRESULT (APIENTRY*)(D3D12DDIARG_OPENADAPTER*);

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "usage: RemoteGpuUmdTest <RemoteGpuUmd.dll>\n";
        return 64;
    }
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::cerr << "PHASE3_UMD_ABI=FAIL load=" << GetLastError() << "\n";
        return 1;
    }
    auto open_adapter = reinterpret_cast<OpenAdapter12Fn>(
        GetProcAddress(module, "OpenAdapter12"));
    if (!open_adapter) {
        std::cerr << "PHASE3_UMD_ABI=FAIL export\n";
        FreeLibrary(module);
        return 2;
    }

    D3D12DDI_ADAPTERFUNCS functions{};
    D3D12DDIARG_OPENADAPTER arguments{};
    arguments.pAdapterFuncs = &functions;
    const HRESULT result = open_adapter(&arguments);
    const bool complete_entry_table =
        SUCCEEDED(result) && arguments.hAdapter.pDrvPrivate != nullptr &&
        functions.pfnCalcPrivateDeviceSize != nullptr &&
        functions.pfnCreateDevice != nullptr &&
        functions.pfnCloseAdapter != nullptr &&
        functions.pfnGetSupportedVersions != nullptr &&
        functions.pfnGetCaps != nullptr &&
        functions.pfnDestroyDevice != nullptr;
    if (!complete_entry_table) {
        std::cerr << "PHASE3_UMD_ABI=FAIL table hr=0x" << std::hex
                  << static_cast<unsigned long>(result) << std::dec << "\n";
        FreeLibrary(module);
        return 3;
    }

    D3D12DDIARG_CALCPRIVATEDEVICESIZE size_arguments{};
    const SIZE_T private_size = functions.pfnCalcPrivateDeviceSize(
        arguments.hAdapter, &size_arguments);
    const HRESULT device_result = functions.pfnCreateDevice(arguments.hAdapter, nullptr);
    if (private_size == 0 || device_result != DXGI_ERROR_UNSUPPORTED) {
        std::cerr << "PHASE3_UMD_ABI=FAIL fail_closed hr=0x" << std::hex
                  << static_cast<unsigned long>(device_result) << std::dec << "\n";
        functions.pfnCloseAdapter(arguments.hAdapter);
        FreeLibrary(module);
        return 4;
    }

    functions.pfnCloseAdapter(arguments.hAdapter);
    FreeLibrary(module);
    std::cout << "PHASE3_UMD_ABI=PASS export=OpenAdapter12"
              << " entry_table=valid create_device=fail_closed"
              << " private_device_bytes=" << private_size << "\n";
    return 0;
}
