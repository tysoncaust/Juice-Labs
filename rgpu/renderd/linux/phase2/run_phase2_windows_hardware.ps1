param(
    [string]$SdkRoot = 'C:\VulkanSDK\1.4.350.0',
    [int]$TimestampIterations = 8192
)

$ErrorActionPreference = 'Stop'
$phase = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $phase '..\..\..\..')
$out = Join-Path $phase 'out-native'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

$cl = (Get-Command cl.exe -ErrorAction Stop).Source
$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
$ffprobe = Join-Path (Split-Path -Parent $ffmpeg) 'ffprobe.exe'
$nvidiaSmi = Join-Path $env:WINDIR 'System32\nvidia-smi.exe'
$glslang = Join-Path $SdkRoot 'Bin\glslangValidator.exe'
$vulkanInfo = Join-Path $SdkRoot 'Bin\vulkaninfoSDK.exe'
$vulkanInclude = Join-Path $SdkRoot 'Include'
$vulkanLib = Join-Path $SdkRoot 'Lib'
$vulkanLoader = Join-Path $env:WINDIR 'System32\vulkan-1.dll'

foreach ($required in @($ffprobe,$nvidiaSmi,$glslang,$vulkanInfo,$vulkanLoader)) {
    if (-not (Test-Path $required)) { throw "Required tool/file missing: $required" }
}

$icdManifest = Get-ChildItem (Join-Path $env:WINDIR 'System32\DriverStore\FileRepository') `
    -Filter 'nv-vk64.json' -Recurse -File -ErrorAction Stop |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $icdManifest) { throw 'NVIDIA Vulkan ICD manifest not found' }
$icdJson = Get-Content $icdManifest.FullName -Raw | ConvertFrom-Json
$icdLibrary = Join-Path $icdManifest.DirectoryName $icdJson.ICD.library_path
if (-not (Test-Path $icdLibrary)) { throw "NVIDIA ICD library missing: $icdLibrary" }

$env:VK_DRIVER_FILES = $icdManifest.FullName
$env:VK_LOADER_DRIVERS_SELECT = 'nv-vk64.json'
$env:VK_LOADER_LAYERS_DISABLE = '*'
$env:RGPU_EXPECT_DEVICE = 'NVIDIA'
$env:RGPU_EXPECT_VENDOR_ID = '0x10de'
$env:RGPU_TIMESTAMP_ITERATIONS = [string]$TimestampIterations

Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $out '*')

Invoke-Checked $glslang @('-V',(Join-Path $phase 'shaders\phase2.vert'),'-o',(Join-Path $out 'phase2.vert.spv'))
Invoke-Checked $glslang @('-V',(Join-Path $phase 'shaders\phase2.frag'),'-o',(Join-Path $out 'phase2.frag.spv'))

$commonCompile = @('/nologo','/std:c++17','/EHsc','/O2','/W4','/WX','/D_CRT_SECURE_NO_WARNINGS',"/I$vulkanInclude")
Invoke-Checked $cl ($commonCompile + @(
    (Join-Path $phase 'phase2_executor.cpp'),
    "/Fe:$out\phase2_executor.exe",
    '/link',"/LIBPATH:$vulkanLib",'vulkan-1.lib'
))
Invoke-Checked $cl ($commonCompile + @(
    (Join-Path $phase 'phase2_hardware_probe.cpp'),
    "/Fe:$out\phase2_hardware_probe.exe",
    '/link',"/LIBPATH:$vulkanLib",'vulkan-1.lib'
))

$vulkanInfoOut = Join-Path $out 'vulkaninfo-summary.txt'
$vulkanInfoErr = Join-Path $out 'vulkaninfo-summary.err.txt'
$vulkanInfoProcess = Start-Process -FilePath $vulkanInfo -ArgumentList @('--summary') `
    -RedirectStandardOutput $vulkanInfoOut -RedirectStandardError $vulkanInfoErr -PassThru -Wait
if ($vulkanInfoProcess.ExitCode -ne 0) { throw "vulkaninfo failed: $($vulkanInfoProcess.ExitCode)" }

$dmonOut = Join-Path $out 'nvidia-dmon.txt'
$dmonErr = Join-Path $out 'nvidia-dmon.err.txt'
$dmon = Start-Process -FilePath $nvidiaSmi -ArgumentList @('dmon','-s','u','-d','1','-c','8') `
    -RedirectStandardOutput $dmonOut -RedirectStandardError $dmonErr -PassThru
Start-Sleep -Milliseconds 300

Invoke-Checked (Join-Path $out 'phase2_hardware_probe.exe') @((Join-Path $out 'hardware-probe.json'))
Invoke-Checked (Join-Path $out 'phase2_executor.exe') @(
    (Join-Path $out 'phase2.vert.spv'),
    (Join-Path $out 'phase2.frag.spv'),
    (Join-Path $out 'frame.rgba'),
    (Join-Path $out 'render-evidence.json')
)

if (-not $dmon.WaitForExit(12000)) {
    Stop-Process -Id $dmon.Id -Force -ErrorAction SilentlyContinue
    throw 'nvidia-smi dmon did not exit'
}

Invoke-Checked $ffmpeg @(
    '-hide_banner','-loglevel','error','-y',
    '-f','rawvideo','-pixel_format','rgba','-video_size','256x256','-framerate','60',
    '-i',(Join-Path $out 'frame.rgba'),'-frames:v','1',
    '-c:v','h264_nvenc','-preset','p4','-tune','ll','-f','h264',
    (Join-Path $out 'frame.h264')
)
$codec = (& $ffprobe -v error -select_streams v:0 -show_entries stream=codec_name `
    -of default=nw=1:nk=1 (Join-Path $out 'frame.h264')).Trim()
if ($LASTEXITCODE -ne 0 -or $codec -ne 'h264') { throw "H.264 verification failed: $codec" }

$probe = Get-Content (Join-Path $out 'hardware-probe.json') -Raw | ConvertFrom-Json
$render = Get-Content (Join-Path $out 'render-evidence.json') -Raw | ConvertFrom-Json
if (-not $probe.hardware_vulkan -or $probe.vendor_id -ne 0x10de) { throw 'Hardware NVIDIA Vulkan gate failed' }
if ($probe.device_name -match '(?i)llvmpipe|lavapipe|software|swiftshader|cpu') { throw 'Software Vulkan device selected' }
if (-not $render.resource_creation -or -not $render.graphics_pipeline_created -or `
    -not $render.fence_completed -or -not $render.frame_non_uniform) {
    throw 'Renderer acceptance failed'
}

$dmonLines = Get-Content $dmonOut -ErrorAction Stop
$smSamples = @()
foreach ($line in $dmonLines) {
    if ($line -match '^\s*\d+\s+(\d+)\s+') { $smSamples += [int]$Matches[1] }
}
$maxSm = if ($smSamples.Count) { ($smSamples | Measure-Object -Maximum).Maximum } else { 0 }
if ($maxSm -le 0) { throw 'Independent NVIDIA utilization sampling did not observe GPU activity' }

$referenceEvidencePath = Join-Path $phase 'out\phase2-evidence.json'
$referenceCrc = $null
if (Test-Path $referenceEvidencePath) {
    $referenceCrc = (Get-Content $referenceEvidencePath -Raw | ConvertFrom-Json).frame_crc32
}
$frameMatch = ($null -ne $referenceCrc -and $render.frame_crc32 -eq $referenceCrc)
if (-not $frameMatch) { throw "Native frame CRC does not match reference: native=$($render.frame_crc32) reference=$referenceCrc" }

$os = Get-CimInstance Win32_OperatingSystem
$nvidia = (& $nvidiaSmi --query-gpu=name,pci.bus_id,driver_version,uuid `
    --format=csv,noheader,nounits | Select-Object -First 1).Trim()
$h264Path = Join-Path $out 'frame.h264'
$rgbaPath = Join-Path $out 'frame.rgba'
$manifest = [ordered]@{
    acceptance = 'PHASE2_NATIVE_HARDWARE_VULKAN_PASS'
    collected_utc = [DateTime]::UtcNow.ToString('o')
    git_commit = (& git -C $repo rev-parse HEAD).Trim()
    environment = [ordered]@{
        operating_system = $os.Caption
        os_version = $os.Version
        os_build = $os.BuildNumber
        architecture = $env:PROCESSOR_ARCHITECTURE
        vulkan_sdk = Split-Path -Leaf $SdkRoot
        vulkan_loader_path = $vulkanLoader
        vulkan_loader_sha256 = (Get-FileHash $vulkanLoader -Algorithm SHA256).Hash.ToLowerInvariant()
        vulkan_loader_file_version = (Get-Item $vulkanLoader).VersionInfo.FileVersion
        selected_icd_manifest = $icdManifest.FullName
        selected_icd_manifest_sha256 = (Get-FileHash $icdManifest.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        selected_icd_library = (Resolve-Path $icdLibrary).Path
        selected_icd_library_sha256 = (Get-FileHash $icdLibrary -Algorithm SHA256).Hash.ToLowerInvariant()
        driver_override = $env:VK_DRIVER_FILES
        driver_filter = $env:VK_LOADER_DRIVERS_SELECT
        nvidia_smi_identity = $nvidia
    }
    vulkan = $probe
    renderer = $render
    independent_gpu_activity = [ordered]@{
        observer = 'nvidia-smi dmon'
        max_sm_utilization_percent = $maxSm
        samples = $smSamples
        raw_log_sha256 = (Get-FileHash $dmonOut -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    output = [ordered]@{
        raw_rgba_bytes = (Get-Item $rgbaPath).Length
        raw_rgba_sha256 = (Get-FileHash $rgbaPath -Algorithm SHA256).Hash.ToLowerInvariant()
        frame_crc32 = $render.frame_crc32
        reference_crc32 = $referenceCrc
        reference_frame_match = $frameMatch
        compressed_codec = $codec
        compressed_encoder = 'h264_nvenc'
        compressed_bytes = (Get-Item $h264Path).Length
        compressed_sha256 = (Get-FileHash $h264Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifestPath = Join-Path $out 'phase2-native-hardware-evidence.json'
$manifest | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 $manifestPath

$summary = @(
    'PHASE2_NATIVE_HARDWARE_VULKAN=PASS'
    "DEVICE=$($probe.device_name)"
    "DRIVER=$($probe.driver_name) $($probe.driver_info)"
    "ICD_MANIFEST=$($icdManifest.FullName)"
    "GPU_TIMESTAMP_TICKS=$($probe.timestamp_delta_ticks)"
    "GPU_TIMESTAMP_NS=$($probe.timestamp_elapsed_ns)"
    "INDEPENDENT_MAX_SM_UTILIZATION_PERCENT=$maxSm"
    "FRAME_CRC32=$($render.frame_crc32)"
    'REFERENCE_FRAME_MATCH=PASS'
    'NVENC_H264_RETURN=PASS'
    "EVIDENCE=$manifestPath"
)
$summary | Set-Content -Encoding utf8 (Join-Path $out 'phase2-native-summary.txt')
$summary | ForEach-Object { Write-Output $_ }
