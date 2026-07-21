from pathlib import Path
root=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12')

b=root/'build_ags_early.ps1'
s=b.read_text(encoding='utf-8').replace('\r\n','\n')
old='''    "$here\\src\\rgpu_dxgi_early.cpp" -o "$test\\rgpu_dxgi_early.dll" -ldxguid\n'''
new='''    "$here\\src\\rgpu_dxgi_early.cpp" "$here\\src\\rgpu_d3d12_slots.cpp" `\n    -o "$test\\rgpu_dxgi_early.dll" -ldxguid\n'''
if s.count(old)!=1: raise RuntimeError(f'build fragment matches={s.count(old)}')
s=s.replace(old,new,1)
b.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')

h=root/'test'/'rgpu_ags_early_harness.cpp'
s=h.read_text(encoding='utf-8').replace('\r\n','\n')
s=s.replace('typedef void (WINAPI *pEarlyStats)(long *, long *, long *);',
'''typedef void (WINAPI *pEarlyStats)(long *, long *, long *, long *, long *, long *, long *);''')
s=s.replace('''    stats(&installed, &calls, &arms);\n''','''    long coreInstalled=0, coreCalls=0, sdkFactoryCalls=0, factoryDeviceCalls=0;\n    stats(&installed, &calls, &arms, &coreInstalled, &coreCalls, &sdkFactoryCalls, &factoryDeviceCalls);\n''')
s=s.replace('''    std::printf("AGS rc=%d device=%p | hook=%ld calls=%ld external-arms=%ld\\n",\n                rc, (void *)rp.pDevice, installed, calls, arms);\n''','''    std::printf("AGS rc=%d device=%p | hook=%ld calls=%ld external-arms=%ld | core-hook=%ld core-calls=%ld\\n",\n                rc, (void *)rp.pDevice, installed, calls, arms, coreInstalled, coreCalls);\n''')
h.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print('Updated early DXGI build linkage and harness stats signature.')
