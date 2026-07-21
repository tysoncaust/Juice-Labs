$ErrorActionPreference='Stop'
$root='C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$objdump=(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$dll='C:\Windows\System32\d3d12.dll'
$ranges=@(
 @{Name='ctor_fa40_fc40'; Start='0x18000fa40'; Stop='0x18000fc40'},
 @{Name='callbacks_10200_10660'; Start='0x180010200'; Stop='0x180010660'},
 @{Name='callback_7d60_7e20'; Start='0x180007d60'; Stop='0x180007e20'},
 @{Name='callback_8f80_9060'; Start='0x180008f80'; Stop='0x180009060'},
 @{Name='callback_eea0_ef80'; Start='0x18000eea0'; Stop='0x18000ef80'}
)
foreach($r in $ranges){
 $out=& $objdump -d "--start-address=$($r.Start)" "--stop-address=$($r.Stop)" $dll 2>&1
 if($LASTEXITCODE){throw "objdump failed $($r.Name)"}
 $path=Join-Path $root "test\$($r.Name).txt"
 $out | Set-Content -LiteralPath $path -Encoding utf8
 Write-Host $path
}
