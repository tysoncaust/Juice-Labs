$ErrorActionPreference='Stop'
$root='C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$objdump=(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$dll='C:\Windows\System32\d3d12.dll'
$imageBase=[UInt64]0x180000000
$start=$imageBase + [UInt64]0x9700
$stop=$imageBase + [UInt64]0x9c00
$args=@('-d',('--start-address=0x{0:X}' -f $start),('--stop-address=0x{0:X}' -f $stop),$dll)
$out=& $objdump @args 2>&1
if($LASTEXITCODE){ throw "objdump failed: $LASTEXITCODE" }
$path=Join-Path $root 'test\d3d12_loader_9700_9c00.txt'
$out | Set-Content -LiteralPath $path -Encoding utf8
Write-Host "ImageBase=0x$('{0:X}' -f $imageBase) output=$path"
