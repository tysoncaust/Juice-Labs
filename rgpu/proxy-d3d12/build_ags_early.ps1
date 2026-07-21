$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$test = Join-Path $here 'test'
New-Item -ItemType Directory -Force -Path $test | Out-Null
$gxx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\g++.exe" -ErrorAction Stop | Select-Object -First 1).FullName
Write-Host "g++: $gxx"

# Build the current D3D12 capture DLL first, including the external-device arm export.
& $gxx -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ '-Wl,--kill-at' `
    "$here\src\rgpu_d3d12_proxy.cpp" "$here\src\rgpu_d3d12_slots.cpp" `
    -o "$test\rgpu_d3d12.dll" -ldxguid -ldxgi
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# Transparent early DXGI/AGS interception proxy.
& $gxx -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ '-Wl,--kill-at' `
    "$here\src\rgpu_dxgi_early.cpp" "$here\src\rgpu_d3d12_slots.cpp" `
    -o "$test\rgpu_dxgi_early.dll" -ldxguid
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# Compile harness unoptimised so its exported AGS test function has a conservative,
# easily relocated prologue. The production game hook still remains fail-safe.
& $gxx -std=c++17 -O0 '-Wl,--export-all-symbols' `
    "$here\test\rgpu_ags_early_harness.cpp" -o "$test\rgpu_ags_early_harness.exe" -ldxguid
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host 'built D3D12 capture DLL, early DXGI DLL, and AGS harness; running...'
Push-Location $test
try {
    & "$test\rgpu_ags_early_harness.exe"
    $rc = $LASTEXITCODE
} finally { Pop-Location }
exit $rc
