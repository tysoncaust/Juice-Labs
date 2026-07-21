from pathlib import Path
p=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\src\rgpu_d3d12_proxy.cpp')
lines=p.read_text(encoding='utf-8').splitlines()
out=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\test\proxy_mid_excerpt.txt')
out.write_text('\n'.join(f'{i+1}: {line}' for i,line in enumerate(lines[190:305],start=190)),encoding='utf-8')
print(out)
