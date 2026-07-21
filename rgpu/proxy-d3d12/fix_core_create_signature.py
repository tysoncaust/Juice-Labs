from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('\r\n','\n')

s=s.replace(
'''typedef HRESULT (WINAPI *fnSystemD3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);\n''',
'''typedef HRESULT (WINAPI *fnSystemD3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);\n/* Internal dispatch entry 0, recovered from d3d12.dll+0x7F20. */\ntypedef HRESULT (WINAPI *fnCoreDispatchCreateDevice)(\n    IUnknown *, D3D_FEATURE_LEVEL, unsigned char, const void *,\n    uintptr_t, REFIID, void **);\n''',1)
s=s.replace(
'''static fnSystemD3D12CreateDevice g_core_table_create_device_original = nullptr;\n''',
'''static fnCoreDispatchCreateDevice g_core_table_create_device_original = nullptr;\n''',1)

old='''static HRESULT WINAPI hook_core_table_create_device(\n    IUnknown *adapter, D3D_FEATURE_LEVEL featureLevel, REFIID riid, void **ppDevice) {\n    InterlockedIncrement(&g_core_table_create_calls);\n    void *returnAddress = __builtin_return_address(0);\n    char callerDesc[320]; describe_address(returnAddress, callerDesc, sizeof(callerDesc));\n    HRESULT hr = g_core_table_create_device_original(adapter, featureLevel, riid, ppDevice);\n    ID3D12Device *device = (SUCCEEDED(hr) && ppDevice) ? (ID3D12Device *)*ppDevice : nullptr;\n    log_line("CORE_TABLE[0] CreateDevice caller=%p %s iid={%08lX-%04X-%04X-...} fl=0x%X hr=0x%08lX device=%p vtbl=%p",\n             returnAddress, callerDesc,\n             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,\n             (unsigned)featureLevel, (unsigned long)hr, (void *)device,\n             device ? *(void **)device : nullptr);\n'''
new='''static HRESULT WINAPI hook_core_table_create_device(\n    IUnknown *adapter, D3D_FEATURE_LEVEL featureLevel, unsigned char createFlag,\n    const void *creationContext, uintptr_t mode, REFIID riid, void **ppDevice) {\n    InterlockedIncrement(&g_core_table_create_calls);\n    void *returnAddress = __builtin_return_address(0);\n    char callerDesc[320]; describe_address(returnAddress, callerDesc, sizeof(callerDesc));\n    HRESULT hr = g_core_table_create_device_original(\n        adapter, featureLevel, createFlag, creationContext, mode, riid, ppDevice);\n    ID3D12Device *device = (SUCCEEDED(hr) && ppDevice) ? (ID3D12Device *)*ppDevice : nullptr;\n    log_line("CORE_TABLE[0] CreateDevice caller=%p %s iid={%08lX-%04X-%04X-...} fl=0x%X flag=%u context=%p mode=0x%llX hr=0x%08lX device=%p vtbl=%p",\n             returnAddress, callerDesc,\n             (unsigned long)riid.Data1, (unsigned)riid.Data2, (unsigned)riid.Data3,\n             (unsigned)featureLevel, (unsigned)createFlag, creationContext,\n             (unsigned long long)mode, (unsigned long)hr, (void *)device,\n             device ? *(void **)device : nullptr);\n'''
if s.count(old)!=1: raise RuntimeError(f'old Core create detour matches={s.count(old)}')
s=s.replace(old,new,1)
s=s.replace(
'''reinterpret_cast<fnSystemD3D12CreateDevice>((uintptr_t)q[0]);\n''',
'''reinterpret_cast<fnCoreDispatchCreateDevice>((uintptr_t)q[0]);\n''',1)

p.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Corrected internal Core dispatch CreateDevice to its seven-argument ABI.')
