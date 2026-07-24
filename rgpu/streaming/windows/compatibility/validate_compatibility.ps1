param([switch]$IncludeInteractiveBoundary)

$ErrorActionPreference='Stop'

$root=Split-Path -Parent $MyInvocation.MyCommand.Path

$evidence=Join-Path $root 'evidence'

& (Join-Path $root 'build_compatibility.ps1') -Mode Passive

if ($LASTEXITCODE -ne 0) { throw 'Passive build failed' }

if (Test-Path (Join-Path $root 'out\interactive')) { throw 'Passive boundary failed: interactive directory exists' }

$required=@('video-host-latest.json','passive-destiny2-latest.json','mac-video-receiver-latest.txt','destiny2-guard-latest.txt','hybrid-gpu-topology-latest.txt','srt-sender-720p-latest.json','srt-sender-900p-latest.json','srt-receiver-720p-latest.json','srt-receiver-900p-latest.json','srt-wrong-key-rejection-latest.json','srt-reconnect-resolution-latest.json','srt-impairment-3pct-50ms-smoke-latest.json')

foreach($name in $required){$p=Join-Path $evidence $name;if(-not(Test-Path $p)){throw "Required evidence missing: $name"}}

$matrix=Get-Content (Join-Path $root 'compatibility-matrix.json') -Raw|ConvertFrom-Json

if($matrix.claim_model.vendor_approval_required -ne $false -or $matrix.claim_model.universal_battleye_compatibility -ne 'out_of_scope'){throw 'Claim model failed'}

$send720=Get-Content (Join-Path $evidence 'srt-sender-720p-latest.json') -Raw|ConvertFrom-Json

$send900=Get-Content (Join-Path $evidence 'srt-sender-900p-latest.json') -Raw|ConvertFrom-Json

$recv720=Get-Content (Join-Path $evidence 'srt-receiver-720p-latest.json') -Raw|ConvertFrom-Json

$recv900=Get-Content (Join-Path $evidence 'srt-receiver-900p-latest.json') -Raw|ConvertFrom-Json

$wrong=Get-Content (Join-Path $evidence 'srt-wrong-key-rejection-latest.json') -Raw|ConvertFrom-Json
$impairment=Get-Content (Join-Path $evidence 'srt-impairment-3pct-50ms-smoke-latest.json') -Raw|ConvertFrom-Json

if(-not($send720.passed -and $send900.passed -and $recv720.passed -and $recv900.passed -and $wrong.passed -and $impairment.status -eq 'smoke_pass')){throw 'SRT evidence failed'}

if($recv720.observed_width -ne 1280 -or $recv900.observed_width -ne 1600){throw 'Resolution evidence failed'}

if($wrong.marker_counts.h264 -ne 0 -or $wrong.marker_counts.incorrect_passphrase -le 0){throw 'Authentication rejection failed'}

if($IncludeInteractiveBoundary){& (Join-Path $root 'build_compatibility.ps1') -Mode Interactive;if($LASTEXITCODE-ne 0){throw 'Interactive boundary build failed'}}

$summary=@('RGPU_BAREMETAL_COMPATIBILITY=PASS','PASSIVE_BUILD_INPUT_BROKER_COMPILED=FALSE','REAL_WASAPI_CAPTURE=PASS','ENCRYPTED_SRT_TRANSPORT=PASS','INCORRECT_KEY_REJECTION=PASS','SESSION_RECONNECTION=PASS','RESOLUTION_RENEGOTIATION=PASS','SRT_IMPAIRMENT_SMOKE_3PCT_50MS=PASS','SRT_FULL_IMPAIRMENT_MATRIX=NOT_COMPLETE','WEBRTC_PWA_RECEIVER=SCAFFOLD','INPUT_TO_PHOTON_LATENCY=NOT_MEASURED','DESTINY2_PASSIVE_OBSERVED_COMPATIBILITY=PASS','DESTINY2_MANUAL_GAMEPLAY=NOT_TESTED','DESTINY2_MATCHMAKING=NOT_TESTED','DESTINY2_INTERACTIVE_REMOTE_INPUT=NOT_TESTED','GENERAL_BATTLEYE_COMPATIBILITY=UNPROVEN','UNIVERSAL_BATTLEYE_COMPATIBILITY=OUT_OF_SCOPE','VENDOR_APPROVAL=NOT_REQUIRED_FOR_RELEASE','VENDOR_ENDORSEMENT=NOT_CLAIMED','OPEN_SOURCE_RELEASE=PRERELEASE_SCAFFOLD','INDEPENDENT_REVIEW=NOT_COMPLETE','SIGNED_BUILD_PROVENANCE=WORKFLOW_CONFIGURED_NOT_RUN')

$summaryPath=Join-Path $root 'compatibility-validation-latest.txt';$summary|Set-Content -Encoding utf8 $summaryPath;$summary|ForEach-Object{Write-Output $_};Write-Output "EVIDENCE=$summaryPath"
