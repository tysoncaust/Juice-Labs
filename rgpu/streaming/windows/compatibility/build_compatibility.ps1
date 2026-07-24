param(
    [ValidateSet('Passive','Interactive','All')]
    [string]$Mode = 'Passive'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $root 'out'
$common = Join-Path $out 'common'
$passive = Join-Path $out 'passive'
$interactive = Join-Path $out 'interactive'
New-Item -ItemType Directory -Force -Path $common,$passive | Out-Null
if ($Mode -eq 'Passive' -and (Test-Path $interactive)) { Remove-Item $interactive -Recurse -Force }
$cl = (Get-Command cl.exe -ErrorAction Stop).Source
function Invoke-Checked([string[]]$Arguments) { & $cl @Arguments; if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE" } }
Invoke-Checked @('/nologo','/std:c++17','/EHsc','/W4','/WX',"/Fo:$common\wasapi_loopback_capture.obj",(Join-Path $root 'wasapi_loopback_capture.cpp'),"/Fe:$common\rgpu_wasapi_loopback_capture.exe",'/link','ole32.lib','avrt.lib')
Invoke-Checked @('/nologo','/std:c++17','/EHsc','/W4','/WX',"/Fo:$common\wasapi_tone_player.obj",(Join-Path $root 'wasapi_tone_player.cpp'),"/Fe:$common\rgpu_wasapi_tone_player.exe",'/link','ole32.lib','avrt.lib')
'PASSIVE_INPUT_BROKER_COMPILED=FALSE' | Set-Content -Encoding utf8 (Join-Path $passive 'mode-summary.txt')
if ($Mode -in @('Interactive','All')) {
    New-Item -ItemType Directory -Force -Path $interactive | Out-Null
    Invoke-Checked @('/nologo','/std:c++17','/EHsc','/W4','/WX',"/Fo:$interactive\input_test_window.obj",(Join-Path $root 'input_test_window.cpp'),"/Fe:$interactive\rgpu_input_test_window.exe",'/link','/SUBSYSTEM:WINDOWS','user32.lib','shell32.lib')
    Invoke-Checked @('/nologo','/std:c++17','/EHsc','/W4','/WX',"/Fo:$interactive\input_relay_host.obj",(Join-Path $root 'input_relay_host.cpp'),"/Fe:$interactive\rgpu_input_relay_host.exe",'/link','ws2_32.lib','user32.lib')
    @('INTERACTIVE_INPUT_BROKER_COMPILED=TRUE','HUMAN_INPUT_ONLY=TRUE','MACROS=FALSE','AUTOMATION=FALSE') | Set-Content -Encoding utf8 (Join-Path $interactive 'mode-summary.txt')
}
$summary = @('RGPU_COMPATIBILITY_BUILD=PASS',"BUILD_MODE=$Mode",'WASAPI_LOOPBACK_CAPTURE=BUILD_PASS','WASAPI_TONE_PLAYER=BUILD_PASS','PASSIVE_INPUT_BROKER_COMPILED=FALSE',"INTERACTIVE_INPUT_BROKER_COMPILED=$(if ($Mode -in @('Interactive','All')) {'TRUE'} else {'FALSE'})",'CUSTOM_GRAPHICS_DRIVER=FALSE','VIRTUAL_DISPLAY=FALSE','KERNEL_DRIVER=FALSE','ANTI_CHEAT_BYPASS_COMPONENTS=FALSE')
$summaryPath = Join-Path $out 'build-summary.txt'; $summary | Set-Content -Encoding utf8 $summaryPath; $summary | ForEach-Object { Write-Output $_ }; Write-Output "EVIDENCE=$summaryPath"
