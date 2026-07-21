$ErrorActionPreference='Stop'
$root='C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$objdump=(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\objdump.exe" -ErrorAction Stop | Select-Object -First 1).FullName
$dll='C:\Windows\System32\d3d12.dll'
$chunks=@(
  @{Name='guid_172e8'; Start='0x1800172e8'; Stop='0x180017308'},
  @{Name='guid_179e0'; Start='0x1800179e0'; Stop='0x180017a00'},
  @{Name='data_1ece0'; Start='0x18001ece0'; Stop='0x18001ed80'},
  @{Name='code_9aa0_9c20'; Start='0x180009aa0'; Stop='0x180009c20'}
)
foreach($c in $chunks){
  $args=if($c.Name -like 'code*'){@('-d',"--start-address=$($c.Start)","--stop-address=$($c.Stop)",$dll)}else{@('-s',"--start-address=$($c.Start)","--stop-address=$($c.Stop)",$dll)}
  $out=& $objdump @args 2>&1
  if($LASTEXITCODE){throw "objdump failed for $($c.Name)"}
  $path=Join-Path $root "test\$($c.Name).txt"
  $out | Set-Content -LiteralPath $path -Encoding utf8
  Write-Host $path
}
