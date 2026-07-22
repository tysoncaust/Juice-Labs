param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot,

    [int]$DurationSeconds = 100,

    [string]$EvidenceDirectory = ""
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Split-Path -Parent $here
$proxy = Join-Path $project "test\rgpu_d3d12.dll"
$bin = Join-Path $GameRoot "TokyoXtremeRacer\Binaries\Win64"
$game = Join-Path $bin "TokyoXtremeRacer-Win64-Shipping.exe"
$deployed = Join-Path $bin "d3d12.dll"

if (-not (Test-Path -LiteralPath $proxy)) { throw "Built proxy not found: $proxy" }
if (-not (Test-Path -LiteralPath $game)) { throw "TXR executable not found: $game" }
if ($DurationSeconds -lt 15 -or $DurationSeconds -gt 600) { throw "DurationSeconds must be 15..600" }

if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $project "test\evidence"
}
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$evidenceLog = Join-Path $EvidenceDirectory "txr-rgpu-$stamp.log"
$summaryPath = Join-Path $EvidenceDirectory "txr-rgpu-$stamp-summary.txt"
$backup = $null
$started = $null

try {
    if (Test-Path -LiteralPath $deployed) {
        $proxyHash = (Get-FileHash -LiteralPath $proxy -Algorithm SHA256).Hash
        $deployedHash = (Get-FileHash -LiteralPath $deployed -Algorithm SHA256).Hash
        if ($proxyHash -eq $deployedHash) {
            # Recover cleanly from an interrupted earlier probe that left our own DLL behind.
            Remove-Item -LiteralPath $deployed -Force
        } else {
            $backup = Join-Path $EvidenceDirectory "preexisting-d3d12-$stamp.dll"
            Copy-Item -LiteralPath $deployed -Destination $backup -Force
        }
    }
    if (Test-Path -LiteralPath $evidenceLog) { Remove-Item -LiteralPath $evidenceLog -Force }
    Copy-Item -LiteralPath $proxy -Destination $deployed -Force

    # Give this process a unique log path so concurrent harnesses cannot contaminate evidence.
    $env:RGPU_LOG_PATH = $evidenceLog

    # Expire before process teardown so the final per-entry counts reach the evidence log.
    $probeSeconds = [Math]::Max(10, $DurationSeconds - 10)
    $env:RGPU_CET_PROBE_MS = [string]([Math]::Min(600000, $probeSeconds * 1000))
    $env:RGPU_CET_PROBE_CALLS = "5000000"

    $started = Start-Process -FilePath $game -WorkingDirectory $bin -PassThru
    Write-Host "Started TXR pid=$($started.Id); observing for $DurationSeconds seconds"
    Start-Sleep -Seconds $DurationSeconds
}
finally {
    # Stop only the exact process launched by this bounded probe.
    if ($started) {
        Stop-Process -Id $started.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
    Remove-Item Env:RGPU_LOG_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:RGPU_CET_PROBE_MS -ErrorAction SilentlyContinue
    Remove-Item Env:RGPU_CET_PROBE_CALLS -ErrorAction SilentlyContinue

    if ($backup -and (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $backup -Destination $deployed -Force
    } elseif (Test-Path -LiteralPath $deployed) {
        Remove-Item -LiteralPath $deployed -Force
    }
}

if (-not (Test-Path -LiteralPath $evidenceLog)) { throw "TXR produced no rgpu log" }

$interesting = Select-String -LiteralPath $evidenceLog -Pattern @(
    "CORE_EXPORT_TABLE", "produced a command", "render QUEUE", "ExecuteCommandLists",
    "submit#", "D3D11", "DXGI pDevice", "FIRST Draw", "FIRST Dispatch", "ATTACH"
) | ForEach-Object { $_.Line }

$summary = @(
    "TXR rgpu corrected export-table probe",
    "Timestamp: $stamp",
    "DurationSeconds: $DurationSeconds",
    "EvidenceLog: $evidenceLog",
    "InterestingLines: $($interesting.Count)",
    "",
    $interesting
)
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$summary | ForEach-Object { Write-Host $_ }
Write-Host "SUMMARY_PATH=$summaryPath"
Write-Host "EVIDENCE_LOG=$evidenceLog"
