from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\apply_core_create_table_hook.py')
s=p.read_text(encoding='utf-8')
s=s.replace("needle='''static HRESULT STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {\\n'''",
            "needle='''static uintptr_t STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {\\n'''")
s=s.replace("block=r'''static HRESULT WINAPI hook_core_table_create_device(",
            "block=r'''static HRESULT WINAPI hook_core_table_create_device(")
s=s.replace("static HRESULT STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {",
            "static uintptr_t STDMETHODCALLTYPE hook_core_versioned_slot11(IUnknown *self, void *exportsTable) {")
p.write_text(s,encoding='utf-8')
print('Adjusted table-hook patcher for pointer-sized slot-11 signature.')
