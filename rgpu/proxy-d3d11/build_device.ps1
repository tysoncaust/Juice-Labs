# Build + verify the full ID3D11Device wrapper and the d3d11.dll proxy
# (pass-through+tee). Produces test\rgpu_d3d11.dll (the injectable proxy) and
# test\rgpu_device_harness.exe (drives the proxy export -> real device -> wrapper
# -> real frame -> tee, on the local GPU). mingw-w64 (WinLibs) or MSVC both work.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = "$here\src"; $test = "$here\test"
$gxx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\g++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $gxx) { $gxx = "g++" }  # assume on PATH
Write-Host "g++: $gxx"

$dllArgs = @('-std=c++17','-O2','-shared','-static-libgcc','-static-libstdc++','-Wl,--kill-at',
  "$src\rgpu_d3d11_proxy.cpp","$src\rgpu_d3d11_device.cpp","$src\rgpu_serializer.cpp",
  '-o',"$test\rgpu_d3d11.dll",'-ldxguid','-ldxgi')
& $gxx @dllArgs
Write-Host "built test\rgpu_d3d11.dll (injectable proxy)"

$exeArgs = @('-std=c++17','-O2',"$test\rgpu_device_harness.cpp",'-o',"$test\rgpu_device_harness.exe",
  '-ld3d11','-ldxgi','-ldxguid')
& $gxx @exeArgs
Write-Host "built test\rgpu_device_harness.exe; running..."

Push-Location $test
try { & "$test\rgpu_device_harness.exe"; $rc = $LASTEXITCODE } finally { Pop-Location }
Write-Host ""
Write-Host "To boot-test a game: copy test\rgpu_d3d11.dll next to the game's .exe as"
Write-Host "d3d11.dll (Unity/D3D11 titles load it automatically; UE5 needs -d3d11 and"
Write-Host "only if it isn't a D3D12/SM6-only title), launch it, then read %TEMP%\rgpu_proxy.log."
exit $rc
