$ErrorActionPreference='Stop'
$root='C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$objdump=(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$dll='C:\Windows\System32\d3d12.dll'
$jobs=@(
 @{Name='sections'; Args=@('-h',$dll)},
 @{Name='data_163c0_16500'; Args=@('-s','--start-address=0x1800163c0','--stop-address=0x180016500',$dll)},
 @{Name='code_163c0_16500'; Args=@('-d','--start-address=0x1800163c0','--stop-address=0x180016500',$dll)}
)
foreach($j in $jobs){
 $out=& $objdump @($j.Args) 2>&1
 if($LASTEXITCODE){throw "objdump failed $($j.Name)"}
 $path=Join-Path $root "test\$($j.Name).txt"
 $out | Set-Content -LiteralPath $path -Encoding utf8
 Write-Host $path
}
