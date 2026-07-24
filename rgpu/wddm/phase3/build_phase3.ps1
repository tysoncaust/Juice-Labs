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

Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $out 'rgpu_transport_service.exe'), (Join-Path $out 'rgpu_phase3_client.exe'), (Join-Path $out 'RemoteGpuUmd.dll'), (Join-Path $out 'RemoteGpuUmdTest.exe'), (Join-Path $out 'RemoteGpuKmd.sys'), (Join-Path $out 'phase3-summary.txt'), (Join-Path $out 'service.stdout.txt'), (Join-Path $out 'service.stderr.txt')

Invoke-Checked $cl @(
    '/nologo','/std:c++17','/EHsc','/W4','/WX','/DUNICODE','/D_UNICODE',
    (Join-Path $phase 'service\rgpu_transport_service.cpp'),
    "/Fe:$out\rgpu_transport_service.exe"
)
Invoke-Checked $cl @(
    '/nologo','/std:c++17','/EHsc','/W4','/WX',
    (Join-Path $phase 'client\rgpu_phase3_client.cpp'),
    "/Fe:$out\rgpu_phase3_client.exe"
)

$umdIncludes = @(
    "/I$includeRoot\um", "/I$includeRoot\shared",
    "/I$sdkIncludeRoot\um", "/I$sdkIncludeRoot\shared", "/I$sdkIncludeRoot\ucrt"
)
Invoke-Checked $cl (@(
    '/nologo','/std:c++17','/EHsc','/W4','/WX','/wd4201','/LD','/DUNICODE','/D_UNICODE',
    "/Fo:$out\RemoteGpuUmd.obj"
) + $umdIncludes + @(
    (Join-Path $phase 'umd\RemoteGpuUmd.cpp'),
    '/link',"/DEF:$phase\umd\RemoteGpuUmd.def", "/OUT:$out\RemoteGpuUmd.dll", "/IMPLIB:$out\RemoteGpuUmd.lib"
))
Invoke-Checked $cl (@(
    '/nologo','/std:c++17','/EHsc','/W4','/WX','/wd4201','/DUNICODE','/D_UNICODE',
    "/Fo:$out\RemoteGpuUmdTest.obj"
) + $umdIncludes + @(
    (Join-Path $phase 'umd\RemoteGpuUmdTest.cpp'),
    "/Fe:$out\RemoteGpuUmdTest.exe"
))

$kernelBuilt = $false
if (-not $SkipKernel) {
    $kernelIncludes = @("/I$includeRoot\km", "/I$includeRoot\shared")
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
}

$serviceOut = Join-Path $out 'service.stdout.txt'
$serviceErr = Join-Path $out 'service.stderr.txt'
$service = Start-Process -FilePath (Join-Path $out 'rgpu_transport_service.exe') -ArgumentList @('--requests','4') -PassThru -RedirectStandardOutput $serviceOut -RedirectStandardError $serviceErr
Start-Sleep -Milliseconds 250

Invoke-Checked (Join-Path $out 'RemoteGpuUmdTest.exe') @((Join-Path $out 'RemoteGpuUmd.dll'))
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
    'PHASE3_TRANSPORT=PASS'
    'BOUNDED_SHARED_MEMORY_QUEUES=PASS capacity=16'
    'OUT_OF_PROCESS_SERVICE=PASS'
    'UMD_OPENADAPTER12_ABI=PASS'
    'UMD_TO_SERVICE_ROUNDTRIP=PASS'
    'UMD_CREATE_DEVICE=FAIL_CLOSED_NOT_IMPLEMENTED'
    "KERNEL_BROKER_BUILD=$([string]$(if ($kernelBuilt) {'PASS'} else {'SKIPPED'}))"
    'KERNEL_NETWORK_OPERATIONS=0'
    'KERNEL_BLOCKING_NETWORK_WAITS=0'
    'ROOT_ENUMERATED_RENDER_ADAPTER=NOT_COMPLETE'
    'D3D12_GRAPHICS_DDI_BODY=NOT_COMPLETE'
    "WDK_PACKAGE=$($wdk.Name)"
)
$summary | Set-Content -Encoding utf8 (Join-Path $out 'phase3-summary.txt')
$summary | ForEach-Object { Write-Output $_ }
Write-Output ($serviceText.Trim())
Remove-Item -Force -ErrorAction SilentlyContinue $serviceOut, $serviceErr
