from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('\r\n','\n')

s=s.replace(
'''static fnSystemD3D12CreateDevice g_system_create_device_original = nullptr;\n''',
'''static fnSystemD3D12CreateDevice g_system_create_device_original = nullptr;\nstatic fnSystemD3D12CreateDevice g_core_table_create_device_original = nullptr;\n''',1)
s=s.replace(
'''static volatile LONG g_system_create_devices = 0;\n''',
'''static volatile LONG g_system_create_devices = 0;\nstatic volatile LONG g_core_table_create_calls = 0;\nstatic volatile LONG g_core_table_create_devices = 0;\n''',1)

needle='''static uintptr_t STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {\n'''
block=r'''static HRESULT WINAPI hook_core_table_create_device(
    IUnknown *adapter, D3D_FEATURE_LEVEL featureLevel, REFIID riid, void **ppDevice) {
    InterlockedIncrement(&g_core_table_create_calls);
    void *returnAddress = __builtin_return_address(0);
    char callerDesc[320]; describe_address(returnAddress, callerDesc, sizeof(callerDesc));
    HRESULT hr = g_core_table_create_device_original(adapter, featureLevel, riid, ppDevice);
    ID3D12Device *device = (SUCCEEDED(hr) && ppDevice) ? (ID3D12Device *)*ppDevice : nullptr;
    log_line("CORE_TABLE[2] CreateDevice caller=%p %s iid={%08lX-%04X-%04X-...} fl=0x%X hr=0x%08lX device=%p vtbl=%p",
             returnAddress, callerDesc,
             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,
             (unsigned)featureLevel, (unsigned long)hr, (void *)device,
             device ? *(void **)device : nullptr);
    if (device) {
        InterlockedIncrement(&g_core_table_create_devices);
        arm_device(device, "Core_DispatchTable_CreateDevice");
    }
    return hr;
}

static uintptr_t STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {
'''
if s.count(needle)!=1: raise RuntimeError('slot11 function insertion point missing')
s=s.replace(needle,block,1)

old='''    if (exportsTable && readable_qwords(exportsTable, 18 * sizeof(uint64_t))) {\n        uint64_t *q = (uint64_t *)exportsTable;\n        for (unsigned i = 0; i < 18; ++i) {\n            char desc[320]; describe_address((void *)(uintptr_t)q[i], desc, sizeof(desc));\n            log_line("CORE_EXPORT_TABLE[%02u]=0x%016llX %s", i,\n                     (unsigned long long)q[i], desc);\n        }\n    }\n'''
new='''    if (exportsTable && readable_qwords(exportsTable, 18 * sizeof(uint64_t))) {\n        uint64_t *q = (uint64_t *)exportsTable;\n        for (unsigned i = 0; i < 18; ++i) {\n            char desc[320]; describe_address((void *)(uintptr_t)q[i], desc, sizeof(desc));\n            log_line("CORE_EXPORT_TABLE[%02u]=0x%016llX %s", i,\n                     (unsigned long long)q[i], desc);\n        }\n\n        /* The dispatch table indexes match d3d12.dll's export-address-table\n         * indexes: index 11 is D3D12GetInterface, therefore index 2 is\n         * D3D12CreateDevice. Replace the writable per-loader copy only. */\n        if (!g_core_table_create_device_original && q[2]) {\n            g_core_table_create_device_original =\n                reinterpret_cast<fnSystemD3D12CreateDevice>((uintptr_t)q[2]);\n            DWORD oldProtect = 0;\n            void **entry = reinterpret_cast<void **>(&q[2]);\n            if (VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &oldProtect)) {\n                *entry = (void *)hook_core_table_create_device;\n                DWORD ignored = 0;\n                VirtualProtect(entry, sizeof(void *), oldProtect, &ignored);\n                FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void *));\n                log_line("CORE_EXPORT_TABLE[02] hooked original=%p detour=%p",\n                         (void *)g_core_table_create_device_original,\n                         (void *)hook_core_table_create_device);\n            } else {\n                log_line("CORE_EXPORT_TABLE[02] hook failed VirtualProtect=%lu",\n                         GetLastError());\n                g_core_table_create_device_original = nullptr;\n            }\n        }\n    }\n'''
if s.count(old)!=1: raise RuntimeError('table dump block missing')
s=s.replace(old,new,1)

stats_marker='''__declspec(dllexport) void WINAPI rgpu_dxgi_system_getinterface_stats(\n    long *installed, long *calls) {\n    if (installed) *installed = g_system_get_interface_hook_installed;\n    if (calls) *calls = g_system_get_interface_calls;\n}\n'''
stats_new=stats_marker+'''__declspec(dllexport) void WINAPI rgpu_dxgi_core_table_create_stats(\n    long *calls, long *devices) {\n    if (calls) *calls = g_core_table_create_calls;\n    if (devices) *devices = g_core_table_create_devices;\n}\n'''
if s.count(stats_marker)!=1: raise RuntimeError('stats insertion point missing')
s=s.replace(stats_marker,stats_new,1)

p.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Added per-loader Core dispatch-table D3D12CreateDevice detour.')
