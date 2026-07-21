/* Transparent early DXGI proxy for TXR D3D12 interception.
 *
 * TXR imports dxgi.dll before process entry but obtains D3D12 dynamically. This
 * proxy therefore installs two early observations without changing the adapter:
 *   1. host-exported AMD AGS DX12 device creation;
 *   2. the Agility D3D12Core.dll D3D12GetInterface export as soon as that module
 *      is loaded, followed by SDKConfiguration1 / DeviceFactory vtable hooks.
 * Any real ID3D12Device returned by those routes is handed to the existing rgpu
 * D3D12 command-capture DLL through rgpu_d3d12_arm_external_device.
 */
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdio>

#include "rgpu_inlinehook.h"

extern "C" unsigned RGPU_SLOT_Factory_CreateDevice;
extern "C" unsigned RGPU_SLOT_SDKConfig1_CreateDeviceFactory;

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

typedef AGSReturnCode (WINAPI *fnAGSCreateDX12)(
    AGSContext *, const AGSDX12DeviceCreationParams *,
    const AGSDX12ExtensionParams *, AGSDX12ReturnedParams *);
typedef void (WINAPI *fnArmExternalDevice)(ID3D12Device *, const char *);
typedef HRESULT (WINAPI *fnCoreGetInterface)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *fnSystemD3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
/* Internal dispatch entry 0, recovered from d3d12.dll+0x7F20. */
typedef HRESULT (WINAPI *fnCoreDispatchCreateDevice)(
    IUnknown *, D3D_FEATURE_LEVEL, unsigned char, const void *,
    uintptr_t, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnCoreGetDllExports)(IUnknown *, void *);
typedef HRESULT (STDMETHODCALLTYPE *fnSDKCreateDeviceFactory)(
    ID3D12SDKConfiguration1 *, UINT, LPCSTR, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnFactoryCreateDevice)(
    ID3D12DeviceFactory *, IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

static HMODULE g_real_dxgi = nullptr;
static fnAGSCreateDX12 g_ags_original = nullptr;
static void *g_ags_target = nullptr;
static volatile LONG g_ags_hook_installed = 0;
static volatile LONG g_ags_calls = 0;

static fnCoreGetInterface g_core_get_interface_original = nullptr;
static fnSystemD3D12CreateDevice g_system_create_device_original = nullptr;
static fnCoreDispatchCreateDevice g_core_table_create_device_original = nullptr;
static fnCoreGetInterface g_system_get_interface_original = nullptr;
static void *g_system_create_device_target = nullptr;
static void *g_system_get_interface_target = nullptr;
static volatile LONG g_system_create_hook_installed = 0;
static volatile LONG g_system_get_interface_hook_installed = 0;
static volatile LONG g_system_create_calls = 0;
static volatile LONG g_system_create_devices = 0;
static volatile LONG g_core_table_create_calls = 0;
static volatile LONG g_core_table_create_devices = 0;
static volatile LONG g_system_get_interface_calls = 0;
static fnCoreGetDllExports g_core_get_dll_exports_original = nullptr;
typedef HRESULT (STDMETHODCALLTYPE *fnCoreQueryInterface)(IUnknown *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *fnCoreVersionedSlot9)(IUnknown *, void *);
typedef uintptr_t (STDMETHODCALLTYPE *fnCoreVersionedSlot11)(IUnknown *, void *);
static fnCoreQueryInterface g_core_query_interface_original = nullptr;
static fnCoreVersionedSlot9 g_core_versioned_slot9_original = nullptr;
static fnCoreVersionedSlot11 g_core_versioned_slot11_original = nullptr;
static void *g_core_target = nullptr;
static void *g_core_module_vtbl = nullptr;
static volatile LONG g_core_exports_calls = 0;
static volatile LONG g_core_qi_calls = 0;
static volatile LONG g_core_versioned_slot9_calls = 0;
static volatile LONG g_core_versioned_slot11_calls = 0;
static volatile LONG g_core_hook_installed = 0;
static volatile LONG g_core_get_interface_calls = 0;
static volatile LONG g_core_monitor_started = 0;
static void *g_dll_notification_cookie = nullptr;

static fnSDKCreateDeviceFactory g_sdk_create_factory_original = nullptr;
static fnFactoryCreateDevice g_factory_create_device_original = nullptr;
static volatile LONG g_sdk_factory_calls = 0;
static volatile LONG g_factory_device_calls = 0;
static volatile LONG g_arm_calls = 0;
static volatile LONG g_reported = 0;

static void log_line(const char *fmt, ...) {
    char msg[2048];
    va_list ap; va_start(ap, fmt);
    int n = std::vsnprintf(msg, sizeof(msg) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(msg) - 3) n = (int)sizeof(msg) - 3;
    msg[n++] = '\r'; msg[n++] = '\n'; msg[n] = 0;

    char path[MAX_PATH] = {0};
    DWORD pn = GetTempPathA(MAX_PATH, path);
    if (!pn || pn > MAX_PATH - 32) return;
    std::strcat(path, "rgpu_dxgi_early.log");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0; WriteFile(f, msg, (DWORD)n, &wrote, nullptr); CloseHandle(f);
}

static HMODULE real_dxgi() {
    if (g_real_dxgi) return g_real_dxgi;
    char path[MAX_PATH] = {0};
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n > MAX_PATH - 16) return nullptr;
    std::strcat(path, "\\dxgi.dll");
    g_real_dxgi = LoadLibraryA(path);
    return g_real_dxgi;
}
static FARPROC real_export(const char *name) {
    HMODULE m = real_dxgi();
    return m ? GetProcAddress(m, name) : nullptr;
}

static fnArmExternalDevice find_arm_export(bool allowLoad) {
    auto scan = []() -> fnArmExternalDevice {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) return nullptr;
        MODULEENTRY32 me{}; me.dwSize = sizeof(me);
        fnArmExternalDevice result = nullptr;
        if (Module32First(snap, &me)) do {
            FARPROC p = GetProcAddress(me.hModule, "rgpu_d3d12_arm_external_device");
            if (p) { result = reinterpret_cast<fnArmExternalDevice>(p); break; }
        } while (Module32Next(snap, &me));
        CloseHandle(snap);
        return result;
    };
    fnArmExternalDevice arm = scan();
    if (!arm && allowLoad) {
        LoadLibraryA("d3d12.dll");
        arm = scan();
    }
    return arm;
}

static void arm_device(ID3D12Device *device, const char *source) {
    if (!device) return;
    fnArmExternalDevice arm = find_arm_export(true);
    if (arm) {
        arm(device, source);
        InterlockedIncrement(&g_arm_calls);
        log_line("%s returned device handed to rgpu D3D12 capture device=%p", source, (void *)device);
    } else {
        log_line("%s returned device but rgpu_d3d12_arm_external_device was not found", source);
    }
}

static bool hook_factory_object(void *obj);
static bool hook_sdk_configuration(void *obj);

static HRESULT WINAPI hook_system_d3d12_get_interface(
    REFCLSID clsid, REFIID riid, void **ppv) {
    InterlockedIncrement(&g_system_get_interface_calls);
    HRESULT hr = g_system_get_interface_original(clsid, riid, ppv);
    void *obj = (SUCCEEDED(hr) && ppv) ? *ppv : nullptr;
    log_line("SYSTEM d3d12!D3D12GetInterface clsid={%08lX-%04X-%04X-...} iid={%08lX-%04X-%04X-...} hr=0x%08lX object=%p",
             (unsigned long)clsid.Data1, (unsigned)clsid.Data2, (unsigned)clsid.Data3,
             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
             (unsigned long)hr, obj);
    if (obj) {
        hook_sdk_configuration(obj);
        hook_factory_object(obj);
    }
    return hr;
}

static HRESULT WINAPI hook_system_d3d12_create_device(
    IUnknown *adapter, D3D_FEATURE_LEVEL featureLevel, REFIID riid, void **ppDevice) {
    InterlockedIncrement(&g_system_create_calls);
    HRESULT hr = g_system_create_device_original(adapter, featureLevel, riid, ppDevice);
    ID3D12Device *device = (SUCCEEDED(hr) && ppDevice) ? (ID3D12Device *)*ppDevice : nullptr;
    log_line("SYSTEM d3d12!D3D12CreateDevice iid={%08lX-%04X-%04X-...} fl=0x%X hr=0x%08lX device=%p vtbl=%p",
             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
             (unsigned)featureLevel, (unsigned long)hr, (void *)device,
             device ? *(void **)device : nullptr);
    if (device) {
        InterlockedIncrement(&g_system_create_devices);
        arm_device(device, "System_D3D12CreateDevice");
    }
    return hr;
}

static bool hook_system_d3d12_module(HMODULE module) {
    if (!module) return false;
    if (GetProcAddress(module, "rgpu_d3d12_arm_external_device"))
        return false; /* our game-directory proxy, not the system runtime */

    if (!g_system_create_hook_installed) {
        FARPROC target = GetProcAddress(module, "D3D12CreateDevice");
        if (target) {
            void *trampoline = nullptr;
            g_system_create_device_target = (void *)target;
            if (rgpu_inline_hook((void *)target, (void *)hook_system_d3d12_create_device, &trampoline)) {
                g_system_create_device_original = reinterpret_cast<fnSystemD3D12CreateDevice>(trampoline);
                InterlockedExchange(&g_system_create_hook_installed, 1);
            }
        }
    }

    if (!g_system_get_interface_hook_installed) {
        FARPROC target = GetProcAddress(module, "D3D12GetInterface");
        if (target) {
            void *trampoline = nullptr;
            g_system_get_interface_target = (void *)target;
            if (rgpu_inline_hook((void *)target, (void *)hook_system_d3d12_get_interface, &trampoline)) {
                g_system_get_interface_original = reinterpret_cast<fnCoreGetInterface>(trampoline);
                InterlockedExchange(&g_system_get_interface_hook_installed, 1);
            }
        }
    }

    return g_system_create_hook_installed && g_system_get_interface_hook_installed;
}

static bool patch_slot(void *obj, unsigned idx, void *detour, void **original) {
    if (!obj) return false;
    void ***pvt = reinterpret_cast<void ***>(obj);
    if (!pvt || !*pvt) return false;
    void **slot = &(*pvt)[idx];
    if (*slot == detour) return true;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    void *prior = *slot;
    *slot = detour;
    VirtualProtect(slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    if (original && !*original) *original = prior;
    return true;
}

static bool hook_factory_object(void *obj);

static HRESULT STDMETHODCALLTYPE hook_factory_create_device(
    ID3D12DeviceFactory *self, IUnknown *adapter, D3D_FEATURE_LEVEL fl,
    REFIID riid, void **ppDevice) {
    InterlockedIncrement(&g_factory_device_calls);
    HRESULT hr = g_factory_create_device_original(self, adapter, fl, riid, ppDevice);
    ID3D12Device *device = (SUCCEEDED(hr) && ppDevice) ? (ID3D12Device *)*ppDevice : nullptr;
    log_line("D3D12Core DeviceFactory::CreateDevice hr=0x%08lX iid={%08lX-%04X-%04X-...} device=%p",
             (unsigned long)hr, (unsigned long)riid.Data1, (unsigned)riid.Data2,
             (unsigned)riid.Data3, (void *)device);
    if (device) arm_device(device, "D3D12Core_DeviceFactory");
    return hr;
}

static bool hook_factory_object(void *obj) {
    if (!obj) return false;
    ID3D12DeviceFactory *factory = nullptr;
    if (FAILED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12DeviceFactory),
                                                 (void **)&factory)) || !factory)
        return false;
    bool ok = patch_slot(factory, RGPU_SLOT_Factory_CreateDevice,
                         (void *)hook_factory_create_device,
                         (void **)&g_factory_create_device_original);
    log_line("D3D12Core factory observed object=%p vtbl=%p hook=%d",
             (void *)factory, *(void **)factory, ok ? 1 : 0);
    factory->Release();
    return ok;
}

static HRESULT STDMETHODCALLTYPE hook_sdk_create_device_factory(
    ID3D12SDKConfiguration1 *self, UINT sdkVersion, LPCSTR sdkPath,
    REFIID riid, void **ppFactory) {
    InterlockedIncrement(&g_sdk_factory_calls);
    HRESULT hr = g_sdk_create_factory_original(self, sdkVersion, sdkPath, riid, ppFactory);
    log_line("D3D12Core SDKConfiguration1::CreateDeviceFactory sdk=%u path=\"%s\" hr=0x%08lX factory=%p",
             sdkVersion, sdkPath ? sdkPath : "(null)", (unsigned long)hr,
             ppFactory ? *ppFactory : nullptr);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory_object(*ppFactory);
    return hr;
}

static bool hook_sdk_configuration(void *obj) {
    if (!obj) return false;
    ID3D12SDKConfiguration1 *sdk = nullptr;
    if (FAILED(((IUnknown *)obj)->QueryInterface(__uuidof(ID3D12SDKConfiguration1),
                                                 (void **)&sdk)) || !sdk)
        return false;
    bool ok = patch_slot(sdk, RGPU_SLOT_SDKConfig1_CreateDeviceFactory,
                         (void *)hook_sdk_create_device_factory,
                         (void **)&g_sdk_create_factory_original);
    log_line("D3D12Core SDKConfiguration1 observed object=%p vtbl=%p hook=%d",
             (void *)sdk, *(void **)sdk, ok ? 1 : 0);
    sdk->Release();
    return ok;
}

static bool readable_qwords(void *p, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    uintptr_t end = (uintptr_t)p + bytes;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return end <= regionEnd;
}

static void describe_address(void *address, char *out, size_t outSize) {
    if (!out || !outSize) return;
    out[0] = 0;
    if (!address) { std::snprintf(out, outSize, "null"); return; }
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module) && module) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char *base = std::strrchr(path, '\\');
        base = base ? base + 1 : path;
        std::snprintf(out, outSize, "%s+0x%llX", base,
                      (unsigned long long)((uintptr_t)address - (uintptr_t)module));
    } else {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)))
            std::snprintf(out, outSize, "mem(base=%p protect=0x%lX type=0x%lX)",
                          mbi.AllocationBase, (unsigned long)mbi.Protect,
                          (unsigned long)mbi.Type);
        else
            std::snprintf(out, outSize, "unmapped");
    }
}

static HRESULT WINAPI hook_core_table_create_device(
    IUnknown *adapter, D3D_FEATURE_LEVEL featureLevel, unsigned char createFlag,
    const void *creationContext, uintptr_t mode, REFIID riid, void **ppDevice) {
    InterlockedIncrement(&g_core_table_create_calls);
    void *returnAddress = __builtin_return_address(0);
    char callerDesc[320]; describe_address(returnAddress, callerDesc, sizeof(callerDesc));
    HRESULT hr = g_core_table_create_device_original(
        adapter, featureLevel, createFlag, creationContext, mode, riid, ppDevice);
    ID3D12Device *device = (SUCCEEDED(hr) && ppDevice) ? (ID3D12Device *)*ppDevice : nullptr;
    log_line("CORE_TABLE[0] CreateDevice caller=%p %s iid={%08lX-%04X-%04X-...} fl=0x%X flag=%u context=%p mode=0x%llX hr=0x%08lX device=%p vtbl=%p",
             returnAddress, callerDesc,
             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
             (unsigned)featureLevel, (unsigned)createFlag, creationContext,
             (unsigned long long)mode, (unsigned long)hr, (void *)device,
             device ? *(void **)device : nullptr);
    if (device) {
        InterlockedIncrement(&g_core_table_create_devices);
        arm_device(device, "Core_DispatchTable_CreateDevice");
    }
    return hr;
}

static uintptr_t STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {
    InterlockedIncrement(&g_core_versioned_slot11_calls);
    log_line("CORE_VERSIONED_SLOT11 enter self=%p vtbl=%p exports=%p", self,
             self ? *(void **)self : nullptr, exportsTable);
    uintptr_t result = g_core_versioned_slot11_original(self, exportsTable);
    log_line("CORE_VERSIONED_SLOT11 leave result=0x%016llX", (unsigned long long)result);
    if (exportsTable && readable_qwords(exportsTable, 18 * sizeof(uint64_t))) {
        uint64_t *q = (uint64_t *)exportsTable;
        for (unsigned i = 0; i < 18; ++i) {
            char desc[320]; describe_address((void *)(uintptr_t)q[i], desc, sizeof(desc));
            log_line("CORE_EXPORT_TABLE[%02u]=0x%016llX %s", i,
                     (unsigned long long)q[i], desc);
        }

        /* d3d12.dll's D3D12CreateDevice implementation obtains this table
         * and executes the first qword (mov (%rax), %rax). Replace only the
         * writable per-loader table copy; retain the original Core pointer. */
        if (!g_core_table_create_device_original && q[0]) {
            g_core_table_create_device_original =
                reinterpret_cast<fnCoreDispatchCreateDevice>((uintptr_t)q[0]);
            DWORD oldProtect = 0;
            void **entry = reinterpret_cast<void **>(&q[0]);
            if (VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProtect)) {
                *entry = (void *)hook_core_table_create_device;
                DWORD ignored = 0;
                VirtualProtect(entry, sizeof(void *), oldProtect, &ignored);
                FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void *));
                log_line("CORE_EXPORT_TABLE[00] hooked original=%p detour=%p",
                         (void *)g_core_table_create_device_original,
                         (void *)hook_core_table_create_device);
            } else {
                log_line("CORE_EXPORT_TABLE[00] hook failed VirtualProtect=%lu",
                         GetLastError());
                g_core_table_create_device_original = nullptr;
            }
        }
    }
    return result;
}

static HRESULT STDMETHODCALLTYPE hook_core_versioned_slot9(IUnknown *self, void *payload) {
    InterlockedIncrement(&g_core_versioned_slot9_calls);
    log_line("CORE_VERSIONED_SLOT9 enter self=%p vtbl=%p payload=%p", self,
             self ? *(void **)self : nullptr, payload);
    if (payload && readable_qwords(payload, 24 * sizeof(uint64_t))) {
        uint64_t *q = (uint64_t *)payload;
        for (unsigned i = 0; i < 24; ++i) {
            char desc[320]; describe_address((void *)(uintptr_t)q[i], desc, sizeof(desc));
            log_line("CORE_SLOT9_PAYLOAD_QWORD[%02u]=0x%016llX %s", i,
                     (unsigned long long)q[i], desc);
        }
    }
    HRESULT hr = g_core_versioned_slot9_original(self, payload);
    log_line("CORE_VERSIONED_SLOT9 leave hr=0x%08lX", (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_core_query_interface(IUnknown *self, REFIID riid, void **ppv) {
    InterlockedIncrement(&g_core_qi_calls);
    HRESULT hr = g_core_query_interface_original(self, riid, ppv);
    void *obj = (SUCCEEDED(hr) && ppv) ? *ppv : nullptr;
    log_line("COREMODULE QueryInterface iid={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} hr=0x%08lX object=%p vtbl=%p",
             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
             riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
             riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7],
             (unsigned long)hr, obj, obj ? *(void **)obj : nullptr);
    if (obj) {
        void **vt = *(void ***)obj;
        for (unsigned i = 0; i < 12; ++i) {
            char desc[320]; describe_address(vt[i], desc, sizeof(desc));
            log_line("CORE_QI_VTBL[%02u]=%p %s", i, vt[i], desc);
        }
        /* The system d3d12 loader requests D5010570-699A-47DF-8E11-8E5A334B01CA,
         * then immediately calls slot 9 with a single pointer argument. Patch that
         * exact returned interface before QueryInterface returns to its caller. */
        if (riid.Data1 == 0xD5010570 && riid.Data2 == 0x699A && riid.Data3 == 0x47DF) {
            bool ok = patch_slot(obj, 9, (void *)hook_core_versioned_slot9,
                                 (void **)&g_core_versioned_slot9_original);
            log_line("CORE versioned D5010570 slot9 hook=%d original=%p",
                     ok ? 1 : 0, (void *)g_core_versioned_slot9_original);
        }
        if (riid.Data1 == 0xFC454290 && riid.Data2 == 0x1B19 && riid.Data3 == 0x48C4) {
            bool ok = patch_slot(obj, 11, (void *)hook_core_versioned_slot11,
                                 (void **)&g_core_versioned_slot11_original);
            log_line("CORE versioned FC454290 slot11 hook=%d original=%p",
                     ok ? 1 : 0, (void *)g_core_versioned_slot11_original);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_core_get_dll_exports(IUnknown *self, void *table) {
    InterlockedIncrement(&g_core_exports_calls);
    HRESULT hr = g_core_get_dll_exports_original(self, table);
    log_line("ID3D12CoreModule::GetDllExports hr=0x%08lX table=%p", (unsigned long)hr, table);
    if (SUCCEEDED(hr) && readable_qwords(table, 64 * sizeof(uint64_t))) {
        uint64_t *q = (uint64_t *)table;
        for (unsigned i = 0; i < 64; ++i) {
            char desc[320]; describe_address((void *)(uintptr_t)q[i], desc, sizeof(desc));
            log_line("CORE_EXPORT_QWORD[%02u]=0x%016llX %s", i,
                     (unsigned long long)q[i], desc);
        }
    } else {
        log_line("CORE_EXPORT_QWORD dump skipped: table unreadable or shorter than 512 bytes");
    }
    return hr;
}

static bool hook_core_module_object(void *obj) {
    if (!obj) return false;
    void *vtbl = *(void **)obj;
    if (vtbl == g_core_module_vtbl && g_core_get_dll_exports_original) return true;
    /* ID3D12CoreModule layout from RenderDoc: IUnknown (0..2), LOEnter (3),
     * LOLeave (4), LOTryEnter (5), Initialize (6), GetSDKVersion (7),
     * GetDllExports (8). */
    bool qiOk = patch_slot(obj, 0, (void *)hook_core_query_interface,
                           (void **)&g_core_query_interface_original);
    bool exportsOk = patch_slot(obj, 8, (void *)hook_core_get_dll_exports,
                                (void **)&g_core_get_dll_exports_original);
    if (qiOk && exportsOk) g_core_module_vtbl = vtbl;
    log_line("ID3D12CoreModule observed object=%p vtbl=%p QI-hook=%d GetDllExports-hook=%d",
             obj, vtbl, qiOk ? 1 : 0, exportsOk ? 1 : 0);
    return qiOk && exportsOk;
}

static HRESULT WINAPI hook_core_get_interface(REFCLSID clsid, REFIID riid, void **ppv) {
    InterlockedIncrement(&g_core_get_interface_calls);
    void *returnAddress = __builtin_return_address(0);
    HRESULT hr = g_core_get_interface_original(clsid, riid, ppv);
    void *obj = (SUCCEEDED(hr) && ppv) ? *ppv : nullptr;
    char callerDesc[320]; describe_address(returnAddress, callerDesc, sizeof(callerDesc));
    log_line("D3D12Core!D3D12GetInterface caller=%p %s clsid={%08lX-%04X-%04X-...} iid={%08lX-%04X-%04X-...} hr=0x%08lX object=%p",
             returnAddress, callerDesc,
             (unsigned long)clsid.Data1, (unsigned)clsid.Data2, (unsigned)clsid.Data3,
             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
             (unsigned long)hr, obj);
    if (obj && riid.Data1 == 0xDFAFDD2C) {
        void **vt = *(void ***)obj;
        for (unsigned i = 0; i < 9; ++i) {
            char desc[320]; describe_address(vt[i], desc, sizeof(desc));
            log_line("COREMODULE_VTBL[%u]=%p %s", i, vt[i], desc);
        }
    }
    if (obj) {
        /* ID3D12CoreModule IID DFAFDD2C-355F-4CB3-A8B2-EA7F9260148B. */
        if (riid.Data1 == 0xDFAFDD2C && riid.Data2 == 0x355F && riid.Data3 == 0x4CB3)
            hook_core_module_object(obj);
        hook_sdk_configuration(obj);
        hook_factory_object(obj);
    }
    return hr;
}

static bool hook_d3d12core_module(HMODULE module) {
    if (g_core_hook_installed) return true;
    if (!module) return false;
    FARPROC target = GetProcAddress(module, "D3D12GetInterface");
    if (!target) return false;
    g_core_target = (void *)target;
    void *trampoline = nullptr;
    if (!rgpu_inline_hook((void *)target, (void *)hook_core_get_interface, &trampoline))
        return false;
    g_core_get_interface_original = reinterpret_cast<fnCoreGetInterface>(trampoline);
    InterlockedExchange(&g_core_hook_installed, 1);
    return true;
}

struct RGPU_LDR_LOADED_DATA {
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
union RGPU_LDR_NOTIFICATION_DATA {
    RGPU_LDR_LOADED_DATA Loaded;
    RGPU_LDR_LOADED_DATA Unloaded;
};
typedef VOID (CALLBACK *RGPU_LDR_CALLBACK)(ULONG, const RGPU_LDR_NOTIFICATION_DATA *, PVOID);
typedef NTSTATUS (NTAPI *fnLdrRegisterDllNotification)(ULONG, RGPU_LDR_CALLBACK, PVOID, PVOID *);

static bool unicode_equals_ci(const UNICODE_STRING *s, const wchar_t *name) {
    if (!s || !s->Buffer || !name) return false;
    size_t chars = s->Length / sizeof(wchar_t);
    size_t expected = wcslen(name);
    return chars == expected && _wcsnicmp(s->Buffer, name, chars) == 0;
}

static VOID CALLBACK dll_notification(ULONG reason,
                                      const RGPU_LDR_NOTIFICATION_DATA *data,
                                      PVOID) {
    if (reason == 1 && data) {
        /* Loader-lock callback: only GetProcAddress + VirtualAlloc/Protect based
         * hook installation. Logging is deferred to a normal worker thread. */
        if (unicode_equals_ci(data->Loaded.BaseDllName, L"D3D12Core.dll"))
            hook_d3d12core_module((HMODULE)data->Loaded.DllBase);
        if (unicode_equals_ci(data->Loaded.BaseDllName, L"d3d12.dll"))
            hook_system_d3d12_module((HMODULE)data->Loaded.DllBase);
    }
}

static void register_dll_notification() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    auto reg = reinterpret_cast<fnLdrRegisterDllNotification>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (reg) reg(0, dll_notification, nullptr, &g_dll_notification_cookie);
    HMODULE already = GetModuleHandleW(L"D3D12Core.dll");
    if (already) hook_d3d12core_module(already);
}

static DWORD WINAPI core_monitor_thread(void *) {
    for (int i = 0; i < 200; ++i) {
        HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
        if (core) hook_d3d12core_module(core);

        /* Enumerate all modules because Windows can load both the game-directory
         * d3d12 proxy and the real System32 d3d12.dll under the same base name. */
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me{}; me.dwSize = sizeof(me);
            if (Module32First(snap, &me)) do {
                if (_stricmp(me.szModule, "d3d12.dll") == 0)
                    hook_system_d3d12_module(me.hModule);
            } while (Module32Next(snap, &me));
            CloseHandle(snap);
        }

        if (core && g_core_hook_installed && g_system_create_hook_installed &&
            g_system_get_interface_hook_installed) {
            log_line("runtime monitor Core=%p CoreTarget=%p CoreHook=%ld CoreCalls=%ld SystemCreateTarget=%p CreateHook=%ld CreateCalls=%ld Devices=%ld SystemGetInterfaceTarget=%p GetInterfaceHook=%ld GetInterfaceCalls=%ld",
                     (void *)core, g_core_target, g_core_hook_installed,
                     g_core_get_interface_calls, g_system_create_device_target,
                     g_system_create_hook_installed, g_system_create_calls,
                     g_system_create_devices, g_system_get_interface_target,
                     g_system_get_interface_hook_installed, g_system_get_interface_calls);
            return 0;
        }
        Sleep(50);
    }
    log_line("runtime monitor timeout CoreHook=%ld SystemCreateHook=%ld SystemGetInterfaceHook=%ld",
             g_core_hook_installed, g_system_create_hook_installed,
             g_system_get_interface_hook_installed);
    return 0;
}

static void start_core_monitor_once() {
    if (InterlockedCompareExchange(&g_core_monitor_started, 1, 0) == 0) {
        HANDLE t = CreateThread(nullptr, 0, core_monitor_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
}

static AGSReturnCode WINAPI hook_ags_create_dx12(
    AGSContext *context, const AGSDX12DeviceCreationParams *creation,
    const AGSDX12ExtensionParams *extensions, AGSDX12ReturnedParams *returned) {
    InterlockedIncrement(&g_ags_calls);
    AGSReturnCode rc = g_ags_original(context, creation, extensions, returned);
    ID3D12Device *device = returned ? returned->pDevice : nullptr;
    log_line("AGS DX12 CreateDevice rc=%d iid={%08lX-%04X-%04X-...} fl=0x%X device=%p vtbl=%p",
             rc,
             creation ? (unsigned long)creation->iid.Data1 : 0,
             creation ? (unsigned)creation->iid.Data2 : 0,
             creation ? (unsigned)creation->iid.Data3 : 0,
             creation ? (unsigned)creation->FeatureLevel : 0,
             (void *)device, device ? *(void **)device : nullptr);
    if (device) arm_device(device, "AMD_AGS");
    return rc;
}

static bool install_ags_hook() {
    if (g_ags_hook_installed) return true;
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return false;
    FARPROC target = GetProcAddress(exe, "agsDriverExtensionsDX12_CreateDevice");
    if (!target) return false;
    g_ags_target = (void *)target;
    void *trampoline = nullptr;
    if (!rgpu_inline_hook((void *)target, (void *)hook_ags_create_dx12, &trampoline))
        return false;
    g_ags_original = reinterpret_cast<fnAGSCreateDX12>(trampoline);
    InterlockedExchange(&g_ags_hook_installed, 1);
    return true;
}

static void report_once() {
    start_core_monitor_once();
    if (InterlockedCompareExchange(&g_reported, 1, 0) == 0) {
        log_line("early DXGI proxy active; AGS target=%p hook=%ld; Core target=%p hook=%ld notification=%p",
                 g_ags_target, g_ags_hook_installed, g_core_target,
                 g_core_hook_installed, g_dll_notification_cookie);
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        install_ags_hook();
        register_dll_notification();
    }
    return TRUE;
}

extern "C" {
__declspec(dllexport) BOOL WINAPI rgpu_dxgi_install_ags_hook() {
    BOOL ok = install_ags_hook() ? TRUE : FALSE;
    report_once();
    return ok;
}
__declspec(dllexport) void WINAPI rgpu_dxgi_early_stats(
    long *agsInstalled, long *agsCalls, long *arms,
    long *coreInstalled, long *coreCalls,
    long *sdkFactoryCalls, long *factoryDeviceCalls) {
    if (agsInstalled) *agsInstalled = g_ags_hook_installed;
    if (agsCalls) *agsCalls = g_ags_calls;
    if (arms) *arms = g_arm_calls;
    if (coreInstalled) *coreInstalled = g_core_hook_installed;
    if (coreCalls) *coreCalls = g_core_get_interface_calls;
    if (sdkFactoryCalls) *sdkFactoryCalls = g_sdk_factory_calls;
    if (factoryDeviceCalls) *factoryDeviceCalls = g_factory_device_calls;
}
__declspec(dllexport) void WINAPI rgpu_dxgi_system_create_stats(
    long *installed, long *calls, long *devices) {
    if (installed) *installed = g_system_create_hook_installed;
    if (calls) *calls = g_system_create_calls;
    if (devices) *devices = g_system_create_devices;
}
__declspec(dllexport) void WINAPI rgpu_dxgi_system_getinterface_stats(
    long *installed, long *calls) {
    if (installed) *installed = g_system_get_interface_hook_installed;
    if (calls) *calls = g_system_get_interface_calls;
}
__declspec(dllexport) void WINAPI rgpu_dxgi_core_table_create_stats(
    long *calls, long *devices) {
    if (calls) *calls = g_core_table_create_calls;
    if (devices) *devices = g_core_table_create_devices;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory) {
    report_once(); install_ags_hook();
    typedef HRESULT (WINAPI *Fn)(REFIID, void **);
    Fn fn = reinterpret_cast<Fn>(real_export("CreateDXGIFactory"));
    return fn ? fn(riid, ppFactory) : E_FAIL;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    report_once(); install_ags_hook();
    typedef HRESULT (WINAPI *Fn)(REFIID, void **);
    Fn fn = reinterpret_cast<Fn>(real_export("CreateDXGIFactory1"));
    return fn ? fn(riid, ppFactory) : E_FAIL;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **ppFactory) {
    report_once(); install_ags_hook();
    typedef HRESULT (WINAPI *Fn)(UINT, REFIID, void **);
    Fn fn = reinterpret_cast<Fn>(real_export("CreateDXGIFactory2"));
    return fn ? fn(flags, riid, ppFactory) : E_FAIL;
}
__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void **ppDebug) {
    typedef HRESULT (WINAPI *Fn)(UINT, REFIID, void **);
    Fn fn = reinterpret_cast<Fn>(real_export("DXGIGetDebugInterface1"));
    return fn ? fn(flags, riid, ppDebug) : E_NOINTERFACE;
}
__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    typedef HRESULT (WINAPI *Fn)();
    Fn fn = reinterpret_cast<Fn>(real_export("DXGIDeclareAdapterRemovalSupport"));
    return fn ? fn() : S_OK;
}
__declspec(dllexport) HRESULT WINAPI DXGIDisableVBlankVirtualization() {
    typedef HRESULT (WINAPI *Fn)();
    Fn fn = reinterpret_cast<Fn>(real_export("DXGIDisableVBlankVirtualization"));
    return fn ? fn() : S_OK;
}
} // extern "C"
