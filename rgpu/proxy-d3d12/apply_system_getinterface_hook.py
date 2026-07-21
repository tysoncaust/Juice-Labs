from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('\r\n','\n')

s=s.replace(
'''static fnSystemD3D12CreateDevice g_system_create_device_original = nullptr;\nstatic void *g_system_create_device_target = nullptr;\n''',
'''static fnSystemD3D12CreateDevice g_system_create_device_original = nullptr;\nstatic fnCoreGetInterface g_system_get_interface_original = nullptr;\nstatic void *g_system_create_device_target = nullptr;\nstatic void *g_system_get_interface_target = nullptr;\n''',1)
s=s.replace(
'''static volatile LONG g_system_create_hook_installed = 0;\nstatic volatile LONG g_system_create_calls = 0;\nstatic volatile LONG g_system_create_devices = 0;\n''',
'''static volatile LONG g_system_create_hook_installed = 0;\nstatic volatile LONG g_system_get_interface_hook_installed = 0;\nstatic volatile LONG g_system_create_calls = 0;\nstatic volatile LONG g_system_create_devices = 0;\nstatic volatile LONG g_system_get_interface_calls = 0;\n''',1)

needle='''static HRESULT WINAPI hook_system_d3d12_create_device(\n'''
insert='''static bool hook_factory_object(void *obj);\nstatic bool hook_sdk_configuration(void *obj);\n\nstatic HRESULT WINAPI hook_system_d3d12_get_interface(\n    REFCLSID clsid, REFIID riid, void **ppv) {\n    InterlockedIncrement(&g_system_get_interface_calls);\n    HRESULT hr = g_system_get_interface_original(clsid, riid, ppv);\n    void *obj = (SUCCEEDED(hr) && ppv) ? *ppv : nullptr;\n    log_line("SYSTEM d3d12!D3D12GetInterface clsid={%08lX-%04X-%04X-...} iid={%08lX-%04X-%04X-...} hr=0x%08lX object=%p",\n             (unsigned long)clsid.Data1, (unsigned)clsid.Data2, (unsigned)clsid.Data3,\n             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,\n             (unsigned long)hr, obj);\n    if (obj) {\n        hook_sdk_configuration(obj);\n        hook_factory_object(obj);\n    }\n    return hr;\n}\n\nstatic HRESULT WINAPI hook_system_d3d12_create_device(\n'''
if s.count(needle)!=1: raise RuntimeError('system create insertion point missing')
s=s.replace(needle,insert,1)

start=s.index('static bool hook_system_d3d12_module(HMODULE module) {')
end=s.index('\nstatic bool patch_slot',start)
old=s[start:end]
new='''static bool hook_system_d3d12_module(HMODULE module) {\n    if (!module) return false;\n    if (GetProcAddress(module, "rgpu_d3d12_arm_external_device"))\n        return false; /* our game-directory proxy, not the system runtime */\n\n    if (!g_system_create_hook_installed) {\n        FARPROC target = GetProcAddress(module, "D3D12CreateDevice");\n        if (target) {\n            void *trampoline = nullptr;\n            g_system_create_device_target = (void *)target;\n            if (rgpu_inline_hook((void *)target, (void *)hook_system_d3d12_create_device, &trampoline)) {\n                g_system_create_device_original = reinterpret_cast<fnSystemD3D12CreateDevice>(trampoline);\n                InterlockedExchange(&g_system_create_hook_installed, 1);\n            }\n        }\n    }\n\n    if (!g_system_get_interface_hook_installed) {\n        FARPROC target = GetProcAddress(module, "D3D12GetInterface");\n        if (target) {\n            void *trampoline = nullptr;\n            g_system_get_interface_target = (void *)target;\n            if (rgpu_inline_hook((void *)target, (void *)hook_system_d3d12_get_interface, &trampoline)) {\n                g_system_get_interface_original = reinterpret_cast<fnCoreGetInterface>(trampoline);\n                InterlockedExchange(&g_system_get_interface_hook_installed, 1);\n            }\n        }\n    }\n\n    return g_system_create_hook_installed && g_system_get_interface_hook_installed;\n}\n'''
s=s[:start]+new+s[end:]

s=s.replace(
'''        if (core && g_core_hook_installed && g_system_create_hook_installed) {\n            log_line("runtime monitor Core=%p CoreTarget=%p CoreHook=%ld CoreCalls=%ld SystemCreateTarget=%p SystemHook=%ld SystemCalls=%ld Devices=%ld",\n                     (void *)core, g_core_target, g_core_hook_installed,\n                     g_core_get_interface_calls, g_system_create_device_target,\n                     g_system_create_hook_installed, g_system_create_calls,\n                     g_system_create_devices);\n''',
'''        if (core && g_core_hook_installed && g_system_create_hook_installed &&\n            g_system_get_interface_hook_installed) {\n            log_line("runtime monitor Core=%p CoreTarget=%p CoreHook=%ld CoreCalls=%ld SystemCreateTarget=%p CreateHook=%ld CreateCalls=%ld Devices=%ld SystemGetInterfaceTarget=%p GetInterfaceHook=%ld GetInterfaceCalls=%ld",\n                     (void *)core, g_core_target, g_core_hook_installed,\n                     g_core_get_interface_calls, g_system_create_device_target,\n                     g_system_create_hook_installed, g_system_create_calls,\n                     g_system_create_devices, g_system_get_interface_target,\n                     g_system_get_interface_hook_installed, g_system_get_interface_calls);\n''',1)
s=s.replace(
'''    log_line("runtime monitor timeout CoreHook=%ld SystemHook=%ld",\n             g_core_hook_installed, g_system_create_hook_installed);\n''',
'''    log_line("runtime monitor timeout CoreHook=%ld SystemCreateHook=%ld SystemGetInterfaceHook=%ld",\n             g_core_hook_installed, g_system_create_hook_installed,\n             g_system_get_interface_hook_installed);\n''',1)

s=s.replace(
'''__declspec(dllexport) void WINAPI rgpu_dxgi_system_create_stats(\n    long *installed, long *calls, long *devices) {\n    if (installed) *installed = g_system_create_hook_installed;\n    if (calls) *calls = g_system_create_calls;\n    if (devices) *devices = g_system_create_devices;\n}\n''',
'''__declspec(dllexport) void WINAPI rgpu_dxgi_system_create_stats(\n    long *installed, long *calls, long *devices) {\n    if (installed) *installed = g_system_create_hook_installed;\n    if (calls) *calls = g_system_create_calls;\n    if (devices) *devices = g_system_create_devices;\n}\n__declspec(dllexport) void WINAPI rgpu_dxgi_system_getinterface_stats(\n    long *installed, long *calls) {\n    if (installed) *installed = g_system_get_interface_hook_installed;\n    if (calls) *calls = g_system_get_interface_calls;\n}\n''',1)

p.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Added global system d3d12!D3D12GetInterface interception.')
