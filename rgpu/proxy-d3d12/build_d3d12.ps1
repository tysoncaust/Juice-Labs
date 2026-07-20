# Build the rgpu d3d12 proxy (pass-through + command-stream tee) and its harness.
# Produces test\rgpu_d3d12.dll and test\rgpu_d3d12_harness.exe (runs the harness).
# Deploy: copy rgpu_d3d12.dll next to a D3D12 game's .exe as d3d12.dll (for TXR:
# ...\TokyoXtremeRacer\Binaries\Win64\d3d12.dll), launch with NO -d3d11, then read
# %TEMP%\rgpu_d3d12.log. Preserves the game's Agility SDK selection.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force -Path "$here\test" | Out-Null
$gxx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\g++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $gxx) { $gxx = "g++" }
Write-Host "g++: $gxx"
# proxy DLL: main TU + the CINTERFACE slots TU (authoritative vtable indices)
& $gxx -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ "-Wl,--kill-at" `
    "$here\src\rgpu_d3d12_proxy.cpp" "$here\src\rgpu_d3d12_slots.cpp" `
    -o "$here\test\rgpu_d3d12.dll" -ldxguid -ldxgi
Write-Host "built test\rgpu_d3d12.dll"
& $gxx -std=c++17 -O2 "$here\test\rgpu_d3d12_harness.cpp" -o "$here\test\rgpu_d3d12_harness.exe" -ldxguid
Write-Host "built test\rgpu_d3d12_harness.exe; running..."
Push-Location "$here\test"
try { & "$here\test\rgpu_d3d12_harness.exe"; $rc = $LASTEXITCODE } finally { Pop-Location }
exit $rc
