from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_dxgi_early.cpp')
s=p.read_text(encoding='utf-8').replace('_wcsicmp(me.szModule, L"d3d12.dll")','_stricmp(me.szModule, "d3d12.dll")')
p.write_text(s,encoding='utf-8')
print('Fixed MODULEENTRY32 ANSI module-name comparison.')
