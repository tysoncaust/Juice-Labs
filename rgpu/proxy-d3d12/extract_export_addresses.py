from pathlib import Path
import subprocess, os
root=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12')
# locate objdump
base=Path(os.environ['LOCALAPPDATA'])/'Microsoft'/'WinGet'/'Packages'
objdump=next(base.glob('*WinLibs*/mingw64/bin/objdump.exe'))
out=subprocess.run([str(objdump),'-p',r'C:\Windows\System32\d3d12.dll'],capture_output=True,text=True,check=True).stdout.splitlines()
idx=next(i for i,l in enumerate(out) if '[Export Address Table]' in l)
end=next(i for i in range(idx+1,len(out)) if '[Ordinal/Name Pointer]' in out[i])
path=root/'test'/'d3d12_export_addresses_only.txt'
path.write_text('\n'.join(out[idx:end]),encoding='utf-8')
print(path)
