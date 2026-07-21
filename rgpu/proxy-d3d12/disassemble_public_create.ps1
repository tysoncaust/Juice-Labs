$ErrorActionPreference='Stop'
$root='C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$objdump=(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$dll='C:\Windows\System32\d3d12.dll'
$out=& $objdump -d '--start-address=0x180006b30' '--stop-address=0x180006cf0' $dll 2>&1
if($LASTEXITCODE){throw 'objdump failed'}
$path=Join-Path $root 'test\d3d12_public_create_6b30_6cf0.txt'
$out | Set-Content -LiteralPath $path -Encoding utf8
Write-Host $path
