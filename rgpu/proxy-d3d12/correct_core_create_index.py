from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('\r\n','\n')
s=s.replace('CORE_TABLE[2] CreateDevice','CORE_TABLE[0] CreateDevice')
s=s.replace('''        /* The dispatch table indexes match d3d12.dll's export-address-table\n         * indexes: index 11 is D3D12GetInterface, therefore index 2 is\n         * D3D12CreateDevice. Replace the writable per-loader copy only. */\n        if (!g_core_table_create_device_original && q[2]) {\n            g_core_table_create_device_original =\n                reinterpret_cast<fnSystemD3D12CreateDevice>((uintptr_t)q[2]);\n            DWORD oldProtect = 0;\n            void **entry = reinterpret_cast<void **>(&q[2]);\n''','''        /* d3d12.dll's D3D12CreateDevice implementation obtains this table\n         * and executes the first qword (mov (%rax), %rax). Replace only the\n         * writable per-loader table copy; retain the original Core pointer. */\n        if (!g_core_table_create_device_original && q[0]) {\n            g_core_table_create_device_original =\n                reinterpret_cast<fnSystemD3D12CreateDevice>((uintptr_t)q[0]);\n            DWORD oldProtect = 0;\n            void **entry = reinterpret_cast<void **>(&q[0]);\n''')
s=s.replace('CORE_EXPORT_TABLE[02] hooked','CORE_EXPORT_TABLE[00] hooked')
s=s.replace('CORE_EXPORT_TABLE[02] hook failed','CORE_EXPORT_TABLE[00] hook failed')
p.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Corrected Core dispatch CreateDevice index from 2 to 0.')
