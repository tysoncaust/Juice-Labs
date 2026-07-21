from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('\r\n','\n')

s=s.replace(
'''typedef HRESULT (STDMETHODCALLTYPE *fnCoreVersionedSlot9)(IUnknown *, void *);\nstatic fnCoreQueryInterface g_core_query_interface_original = nullptr;\nstatic fnCoreVersionedSlot9 g_core_versioned_slot9_original = nullptr;\n''',
'''typedef HRESULT (STDMETHODCALLTYPE *fnCoreVersionedSlot9)(IUnknown *, void *);\ntypedef HRESULT (STDMETHODCALLTYPE *fnCoreVersionedSlot11)(IUnknown *, void *);\nstatic fnCoreQueryInterface g_core_query_interface_original = nullptr;\nstatic fnCoreVersionedSlot9 g_core_versioned_slot9_original = nullptr;\nstatic fnCoreVersionedSlot11 g_core_versioned_slot11_original = nullptr;\n''',1)
s=s.replace(
'''static volatile LONG g_core_versioned_slot9_calls = 0;\n''',
'''static volatile LONG g_core_versioned_slot9_calls = 0;\nstatic volatile LONG g_core_versioned_slot11_calls = 0;\n''',1)

needle='''static HRESULT STDMETHODCALLTYPE hook_core_versioned_slot9(IUnknown *self, void *payload) {\n'''
block=r'''static HRESULT STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {
    InterlockedIncrement(&g_core_versioned_slot11_calls);
    log_line("CORE_VERSIONED_SLOT11 enter self=%p vtbl=%p exports=%p", self,
             self ? *(void **)self : nullptr, exportsTable);
    HRESULT hr = g_core_versioned_slot11_original(self, exportsTable);
    log_line("CORE_VERSIONED_SLOT11 leave hr=0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr) && exportsTable && readable_qwords(exportsTable, 18 * sizeof(uint64_t))) {
        uint64_t *q = (uint64_t *)exportsTable;
        for (unsigned i = 0; i < 18; ++i) {
            char desc[320]; describe_address((void *)(uintptr_t)q[i], desc, sizeof(desc));
            log_line("CORE_EXPORT_TABLE[%02u]=0x%016llX %s", i,
                     (unsigned long long)q[i], desc);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_core_versioned_slot9(IUnknown *self, void *payload) {
'''
if s.count(needle)!=1: raise RuntimeError('slot9 insertion point missing')
s=s.replace(needle,block,1)

old='''        if (riid.Data1 == 0xD5010570 && riid.Data2 == 0x699A && riid.Data3 == 0x47DF) {\n            bool ok = patch_slot(obj, 9, (void *)hook_core_versioned_slot9,\n                                 (void **)&g_core_versioned_slot9_original);\n            log_line("CORE versioned D5010570 slot9 hook=%d original=%p",\n                     ok ? 1 : 0, (void *)g_core_versioned_slot9_original);\n        }\n'''
new='''        if (riid.Data1 == 0xD5010570 && riid.Data2 == 0x699A && riid.Data3 == 0x47DF) {\n            bool ok = patch_slot(obj, 9, (void *)hook_core_versioned_slot9,\n                                 (void **)&g_core_versioned_slot9_original);\n            log_line("CORE versioned D5010570 slot9 hook=%d original=%p",\n                     ok ? 1 : 0, (void *)g_core_versioned_slot9_original);\n        }\n        if (riid.Data1 == 0xFC454290 && riid.Data2 == 0x1B19 && riid.Data3 == 0x48C4) {\n            bool ok = patch_slot(obj, 11, (void *)hook_core_versioned_slot11,\n                                 (void **)&g_core_versioned_slot11_original);\n            log_line("CORE versioned FC454290 slot11 hook=%d original=%p",\n                     ok ? 1 : 0, (void *)g_core_versioned_slot11_original);\n        }\n'''
if s.count(old)!=1: raise RuntimeError('QI hook-selection block missing')
s=s.replace(old,new,1)

p.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Added FC454290 slot-11 export-table capture.')
