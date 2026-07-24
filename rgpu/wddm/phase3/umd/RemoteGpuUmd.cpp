#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <dxgi.h>
#include <d3d12umddi.h>

#include <new>

#include "../shared/rgpu_phase3_queue.h"

using namespace rgpu::phase3;

namespace {

struct AdapterState {
    D3D12DDI_HRTADAPTER runtime_adapter{};
    const D3DDDI_ADAPTERCALLBACKS* callbacks = nullptr;
    HANDLE mapping = nullptr;
    HANDLE request_event = nullptr;
    HANDLE completion_event = nullptr;
    SharedState* shared = nullptr;
};

struct DeviceState {
    UINT interface_version = 0;
    UINT runtime_version = 0;
};

void CloseTransport(AdapterState* adapter) {
    if (!adapter) return;
    if (adapter->shared) UnmapViewOfFile(adapter->shared);
    if (adapter->completion_event) CloseHandle(adapter->completion_event);
    if (adapter->request_event) CloseHandle(adapter->request_event);
    if (adapter->mapping) CloseHandle(adapter->mapping);
    adapter->shared = nullptr;
    adapter->completion_event = nullptr;
    adapter->request_event = nullptr;
    adapter->mapping = nullptr;
}

HRESULT ConnectTransport(AdapterState* adapter) {
    for (int attempt = 0; attempt < 80; ++attempt) {
        adapter->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
        if (adapter->mapping) break;
        Sleep(25);
    }
    if (!adapter->mapping) return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    adapter->shared = static_cast<SharedState*>(MapViewOfFile(
        adapter->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    for (int attempt = 0; attempt < 80; ++attempt) {
        adapter->request_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                            kRequestEventName);
        adapter->completion_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                               kCompletionEventName);
        if (adapter->request_event && adapter->completion_event && adapter->shared &&
            InterlockedCompareExchange(&adapter->shared->service_alive, 1, 1) == 1) {
            break;
        }
        if (adapter->completion_event) { CloseHandle(adapter->completion_event); adapter->completion_event = nullptr; }
        if (adapter->request_event) { CloseHandle(adapter->request_event); adapter->request_event = nullptr; }
        Sleep(25);
    }
    if (!adapter->shared || !adapter->request_event || !adapter->completion_event ||
        adapter->shared->magic != kMagic || adapter->shared->version != kVersion ||
        InterlockedCompareExchange(&adapter->shared->service_alive, 1, 1) != 1) {
        CloseTransport(adapter);
        return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    }

    const uint64_t sequence = GetTickCount64() | (1ull << 63u);
    const Message ping = make_message(sequence, Opcode::Ping);
    if (!try_push(&adapter->shared->requests, ping)) {
        CloseTransport(adapter);
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    SetEvent(adapter->request_event);
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline) {
        Message completion{};
        while (try_pop(&adapter->shared->completions, &completion)) {
            if (completion.sequence == sequence && valid(completion) &&
                completion.status == static_cast<int32_t>(Status::Ok)) {
                return S_OK;
            }
        }
        const DWORD remaining = static_cast<DWORD>(deadline - GetTickCount64());
        WaitForSingleObject(adapter->completion_event, remaining);
    }
    CloseTransport(adapter);
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

SIZE_T APIENTRY CalcPrivateDeviceSize(
    D3D12DDI_HADAPTER,
    const D3D12DDIARG_CALCPRIVATEDEVICESIZE*) {
    return sizeof(DeviceState);
}

HRESULT APIENTRY CreateDevice(
    D3D12DDI_HADAPTER,
    const D3D12DDIARG_CREATEDEVICE_0003*) {
    // Fail closed until every mandatory D3D12 graphics DDI table is implemented.
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT APIENTRY CloseAdapter(D3D12DDI_HADAPTER adapter) {
    auto* state = static_cast<AdapterState*>(adapter.pDrvPrivate);
    CloseTransport(state);
    delete state;
    return S_OK;
}

HRESULT APIENTRY GetSupportedVersions(
    D3D12DDI_HADAPTER,
    UINT32* entries,
    UINT64* versions) {
    if (!entries) return E_INVALIDARG;
    if (!versions) {
        *entries = 0;
        return S_OK;
    }
    *entries = 0;
    return S_OK;
}

HRESULT APIENTRY GetCaps(
    D3D12DDI_HADAPTER,
    const D3D12DDIARG_GETCAPS*) {
    return E_NOTIMPL;
}

void APIENTRY DestroyDevice(D3D12DDI_HDEVICE) {
}

} // namespace

extern "C" __declspec(dllexport)
HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER* arguments) {
    if (!arguments || !arguments->pAdapterFuncs) {
        return E_INVALIDARG;
    }

    auto* adapter = new (std::nothrow) AdapterState{};
    if (!adapter) return E_OUTOFMEMORY;
    adapter->runtime_adapter = arguments->hRTAdapter;
    adapter->callbacks = arguments->pAdapterCallbacks;
    const HRESULT transport_result = ConnectTransport(adapter);
    if (FAILED(transport_result)) {
        delete adapter;
        return transport_result;
    }

    arguments->hAdapter.pDrvPrivate = adapter;
    D3D12DDI_ADAPTERFUNCS functions{};
    functions.pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
    functions.pfnCreateDevice = CreateDevice;
    functions.pfnCloseAdapter = CloseAdapter;
    functions.pfnGetSupportedVersions = GetSupportedVersions;
    functions.pfnGetCaps = GetCaps;
    functions.pfnGetOptionalDDITables = nullptr;
    functions.pfnFillDDITable = nullptr;
    functions.pfnDestroyDevice = DestroyDevice;
    *arguments->pAdapterFuncs = functions;
    return S_OK;
}
