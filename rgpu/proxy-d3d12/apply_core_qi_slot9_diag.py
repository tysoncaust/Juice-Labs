from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('\r\n','\n')

s=s.replace(
'''static fnCoreGetDllExports g_core_get_dll_exports_original = nullptr;\nstatic void *g_core_target = nullptr;\n''',
'''static fnCoreGetDllExports g_core_get_dll_exports_original = nullptr;\ntypedef HRESULT (STDMETHODCALLTYPE *fnCoreQueryInterface)(IUnknown *, REFIID, void **);\ntypedef HRESULT (STDMETHODCALLTYPE *fnCoreVersionedSlot9)(IUnknown *, void *);\nstatic fnCoreQueryInterface g_core_query_interface_original = nullptr;\nstatic fnCoreVersionedSlot9 g_core_versioned_slot9_original = nullptr;\nstatic void *g_core_target = nullptr;\n''',1)
s=s.replace(
'''static volatile LONG g_core_exports_calls = 0;\n''',
'''static volatile LONG g_core_exports_calls = 0;\nstatic volatile LONG g_core_qi_calls = 0;\nstatic volatile LONG g_core_versioned_slot9_calls = 0;\n''',1)

needle='''static HRESULT STDMETHODCALLTYPE hook_core_get_dll_exports(IUnknown *self, void *table) {\n'''
block=r'''static HRESULT STDMETHODCALLTYPE hook_core_versioned_slot9(IUnknown *self, void *payload) {
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
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_core_get_dll_exports(IUnknown *self, void *table) {
'''
if s.count(needle)!=1: raise RuntimeError('GetDllExports insertion point missing')
s=s.replace(needle,block,1)

old='''    bool ok = patch_slot(obj, 8, (void *)hook_core_get_dll_exports,\n                         (void **)&g_core_get_dll_exports_original);\n    if (ok) g_core_module_vtbl = vtbl;\n    log_line("ID3D12CoreModule observed object=%p vtbl=%p GetDllExports-hook=%d",\n             obj, vtbl, ok ? 1 : 0);\n    return ok;\n'''
new='''    bool qiOk = patch_slot(obj, 0, (void *)hook_core_query_interface,\n                           (void **)&g_core_query_interface_original);\n    bool exportsOk = patch_slot(obj, 8, (void *)hook_core_get_dll_exports,\n                                (void **)&g_core_get_dll_exports_original);\n    if (qiOk && exportsOk) g_core_module_vtbl = vtbl;\n    log_line("ID3D12CoreModule observed object=%p vtbl=%p QI-hook=%d GetDllExports-hook=%d",\n             obj, vtbl, qiOk ? 1 : 0, exportsOk ? 1 : 0);\n    return qiOk && exportsOk;\n'''
if s.count(old)!=1: raise RuntimeError('core module hook block missing')
s=s.replace(old,new,1)

p.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Added CoreModule QueryInterface and versioned slot-9 diagnostics.')
