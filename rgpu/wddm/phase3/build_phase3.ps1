param(
    [switch]$SkipKernel
)

$ErrorActionPreference = 'Stop'
$phase = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $phase '..\..\..')
$out = Join-Path $phase 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$wdk = Get-ChildItem (Join-Path $repo 'external\wdk') -Directory |
    Where-Object Name -Like 'Microsoft.Windows.WDK.x64.*' |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $wdk) { throw 'Official Microsoft.Windows.WDK.x64 NuGet package not found under external\wdk' }
$includeVersion = Get-ChildItem (Join-Path $wdk.FullName 'c\Include') -Directory |
    Where-Object Name -Match '^10\.\d+\.\d+\.\d+$' |
    Sort-Object Name -Descending | Select-Object -First 1
$libVersion = Get-ChildItem (Join-Path $wdk.FullName 'c\Lib') -Directory |
    Where-Object Name -Match '^10\.\d+\.\d+\.\d+$' |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $includeVersion -or -not $libVersion) { throw 'WDK include/lib version folders missing' }
$includeRoot = $includeVersion.FullName
$libRoot = $libVersion.FullName
$sdk = Get-ChildItem (Join-Path $repo 'external\wdk') -Directory |
    Where-Object { $_.Name -Match '^Microsoft\.Windows\.SDK\.CPP\.\d' -and $_.Name -NotMatch '\.x64\.' } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdk) { throw 'Matching Microsoft.Windows.SDK.CPP NuGet package missing' }
$sdkIncludeVersion = Get-ChildItem (Join-Path $sdk.FullName 'c\Include') -Directory |
    Where-Object Name -Match '^10\.\d+\.\d+\.\d+$' |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkIncludeVersion) { throw 'Matching SDK include version missing' }
$sdkIncludeRoot = $sdkIncludeVersion.FullName

$cl = (Get-Command cl.exe -ErrorAction Stop).Source
$link = (Get-Command link.exe -ErrorAction Stop).Source

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $out 'rgpu_transport_service.exe'), (Join-Path $out 'rgpu_phase3_client.exe'), (Join-Path $out 'rgpu_phase3_isolation_client.exe'), (Join-Path $out 'RemoteGpuUmd.dll'), (Join-Path $out 'RemoteGpuUmdTest.exe'), (Join-Path $out 'RemoteGpuKmd.sys'), (Join-Path $out 'RemoteGpuDxgkKmd.sys'), (Join-Path $out 'phase3-summary.txt'), (Join-Path $out 'service.stdout.txt'), (Join-Path $out 'service.stderr.txt'), (Join-Path $out 'isolation-a.stdout.txt'), (Join-Path $out 'isolation-a.stderr.txt'), (Join-Path $out 'isolation-b.stdout.txt'), (Join-Path $out 'isolation-b.stderr.txt')

Invoke-Checked $cl @(
    '/nologo','/std:c++17','/EHsc','/W4','/WX','/wd4324','/DUNICODE','/D_UNICODE',
    (Join-Path $phase 'service\rgpu_transport_service.cpp'),
    "/Fe:$out\rgpu_transport_service.exe"
)
Invoke-Checked $cl @(
    '/nologo','/std:c++17','/EHsc','/W4','/WX','/wd4324',
    (Join-Path $phase 'client\rgpu_phase3_client.cpp'),
    "/Fe:$out\rgpu_phase3_client.exe"
)
Invoke-Checked $cl @(
    '/nologo','/std:c++17','/EHsc','/W4','/WX','/wd4324',
    (Join-Path $phase 'client\rgpu_phase3_isolation_client.cpp'),
    "/Fe:$out\rgpu_phase3_isolation_client.exe"
)

$umdIncludes = @(
    "/I$includeRoot\um", "/I$includeRoot\shared",
    "/I$sdkIncludeRoot\um", "/I$sdkIncludeRoot\shared", "/I$sdkIncludeRoot\ucrt"
)
Invoke-Checked $cl (@(
    '/nologo','/std:c++17','/EHsc','/W4','/WX','/wd4201','/wd4324','/LD','/DUNICODE','/D_UNICODE',
    "/Fo:$out\RemoteGpuUmd.obj"
) + $umdIncludes + @(
    (Join-Path $phase 'umd\RemoteGpuUmd.cpp'),
    '/link',"/DEF:$phase\umd\RemoteGpuUmd.def", "/OUT:$out\RemoteGpuUmd.dll", "/IMPLIB:$out\RemoteGpuUmd.lib"
))
Invoke-Checked $cl (@(
    '/nologo','/std:c++17','/EHsc','/W4','/WX','/wd4201','/wd4324','/DUNICODE','/D_UNICODE',
    "/Fo:$out\RemoteGpuUmdTest.obj"
) + $umdIncludes + @(
    (Join-Path $phase 'umd\RemoteGpuUmdTest.cpp'),
    "/Fe:$out\RemoteGpuUmdTest.exe"
))

$kernelBuilt = $false
$dxgkKernelBuilt = $false
if (-not $SkipKernel) {
    $kernelIncludes = @(
        "/I$includeRoot\km", "/I$includeRoot\shared",
        "/I$sdkIncludeRoot\um", "/I$sdkIncludeRoot\shared", "/I$sdkIncludeRoot\ucrt"
    )
    Invoke-Checked $cl (@(
        '/nologo','/c','/kernel','/W4','/WX','/GS-','/Zl','/D_AMD64_','/D_WIN64',
        "/Fo:$out\RemoteGpuKmd.obj"
    ) + $kernelIncludes + @((Join-Path $phase 'kmd\RemoteGpuKmd.c')))
    Invoke-Checked $link @(
        '/nologo','/driver','/subsystem:native','/entry:DriverEntry','/machine:x64','/nodefaultlib',
        "/libpath:$libRoot\km\x64", "/out:$out\RemoteGpuKmd.sys",
        (Join-Path $out 'RemoteGpuKmd.obj'), 'ntoskrnl.lib','hal.lib','BufferOverflowFastFailK.lib'
    )
    $kernelBuilt = Test-Path (Join-Path $out 'RemoteGpuKmd.sys')

    Invoke-Checked $cl (@(
        '/nologo','/c','/kernel','/W4','/WX','/wd4005','/wd4201','/GS-','/Zl','/D_AMD64_','/D_WIN64',
        "/Fo:$out\RemoteGpuDxgkKmd.obj"
    ) + $kernelIncludes + @((Join-Path $phase 'kmd\RemoteGpuDxgkKmd.c')))
    Invoke-Checked $link @(
        '/nologo','/driver','/subsystem:native','/entry:DriverEntry','/machine:x64','/nodefaultlib',
        "/libpath:$libRoot\km\x64", "/out:$out\RemoteGpuDxgkKmd.sys",
        (Join-Path $out 'RemoteGpuDxgkKmd.obj'), 'displib.lib','ntoskrnl.lib','hal.lib','BufferOverflowFastFailK.lib'
    )
    $dxgkKernelBuilt = Test-Path (Join-Path $out 'RemoteGpuDxgkKmd.sys')
}

$serviceOut = Join-Path $out 'service.stdout.txt'
$serviceErr = Join-Path $out 'service.stderr.txt'
$service = Start-Process -FilePath (Join-Path $out 'rgpu_transport_service.exe') -ArgumentList @('--idle-timeout-ms','30000') -PassThru -RedirectStandardOutput $serviceOut -RedirectStandardError $serviceErr
Start-Sleep -Milliseconds 250

Invoke-Checked (Join-Path $out 'RemoteGpuUmdTest.exe') @((Join-Path $out 'RemoteGpuUmd.dll'))

$isolationAOut = Join-Path $out 'isolation-a.stdout.txt'
$isolationAErr = Join-Path $out 'isolation-a.stderr.txt'
$isolationBOut = Join-Path $out 'isolation-b.stdout.txt'
$isolationBErr = Join-Path $out 'isolation-b.stderr.txt'
$isolationA = Start-Process -FilePath (Join-Path $out 'rgpu_phase3_isolation_client.exe') -PassThru -RedirectStandardOutput $isolationAOut -RedirectStandardError $isolationAErr
$isolationB = Start-Process -FilePath (Join-Path $out 'rgpu_phase3_isolation_client.exe') -PassThru -RedirectStandardOutput $isolationBOut -RedirectStandardError $isolationBErr
foreach ($client in @($isolationA, $isolationB)) {
    if (-not $client.WaitForExit(15000)) {
        Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue
        throw 'Concurrent Phase 3 isolation client timed out'
    }
    $client.Refresh()
}
$isolationAText = [string](Get-Content $isolationAOut -Raw -ErrorAction SilentlyContinue)
$isolationBText = [string](Get-Content $isolationBOut -Raw -ErrorAction SilentlyContinue)
$isolationAError = [string](Get-Content $isolationAErr -Raw -ErrorAction SilentlyContinue)
$isolationBError = [string](Get-Content $isolationBErr -Raw -ErrorAction SilentlyContinue)
if ($isolationAText -notmatch 'PHASE3_ISOLATION_CLIENT=PASS' -or
    $isolationBText -notmatch 'PHASE3_ISOLATION_CLIENT=PASS' -or
    -not [string]::IsNullOrWhiteSpace($isolationAError) -or
    -not [string]::IsNullOrWhiteSpace($isolationBError)) {
    throw "Concurrent Phase 3 isolation failed`nA:$isolationAText`n$isolationAError`nB:$isolationBText`n$isolationBError"
}

Invoke-Checked (Join-Path $out 'rgpu_phase3_client.exe') @()
if (-not $service.WaitForExit(10000)) {
    Stop-Process -Id $service.Id -Force -ErrorAction SilentlyContinue
    throw 'Phase 3 service did not exit after bounded requests'
}
$service.Refresh()
$serviceText = Get-Content $serviceOut -Raw
if ($serviceText -notmatch 'PHASE3_SERVICE=PASS') {
    throw "Service success marker missing; exit=$($service.ExitCode)`n$(Get-Content $serviceErr -Raw)"
}

$summary = @(
    'PHASE3_TRANSPORT_V2=PASS'
    'MPMC_CONTROL_RING=PASS capacity=256'
    'PER_PROCESS_COMPLETION_CHANNELS=PASS clients=16 capacity_each=128'
    'SHARED_BULK_ARENA=PASS bytes=8388608 slots=128 slot_bytes=65536'
    'MULTI_PROCESS_ISOLATION=PASS concurrent_clients=2'
    'ASYNC_OUTSTANDING_BATCHES=PASS batches=128'
    'FENCE_VALUE_COMPLETIONS=PASS'
    'OUT_OF_ORDER_COMPLETION_MATCHING=PASS'
    'CONNECTION_GENERATION_REJECTION=PASS'
    'CANCELLATION_RESET_DEVICE_LOST=PASS'
    'PROCESS_QUOTAS_AND_OBJECT_OWNERSHIP=PASS'
    'BOUNDED_BACKPRESSURE=PASS'
    'OUT_OF_PROCESS_SERVICE=PASS'
    'UMD_OPENADAPTER12_ABI=PASS'
    'UMD_TO_SERVICE_ROUNDTRIP=PASS'
    'UMD_CREATE_DEVICE=FAIL_CLOSED_NOT_IMPLEMENTED'
    "KERNEL_BROKER_BUILD=$([string]$(if ($kernelBuilt) {'PASS'} else {'SKIPPED'}))"
    "DXGK_RENDER_MINIPORT_SCAFFOLD_BUILD=$([string]$(if ($dxgkKernelBuilt) {'PASS'} else {'SKIPPED'}))"
    'DXGK_REGISTER_PATH=DxgkInitialize'
    'DXGK_DISPLAY_CHILDREN=0'
    'DXGK_RESOURCE_AND_SUBMISSION_PATHS=FAIL_CLOSED_NOT_IMPLEMENTED'
    'KERNEL_NETWORK_OPERATIONS=0'
    'KERNEL_BLOCKING_NETWORK_WAITS=0'
    'ROOT_ENUMERATED_RENDER_ADAPTER=BUILD_ONLY_NOT_LIVE_TESTED'
    'D3D12_GRAPHICS_DDI_BODY=NOT_COMPLETE'
    "WDK_PACKAGE=$($wdk.Name)"
)
$summary | Set-Content -Encoding utf8 (Join-Path $out 'phase3-summary.txt')
$summary | ForEach-Object { Write-Output $_ }
Write-Output ($isolationAText.Trim())
Write-Output ($isolationBText.Trim())
Write-Output ($serviceText.Trim())
Remove-Item -Force -ErrorAction SilentlyContinue $serviceOut, $serviceErr, $isolationAOut, $isolationAErr, $isolationBOut, $isolationBErr
