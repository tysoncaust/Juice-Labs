$ErrorActionPreference='Stop'
$root='C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$objdump=(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$dll='C:\Windows\System32\d3d12.dll'
$ranges=@(
 @{Name='resolver_6ba8_6e40';Start='0x180006ba8';Stop='0x180006e40'},
 @{Name='metadata_7ed0_7f80_code';Start='0x180007ed0';Stop='0x180007f80'},
 @{Name='metadata_7ed0_7f80_data';Start='0x180007ed0';Stop='0x180007f80'}
)
foreach($r in $ranges){
 $mode=if($r.Name -like '*data'){'-s'}else{'-d'}
 $out=& $objdump $mode "--start-address=$($r.Start)" "--stop-address=$($r.Stop)" $dll 2>&1
 if($LASTEXITCODE){throw "objdump failed $($r.Name)"}
 $path=Join-Path $root "test\$($r.Name).txt"
 $out | Set-Content -LiteralPath $path -Encoding utf8
 Write-Host $path
}
