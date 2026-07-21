from pathlib import Path
p = Path(r'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12\test_txr_sdkconfig1_live.ps1')
s = p.read_text(encoding='utf-8').replace('\r\n','\n')
s = s.replace('$ps = Get-TxrProcesses\n', '$ps = @(Get-TxrProcesses)\n')
s = s.replace("([regex]::Matches($log, 'D3D12CreateDevice fl=')).Count", "([regex]::Matches($log, 'D3D12CreateDevice riid=')).Count")
p.write_text(s.replace('\n','\r\n'), encoding='utf-8', newline='')
print('Updated TXR live-test process counting and D3D12CreateDevice marker.')
