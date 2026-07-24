param(
    [switch]$RunPhase2Wsl
)

$ErrorActionPreference = 'Stop'
$rgpu = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $rgpu
$phase2 = Join-Path $rgpu 'renderd\linux\phase2'
$phase3 = Join-Path $rgpu 'wddm\phase3'
$phase4 = Join-Path $rgpu 'wddm\phase4'

if ($RunPhase2Wsl) {
    $linuxScript = '/mnt/c/Users/email/Documents/Development/Juice-Labs/rgpu/renderd/linux/phase2/run_phase2.sh'
    & wsl.exe -d Ubuntu -- bash -lc "sed -i 's/\r$//' '$linuxScript'; bash '$linuxScript'"
    if ($LASTEXITCODE -ne 0) { throw "Phase 2 WSL validation failed: $LASTEXITCODE" }
}

$phase2EvidencePath = Join-Path $phase2 'out\phase2-evidence.json'
if (-not (Test-Path $phase2EvidencePath)) { throw 'Phase 2 evidence missing' }
$phase2Evidence = Get-Content $phase2EvidencePath -Raw | ConvertFrom-Json
foreach ($field in @('resource_creation','graphics_pipeline_created','fence_completed','compressed_frame_return')) {
    if ($phase2Evidence.$field -ne $true) { throw "Phase 2 field failed: $field" }
}
if ($phase2Evidence.command_buffers_submitted -ne 1 -or $phase2Evidence.draw_calls -ne 1) {
    throw 'Phase 2 command-buffer/draw acceptance failed'
}

& (Join-Path $phase3 'build_phase3.ps1')
& (Join-Path $phase4 'collect_security_status.ps1') | Out-Null
& (Join-Path $phase4 'build_phase4.ps1')

$phase3Summary = Get-Content (Join-Path $phase3 'out\phase3-summary.txt') -Raw
$phase4Summary = Get-Content (Join-Path $phase4 'out\phase4-summary.txt') -Raw
if ($phase3Summary -notmatch 'PHASE3_TRANSPORT=PASS' -or
    $phase3Summary -notmatch 'KERNEL_BROKER_BUILD=PASS') {
    throw 'Phase 3 summary acceptance missing'
}
if ($phase4Summary -notmatch 'PHASE4_INSTALLER_BUILD=PASS' -or
    $phase4Summary -notmatch 'PACKAGE_VALIDATION=PASS_FAIL_CLOSED') {
    throw 'Phase 4 summary acceptance missing'
}

$hardware = [bool]$phase2Evidence.hardware_vulkan
$overall = @(
    'PHASES_2_4_IMPLEMENTED_FOUNDATION_VALIDATION=PASS'
    "PHASE2_FUNCTIONAL_PIPELINE=PASS"
    "PHASE2_HARDWARE_VULKAN=$($hardware.ToString().ToLowerInvariant())"
    "PHASE2_COLAB_HARDWARE_ACCEPTANCE=$(if ($hardware) {'PASS'} else {'PENDING'})"
    'PHASE3_BOUNDED_TRANSPORT=PASS'
    'PHASE3_UMD_ABI=PASS_FAIL_CLOSED_DEVICE'
    'PHASE3_KERNEL_BROKER_BUILD=PASS'
    'PHASE3_ROOT_RENDER_ADAPTER=PENDING'
    'PHASE3_D3D12_GRAPHICS_DDI=PENDING'
    'PHASE4_CATALOG_AND_SETUP_TOOLING=PASS'
    'PHASE4_PRODUCTION_SIGNATURE=PENDING_EXTERNAL'
    'PHASE4_HLKS_AND_VENDOR_APPROVAL=PENDING_EXTERNAL'
    'FINAL_PRODUCT_READY=FALSE'
)
$overallPath = Join-Path $rgpu 'phases-2-4-validation-latest.txt'
$overall | Set-Content -Encoding utf8 $overallPath
$overall | ForEach-Object { Write-Output $_ }
Write-Output "EVIDENCE=$overallPath"
