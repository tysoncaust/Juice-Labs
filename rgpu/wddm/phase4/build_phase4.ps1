$ErrorActionPreference = 'Stop'
$phase = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $phase '..\..\..')
$phase3out = Resolve-Path (Join-Path $phase '..\phase3\out')
$package = Join-Path $phase 'package'
$out = Join-Path $phase 'out'
New-Item -ItemType Directory -Force -Path $package, $out | Out-Null

$requiredPhase3 = @('RemoteGpuDxgkKmd.sys','RemoteGpuUmd.dll','rgpu_transport_service.exe')
foreach ($name in $requiredPhase3) {
    $source = Join-Path $phase3out $name
    if (-not (Test-Path $source)) { throw "Phase 3 output missing: $source" }
}
Copy-Item -Force (Join-Path $phase3out 'RemoteGpuDxgkKmd.sys') (Join-Path $package 'RemoteGpuKmd.sys')
Copy-Item -Force (Join-Path $phase3out 'RemoteGpuUmd.dll') (Join-Path $package 'RemoteGpuUmd.dll')
Copy-Item -Force (Join-Path $phase3out 'rgpu_transport_service.exe') (Join-Path $package 'RgpuTransportService.exe')
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $package 'production-ready.marker'), (Join-Path $package 'RemoteGpuRoot.cat')

$wdk = Get-ChildItem (Join-Path $repo 'external\wdk') -Directory |
    Where-Object Name -Like 'Microsoft.Windows.WDK.x64.*' |
    Sort-Object Name -Descending | Select-Object -First 1
$inf2cat = Get-ChildItem (Join-Path $wdk.FullName 'c\bin') -Filter Inf2Cat.exe -Recurse |
    Sort-Object FullName | Select-Object -First 1
if (-not $inf2cat) { throw 'Inf2Cat.exe not found in official WDK NuGet package' }

& $inf2cat.FullName "/driver:$package" '/os:10_X64' '/verbose'
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed: $LASTEXITCODE" }
if (-not (Test-Path (Join-Path $package 'RemoteGpuRoot.cat'))) { throw 'Catalog not produced' }

$cl = (Get-Command cl.exe -ErrorAction Stop).Source
& $cl /nologo /std:c++17 /EHsc /W4 /WX /DUNICODE /D_UNICODE `
    (Join-Path $phase 'RemoteGpuSetup.cpp') "/Fe:$out\RemoteGpuSetup.exe" `
    /link setupapi.lib wintrust.lib crypt32.lib advapi32.lib
if ($LASTEXITCODE -ne 0) { throw "RemoteGpuSetup build failed: $LASTEXITCODE" }
Copy-Item -Force (Join-Path $out 'RemoteGpuSetup.exe') (Join-Path $package 'RemoteGpuSetup.exe')

& (Join-Path $out 'RemoteGpuSetup.exe') --validate $package
$validateExit = $LASTEXITCODE
if ($validateExit -ne 20) { throw "Unsigned/incomplete package did not fail closed; exit=$validateExit" }
& (Join-Path $out 'RemoteGpuSetup.exe') --uninstall
$uninstallExit = $LASTEXITCODE
if ($uninstallExit -ne 31) { throw "Non-elevated uninstall did not fail closed; exit=$uninstallExit" }

$summary = @(
    'PHASE4_INSTALLER_BUILD=PASS'
    'PHASE4_UNINSTALLER_BUILD=PASS'
    'CATALOG_GENERATION=PASS_UNSIGNED'
    'PACKAGED_KERNEL_BINARY=DXGK_RENDER_MINIPORT_SCAFFOLD'
    'PRODUCTION_SIGNATURE=BLOCKED_EXTERNAL_CERTIFICATE_AND_SUBMISSION'
    'PACKAGE_VALIDATION=PASS_FAIL_CLOSED_UNSIGNED_OR_INCOMPLETE'
    'NON_ELEVATED_UNINSTALL=PASS_FAIL_CLOSED'
    'TEST_SIGNING_REQUIREMENT=NONE'
    'HVCI_LIVE_TEST=NOT_READY_HVCI_NOT_RUNNING'
    'SECURE_BOOT_LIVE_TEST=NOT_VERIFIED_REQUIRES_ELEVATION'
    'HLK=NOT_RUN_EXTERNAL_TEST_LAB_REQUIRED'
    'GAME_VENDOR_TESTING=NOT_RUN_EXTERNAL_VENDOR_REQUIRED'
    'ANTI_CHEAT_VENDOR_TESTING=NOT_RUN_EXTERNAL_VENDOR_REQUIRED'
    'PRODUCTION_READY=FALSE'
)
$summary | Set-Content -Encoding utf8 (Join-Path $out 'phase4-summary.txt')
$summary | ForEach-Object { Write-Output $_ }
