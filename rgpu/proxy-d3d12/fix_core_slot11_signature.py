from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('\r\n','\n')
s=s.replace('typedef HRESULT (STDMETHODCALLTYPE *fnCoreVersionedSlot11)(IUnknown *, void *);',
            'typedef uintptr_t (STDMETHODCALLTYPE *fnCoreVersionedSlot11)(IUnknown *, void *);')
s=s.replace('static HRESULT STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {',
            'static uintptr_t STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {')
s=s.replace('''    HRESULT hr = g_core_versioned_slot11_original(self, exportsTable);\n    log_line("CORE_VERSIONED_SLOT11 leave hr=0x%08lX", (unsigned long)hr);\n    if (SUCCEEDED(hr) && exportsTable && readable_qwords(exportsTable, 18 * sizeof(uint64_t))) {\n''','''    uintptr_t result = g_core_versioned_slot11_original(self, exportsTable);\n    log_line("CORE_VERSIONED_SLOT11 leave result=0x%016llX", (unsigned long long)result);\n    if (exportsTable && readable_qwords(exportsTable, 18 * sizeof(uint64_t))) {\n''')
s=s.replace('''    return hr;\n}\n\nstatic HRESULT STDMETHODCALLTYPE hook_core_versioned_slot9''','''    return result;\n}\n\nstatic HRESULT STDMETHODCALLTYPE hook_core_versioned_slot9''')
p.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Corrected Core slot-11 return signature and unconditional export-table dump.')
