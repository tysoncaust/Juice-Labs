from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('\r\n','\n')

needle='''typedef HRESULT (WINAPI *fnCoreGetInterface)(REFCLSID, REFIID, void **);\ntypedef HRESULT (STDMETHODCALLTYPE *fnSDKCreateDeviceFactory)(\n'''
repl='''typedef HRESULT (WINAPI *fnCoreGetInterface)(REFCLSID, REFIID, void **);\ntypedef HRESULT (STDMETHODCALLTYPE *fnCoreGetDllExports)(IUnknown *, void *);\ntypedef HRESULT (STDMETHODCALLTYPE *fnSDKCreateDeviceFactory)(\n'''
if s.count(needle)!=1: raise RuntimeError('typedef insertion point missing')
s=s.replace(needle,repl,1)

needle='''static fnCoreGetInterface g_core_get_interface_original = nullptr;\nstatic void *g_core_target = nullptr;\n'''
repl='''static fnCoreGetInterface g_core_get_interface_original = nullptr;\nstatic fnCoreGetDllExports g_core_get_dll_exports_original = nullptr;\nstatic void *g_core_target = nullptr;\nstatic void *g_core_module_vtbl = nullptr;\nstatic volatile LONG g_core_exports_calls = 0;\n'''
if s.count(needle)!=1: raise RuntimeError('global insertion point missing')
s=s.replace(needle,repl,1)

needle='''static HRESULT WINAPI hook_core_get_interface(REFCLSID clsid, REFIID riid, void **ppv) {\n'''
block=r'''static bool readable_qwords(void *p, size_t bytes) {
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
    bool ok = patch_slot(obj, 8, (void *)hook_core_get_dll_exports,
                         (void **)&g_core_get_dll_exports_original);
    if (ok) g_core_module_vtbl = vtbl;
    log_line("ID3D12CoreModule observed object=%p vtbl=%p GetDllExports-hook=%d",
             obj, vtbl, ok ? 1 : 0);
    return ok;
}

static HRESULT WINAPI hook_core_get_interface(REFCLSID clsid, REFIID riid, void **ppv) {
'''
if s.count(needle)!=1: raise RuntimeError('core getinterface insertion point missing')
s=s.replace(needle,block,1)

needle='''    if (obj) {\n        hook_sdk_configuration(obj);\n        hook_factory_object(obj);\n    }\n'''
repl='''    if (obj) {\n        /* ID3D12CoreModule IID DFAFDD2C-355F-4CB3-A8B2-EA7F9260148B. */\n        if (riid.Data1 == 0xDFAFDD2C && riid.Data2 == 0x355F && riid.Data3 == 0x4CB3)\n            hook_core_module_object(obj);\n        hook_sdk_configuration(obj);\n        hook_factory_object(obj);\n    }\n'''
if s.count(needle)!=1: raise RuntimeError('core object observer block missing')
s=s.replace(needle,repl,1)

p.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Added ID3D12CoreModule::GetDllExports table dump diagnostic.')
