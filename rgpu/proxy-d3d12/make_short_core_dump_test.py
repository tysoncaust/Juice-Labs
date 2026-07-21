from pathlib import Path
root=Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12')
s=(root/'test_txr_ags_early_live.ps1').read_text(encoding='utf-8').replace('\r\n','\n')
s=s.replace('duration_seconds = 90','duration_seconds = 30',1)
s=s.replace('for ($i = 0; $i -lt 18; $i++)','for ($i = 0; $i -lt 6; $i++)',1)
s=s.replace('$aliveSamples -ge 12','$aliveSamples -ge 4',1)
out=root/'test_txr_core_exports_dump_live.ps1'
out.write_text(s.replace('\n','\r\n'),encoding='utf-8',newline='')
print(out)
