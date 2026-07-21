$ErrorActionPreference = 'Stop'
$root = 'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$core = 'C:\Program Files\Tokyo Xtreme Racer\TokyoXtremeRacer\Binaries\Win64\D3D12\x64\D3D12Core.dll'
$objdump = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$out = & $objdump -p $core 2>&1
$filtered = $out | Where-Object { $_ -match '(?i)D3D12.*Create|ValidateAndCreate|GetInterface|SDKVersion|DLL Name:' }
$path = Join-Path $root 'test\txr_d3d12core_exports.txt'
$filtered | Set-Content -LiteralPath $path -Encoding utf8
$filtered
