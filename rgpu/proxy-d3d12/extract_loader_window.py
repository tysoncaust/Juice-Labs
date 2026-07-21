from pathlib import Path
src=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\test\d3d12_loader_9700_9c00.txt')
lines=src.read_text(encoding='utf-8-sig',errors='replace').splitlines()
idx=next(i for i,l in enumerate(lines) if '18000997c:' in l)
start=max(0,idx-45); end=min(len(lines),idx+80)
out=src.with_name('d3d12_loader_around_997c.txt')
out.write_text('\n'.join(f'{i+1}: {lines[i]}' for i in range(start,end)),encoding='utf-8')
print(out)
