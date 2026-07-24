param(
    [switch]$RunPhase2Wsl,
    [switch]$RunPhase2Native
)

$ErrorActionPreference = 'Stop'
$rgpu = Split-Path -Parent $MyInvocation.MyCommand.Path
$phase2 = Join-Path $rgpu 'renderd\linux\phase2'
$phase3 = Join-Path $rgpu 'wddm\phase3'
$phase4 = Join-Path $rgpu 'wddm\phase4'

if ($RunPhase2Wsl) {
    $linuxScript = '/mnt/c/Users/email/Documents/Development/Juice-Labs/rgpu/renderd/linux/phase2/run_phase2.sh'
    & wsl.exe -d Ubuntu -- bash -lc "sed -i 's/\r$//' '$linuxScript'; bash '$linuxScript'"
    if ($LASTEXITCODE -ne 0) { throw "Phase 2 WSL validation failed: $LASTEXITCODE" }
}
if ($RunPhase2Native) {
    & (Join-Path $phase2 'run_phase2_windows_hardware.ps1')
}

$functionalPath = Join-Path $phase2 'out\phase2-evidence.json'
$nativePath = Join-Path $phase2 'out-native\phase2-native-hardware-evidence.json'
if (-not (Test-Path $functionalPath)) { throw 'Phase 2 functional evidence missing' }
if (-not (Test-Path $nativePath)) { throw 'Phase 2 native hardware evidence missing' }
$functional = Get-Content $functionalPath -Raw | ConvertFrom-Json
$native = Get-Content $nativePath -Raw | ConvertFrom-Json
foreach ($field in @('resource_creation','graphics_pipeline_created','fence_completed','compressed_frame_return')) {
    if ($functional.$field -ne $true) { throw "Phase 2 functional field failed: $field" }
}
if ($functional.command_buffers_submitted -ne 1 -or $functional.draw_calls -ne 1) {
    throw 'Phase 2 command-buffer/draw acceptance failed'
}
if ($native.acceptance -ne 'PHASE2_NATIVE_HARDWARE_VULKAN_PASS' -or
    $native.vulkan.hardware_vulkan -ne $true -or
    $native.vulkan.vendor_id -ne 0x10de -or
    $native.vulkan.timestamp_delta_ticks -le 0 -or
    $native.independent_gpu_activity.max_sm_utilization_percent -le 0 -or
    $native.output.reference_frame_match -ne $true -or
    $native.output.compressed_encoder -ne 'h264_nvenc') {
    throw 'Phase 2 native hardware Vulkan acceptance failed'
}
if ($native.vulkan.device_name -match '(?i)llvmpipe|lavapipe|software|swiftshader|cpu') {
    throw 'Phase 2 selected a software Vulkan device'
}

& (Join-Path $phase3 'build_phase3.ps1')
& (Join-Path $phase4 'collect_security_status.ps1') | Out-Null
& (Join-Path $phase4 'build_phase4.ps1')

$phase3Summary = Get-Content (Join-Path $phase3 'out\phase3-summary.txt') -Raw
$phase4Summary = Get-Content (Join-Path $phase4 'out\phase4-summary.txt') -Raw
foreach ($marker in @(
    'PHASE3_TRANSPORT_V2=PASS',
    'MULTI_PROCESS_ISOLATION=PASS',
    'ASYNC_OUTSTANDING_BATCHES=PASS',
    'DXGK_RENDER_MINIPORT_SCAFFOLD_BUILD=PASS',
    'KERNEL_BROKER_BUILD=PASS'
)) {
    if ($phase3Summary -notmatch [regex]::Escape($marker)) {
        throw "Phase 3 summary marker missing: $marker"
    }
}
if ($phase4Summary -notmatch 'PHASE4_INSTALLER_BUILD=PASS' -or
    $phase4Summary -notmatch 'PACKAGE_VALIDATION=PASS_FAIL_CLOSED') {
    throw 'Phase 4 summary acceptance missing'
}

$overall = @(
    'PHASES_2_4_GATED_VALIDATION=PASS'
    'GATE1_NATIVE_HARDWARE_VULKAN=PASS'
    "GATE1_DEVICE=$($native.vulkan.device_name)"
    "GATE1_DRIVER=$($native.vulkan.driver_name) $($native.vulkan.driver_info)"
    "GATE1_GPU_TIMESTAMP_TICKS=$($native.vulkan.timestamp_delta_ticks)"
    "GATE1_INDEPENDENT_MAX_SM_UTILIZATION_PERCENT=$($native.independent_gpu_activity.max_sm_utilization_percent)"
    'GATE1_REFERENCE_FRAME_MATCH=PASS'
    'GATE1_NVENC_RETURN=PASS'
    'GATE1_COLAB_ADDITIONAL_RUN=PENDING_NOTEBOOK_EXECUTION'
    'GATE2_TRANSPORT_V2=PASS'
    'GATE2_MULTI_PROCESS_ISOLATION=PASS'
    'GATE2_DXGK_MINIPORT_SCAFFOLD_BUILD=PASS'
    'GATE2_ROOT_ADAPTER_LIVE_TEST=PENDING_ISOLATED_WINDOWS_TARGET'
    'GATE2_D3D12CREATEDEVICE=PENDING_DDI_IMPLEMENTATION'
    'GATE3_MINIMUM_GRAPHICS_IMPLEMENTATION=PENDING'
    'GATE4_ROBUSTNESS_SECURITY=PENDING_ISOLATED_DRIVER_TESTING'
    'GATE5_CATALOG_SETUP_TOOLING=PASS'
    'GATE5_PRODUCTION_SIGNATURE_HLK=PENDING_EXTERNAL'
    'GATE6_VENDOR_VALIDATION=PENDING_EXTERNAL'
    'FINAL_PRODUCT_READY=FALSE'
)
$overallPath = Join-Path $rgpu 'phases-2-4-validation-latest.txt'
$overall | Set-Content -Encoding utf8 $overallPath
$overall | ForEach-Object { Write-Output $_ }
Write-Output "EVIDENCE=$overallPath"
