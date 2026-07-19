# Build + run the rgpu Phase-1 interception proof (synthetic DXGI adapter +
# fail-closed policy). Needs a C++ toolchain with the DXGI/D3D11 SDK headers —
# mingw-w64 (winget BrechtSanders.WinLibs.POSIX.UCRT) or MSVC both work.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$gxx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\g++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $gxx) { $gxx = "g++" }  # assume on PATH (or MSVC via cl.exe separately)
Write-Host "g++: $gxx"
& $gxx -std=c++17 -O2 "$here\test\rgpu_enum_test.cpp" "$here\src\rgpu_synthetic.cpp" `
    -o "$here\test\rgpu_enum_test.exe" -ldxgi -ldxguid
Write-Host "built rgpu_enum_test.exe; running..."
& "$here\test\rgpu_enum_test.exe"
exit $LASTEXITCODE
