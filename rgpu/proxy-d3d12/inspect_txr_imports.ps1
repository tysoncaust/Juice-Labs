$ErrorActionPreference = 'Stop'
$exe = 'C:\Program Files\Tokyo Xtreme Racer\TokyoXtremeRacer\Binaries\Win64\TokyoXtremeRacer-Win64-Shipping.exe'
$root = 'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$objdump = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$out = & $objdump -p $exe 2>&1
$filtered = $out | Where-Object { $_ -match '(?i)DLL Name:|D3D12|DXGI|DML|CreateDevice|GetInterface' }
$path = Join-Path $root 'test\txr_imports_filtered.txt'
$filtered | Set-Content -LiteralPath $path -Encoding utf8
$filtered
