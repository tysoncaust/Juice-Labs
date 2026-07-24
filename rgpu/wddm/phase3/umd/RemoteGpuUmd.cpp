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
    uint32_t owner_pid = 0;
    uint32_t client_slot = kInvalidClientSlot;
    uint64_t generation = 0;
};

struct DeviceState {
    UINT interface_version = 0;
    UINT runtime_version = 0;
};

void CloseTransport(AdapterState* adapter) {
    if (!adapter) return;
    if (adapter->shared && adapter->client_slot < kClientCount &&
        adapter->owner_pid != 0) {
        unregister_client(adapter->shared, adapter->client_slot,
                          adapter->owner_pid);
    }
    if (adapter->shared) UnmapViewOfFile(adapter->shared);
    if (adapter->completion_event) CloseHandle(adapter->completion_event);
    if (adapter->request_event) CloseHandle(adapter->request_event);
    if (adapter->mapping) CloseHandle(adapter->mapping);
    adapter->shared = nullptr;
    adapter->completion_event = nullptr;
    adapter->request_event = nullptr;
    adapter->mapping = nullptr;
    adapter->client_slot = kInvalidClientSlot;
}

bool PushRequest(AdapterState* adapter, const Message& request,
                 DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (try_push(&adapter->shared->requests, request)) {
            SetEvent(adapter->request_event);
            return true;
        }
        Sleep(1);
    }
    return false;
}

HRESULT ConnectTransport(AdapterState* adapter) {
    adapter->owner_pid = GetCurrentProcessId();
    for (int attempt = 0; attempt < 80; ++attempt) {
        adapter->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
        if (adapter->mapping) break;
        Sleep(25);
    }
    if (!adapter->mapping) return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    adapter->shared = static_cast<SharedState*>(MapViewOfFile(
        adapter->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!adapter->shared) {
        CloseTransport(adapter);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (state_valid(adapter->shared) &&
            InterlockedCompareExchange(&adapter->shared->service_alive, 1, 1) == 1) {
            break;
        }
        Sleep(25);
    }
    if (!state_valid(adapter->shared) ||
        InterlockedCompareExchange(&adapter->shared->service_alive, 1, 1) != 1) {
        CloseTransport(adapter);
        return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    }

    adapter->generation = static_cast<uint64_t>(InterlockedCompareExchange64(
        &adapter->shared->connection_generation, 0, 0));
    const int registered = register_client(adapter->shared, adapter->owner_pid,
                                           adapter->generation);
    if (registered < 0) {
        CloseTransport(adapter);
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_SESS);
    }
    adapter->client_slot = static_cast<uint32_t>(registered);
    adapter->request_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                        kRequestEventName);
    wchar_t completion_name[96]{};
    completion_event_name(adapter->client_slot, completion_name,
                          _countof(completion_name));
    adapter->completion_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                           completion_name);
    if (!adapter->request_event || !adapter->completion_event) {
        CloseTransport(adapter);
        return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    }

    const uint64_t sequence = next_sequence(adapter->shared);
    const Message ping = make_message(adapter->generation, sequence,
                                      adapter->owner_pid, adapter->client_slot,
                                      Opcode::Ping);
    if (!PushRequest(adapter, ping, 2000)) {
        CloseTransport(adapter);
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }

    ClientChannel* channel = &adapter->shared->clients[adapter->client_slot];
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline) {
        Message completion{};
        while (try_pop(&channel->completions, &completion)) {
            if (completion.sequence == sequence &&
                valid_for_generation(completion, adapter->generation) &&
                completion.owner_pid == adapter->owner_pid &&
                completion.client_slot == adapter->client_slot &&
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
