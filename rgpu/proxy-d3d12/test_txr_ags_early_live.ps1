$ErrorActionPreference = 'Stop'

$Repo = 'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$D3DProxy = Join-Path $Repo 'test\rgpu_d3d12.dll'
$DXGIProxy = Join-Path $Repo 'test\rgpu_dxgi_early.dll'
$GameDir = 'C:\Program Files\Tokyo Xtreme Racer\TokyoXtremeRacer\Binaries\Win64'
$GameExe = Join-Path $GameDir 'TokyoXtremeRacer-Win64-Shipping.exe'
$D3DDeployed = Join-Path $GameDir 'd3d12.dll'
$DXGIDeployed = Join-Path $GameDir 'dxgi.dll'
$D3DTempLog = Join-Path $env:TEMP 'rgpu_d3d12.log'
$DXGITempLog = Join-Path $env:TEMP 'rgpu_dxgi_early.log'
$D3DSavedLog = Join-Path $Repo 'test\txr_ags_rgpu_d3d12.log'
$DXGISavedLog = Join-Path $Repo 'test\txr_ags_rgpu_dxgi_early.log'
$ReportPath = Join-Path $Repo 'test\txr_ags_live_test.json'
$D3DBackup = $null
$DXGIBackup = $null

function Get-TxrProcesses {
    @(Get-CimInstance Win32_Process -Filter "Name='TokyoXtremeRacer-Win64-Shipping.exe'" -ErrorAction SilentlyContinue)
}
function Stop-TxrProcesses {
    foreach ($p in @(Get-TxrProcesses)) {
        try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop } catch {}
    }
    Start-Sleep -Seconds 2
}
function Backup-IfPresent([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        $backup = "$Path.rgpu-backup-$([DateTime]::UtcNow.ToString('yyyyMMddHHmmss'))"
        Move-Item -LiteralPath $Path -Destination $backup -Force
        return $backup
    }
    return $null
}
function Restore-TestFile([string]$Path, [string]$Backup) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
    if ($Backup -and (Test-Path -LiteralPath $Backup)) {
        Move-Item -LiteralPath $Backup -Destination $Path -Force
    }
}

foreach ($required in @($D3DProxy, $DXGIProxy, $GameExe)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required file not found: $required" }
}

$observations = [System.Collections.Generic.List[object]]::new()
$report = [ordered]@{
    started_utc = [DateTime]::UtcNow.ToString('o')
    duration_seconds = 90
    game_exe = $GameExe
    d3d12_proxy_sha256 = (Get-FileHash -LiteralPath $D3DProxy -Algorithm SHA256).Hash
    dxgi_proxy_sha256 = (Get-FileHash -LiteralPath $DXGIProxy -Algorithm SHA256).Hash
    launch_pid = $null
    observations = $observations
    unique_pids = @()
    stable = $false
    dxgi_markers = [ordered]@{}
    d3d12_markers = [ordered]@{}
    ags_hook_installed = $false
    ags_call_intercepted = $false
    ags_device_armed = $false
    core_hook_installed = $false
    core_getinterface_intercepted = $false
    core_device_armed = $false
    command_stream_live = $false
    cleanup_ok = $false
    error = $null
}

try {
    Stop-TxrProcesses
    $D3DBackup = Backup-IfPresent $D3DDeployed
    $DXGIBackup = Backup-IfPresent $DXGIDeployed
    foreach ($log in @($D3DTempLog, $DXGITempLog, $D3DSavedLog, $DXGISavedLog)) {
        if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
    }

    Copy-Item -LiteralPath $D3DProxy -Destination $D3DDeployed -Force
    Copy-Item -LiteralPath $DXGIProxy -Destination $DXGIDeployed -Force

    $started = Start-Process -FilePath $GameExe -WorkingDirectory $GameDir `
        -ArgumentList @('-windowed', '-ResX=1280', '-ResY=720') -PassThru
    $report.launch_pid = $started.Id

    $seenPids = [System.Collections.Generic.HashSet[int]]::new()
    for ($i = 0; $i -lt 18; $i++) {
        Start-Sleep -Seconds 5
        $ps = @(Get-TxrProcesses)
        foreach ($p in $ps) { [void]$seenPids.Add([int]$p.ProcessId) }
        $observations.Add([pscustomobject][ordered]@{
            elapsed_seconds = (($i + 1) * 5)
            process_count = $ps.Count
            pids = @($ps | ForEach-Object { [int]$_.ProcessId })
            working_set_mb = [math]::Round((($ps | Measure-Object WorkingSetSize -Sum).Sum / 1MB), 1)
            d3d12_log_bytes = if (Test-Path -LiteralPath $D3DTempLog) { (Get-Item -LiteralPath $D3DTempLog).Length } else { 0 }
            dxgi_log_bytes = if (Test-Path -LiteralPath $DXGITempLog) { (Get-Item -LiteralPath $DXGITempLog).Length } else { 0 }
        })
    }
    $report.unique_pids = @($seenPids)

    $d3dLog = if (Test-Path -LiteralPath $D3DTempLog) { Get-Content -LiteralPath $D3DTempLog -Raw } else { '' }
    $dxgiLog = if (Test-Path -LiteralPath $DXGITempLog) { Get-Content -LiteralPath $DXGITempLog -Raw } else { '' }
    if ($d3dLog) { [IO.File]::WriteAllText($D3DSavedLog, $d3dLog, [Text.UTF8Encoding]::new($false)) }
    if ($dxgiLog) { [IO.File]::WriteAllText($DXGISavedLog, $dxgiLog, [Text.UTF8Encoding]::new($false)) }

    $dxgiNeedles = [ordered]@{
        early_active = 'early DXGI proxy active'
        hook_yes = 'hook=1'
        ags_call = 'AGS DX12 CreateDevice rc='
        handed = 'AGS returned device handed to rgpu D3D12 capture'
        arm_missing = 'arm_external_device export was not found'
        core_monitor = 'D3D12Core monitor module='
        core_hook_yes = 'D3D12Core monitor module='
        core_getinterface = 'D3D12Core!D3D12GetInterface clsid='
        core_sdk_factory = 'D3D12Core SDKConfiguration1::CreateDeviceFactory'
        core_factory_device = 'D3D12Core DeviceFactory::CreateDevice'
        core_handed = 'D3D12Core_DeviceFactory returned device handed to rgpu D3D12 capture'
    }
    foreach ($key in $dxgiNeedles.Keys) {
        $report.dxgi_markers[$key] = ([regex]::Matches($dxgiLog, [regex]::Escape($dxgiNeedles[$key]))).Count
    }
    $d3dNeedles = [ordered]@{
        attach = 'ATTACH host='
        external_ags_arm = 'external device arm source=AMD_AGS'
        armed_ags = 'armed-hybrid AMD_AGS device'
        external_core_arm = 'external device arm source=D3D12Core_DeviceFactory'
        armed_core = 'armed-hybrid D3D12Core_DeviceFactory device'
        check_feature_support = 'game USES our hooked device'
        queue_base = 'game called CreateCommandQueue (base)'
        queue1 = 'game called CreateCommandQueue1 (Agility)'
        list_base = 'game called CreateCommandList (base)'
        list1 = 'game called CreateCommandList1 (Agility)'
        submit = 'submit#'
        draw = 'FIRST Draw'
        dispatch = 'FIRST Dispatch'
        barrier = 'FIRST Barrier (enhanced/GCL7)'
        indirect = 'FIRST ExecuteIndirect'
        close = 'FIRST Close'
    }
    foreach ($key in $d3dNeedles.Keys) {
        $report.d3d12_markers[$key] = ([regex]::Matches($d3dLog, [regex]::Escape($d3dNeedles[$key]))).Count
    }

    $report.ags_hook_installed = $report.dxgi_markers.hook_yes -gt 0
    $report.ags_call_intercepted = $report.dxgi_markers.ags_call -gt 0
    $report.ags_device_armed = ($report.dxgi_markers.handed -gt 0 -and $report.d3d12_markers.external_ags_arm -gt 0)
    $report.core_hook_installed = ($dxgiLog -match 'D3D12Core monitor module=.*hook=1')
    $report.core_getinterface_intercepted = $report.dxgi_markers.core_getinterface -gt 0
    $report.core_device_armed = ($report.dxgi_markers.core_handed -gt 0 -and $report.d3d12_markers.external_core_arm -gt 0)
    $report.command_stream_live = (
        $report.d3d12_markers.submit -gt 0 -or
        $report.d3d12_markers.draw -gt 0 -or
        $report.d3d12_markers.dispatch -gt 0 -or
        $report.d3d12_markers.barrier -gt 0 -or
        $report.d3d12_markers.indirect -gt 0 -or
        $report.d3d12_markers.close -gt 0
    )
    $aliveSamples = @($observations | Where-Object { $_.process_count -gt 0 }).Count
    $report.stable = ($aliveSamples -ge 12 -and $seenPids.Count -le 1 -and $report.d3d12_markers.attach -le 1)
}
catch {
    $report.error = $_.Exception.ToString()
}
finally {
    Stop-TxrProcesses
    try {
        Restore-TestFile $DXGIDeployed $DXGIBackup
        Restore-TestFile $D3DDeployed $D3DBackup
        $dxgiClean = if ($DXGIBackup) { Test-Path -LiteralPath $DXGIDeployed } else { -not (Test-Path -LiteralPath $DXGIDeployed) }
        $d3dClean = if ($D3DBackup) { Test-Path -LiteralPath $D3DDeployed } else { -not (Test-Path -LiteralPath $D3DDeployed) }
        $report.cleanup_ok = $dxgiClean -and $d3dClean
    } catch {
        if (-not $report.error) { $report.error = $_.Exception.ToString() }
        $report.cleanup_ok = $false
    }
    $report.completed_utc = [DateTime]::UtcNow.ToString('o')
    $json = $report | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($ReportPath, $json, [Text.UTF8Encoding]::new($false))
    Write-Output $json
}

if ($report.error -or -not $report.cleanup_ok) { exit 1 }
exit 0
