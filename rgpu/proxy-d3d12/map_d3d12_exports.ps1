$ErrorActionPreference='Stop'
$root='C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$objdump=(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$dll='C:\Windows\System32\d3d12.dll'
$out=& $objdump -p $dll 2>&1
$start=($out | Select-String -SimpleMatch '[Ordinal/Name Pointer] Table' | Select-Object -First 1).LineNumber
if(-not $start){throw 'Export name table not found'}
$excerpt=$out[($start-8)..([Math]::Min($out.Count-1,$start+80))]
$path=Join-Path $root 'test\d3d12_export_table.txt'
$excerpt | Set-Content -LiteralPath $path -Encoding utf8
$excerpt
