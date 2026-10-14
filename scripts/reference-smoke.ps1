[CmdletBinding()]
param([ValidateRange(10,300)][int]$Seconds=120, [string]$Run='001', [switch]$PpuInterpreter)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$intake = Join-Path $root 'reports/extracted-intake-003.json'
if (!(Test-Path $intake)) { throw 'Wait for full extraction and intake to finish' }
$report = Get-Content $intake -Raw | ConvertFrom-Json
if ($report.errors.Count -ne 0 -or $report.files.Count -ne 8784) {
    throw 'Unexpected extracted intake; compare ISO manifest before booting'
}
$prefix = Join-Path $root "reports/reference-smoke-$Run"
if (Test-Path "$prefix.json") { throw 'Choose a fresh Run ID' }
$baseConfig = Join-Path $root 'tools/rpcs3/config/config.yml'
$text = Get-Content $baseConfig -Raw
# Headless RPCS3 automatically uses Null video. Keep this trace silent too.
$text = $text -replace '(?m)^Audio:\r?\n  Renderer: [^\r\n]*', "Audio:`n  Renderer: Null"
if ($PpuInterpreter) { $text = $text -replace '(?m)^  PPU Decoder: [^\r\n]*', '  PPU Decoder: Interpreter (static)' }
$config = "$prefix-config.yml"
Set-Content -LiteralPath $config -Value $text -Encoding utf8
$binary = Join-Path $root 'work/extracted/PS3_GAME/USRDIR/EBOOT.BIN'
$started = [DateTime]::UtcNow
$process = Start-Process -FilePath (Join-Path $root 'tools/rpcs3/rpcs3.exe') -ArgumentList @('--headless','--config',('"'+$config+'"'),('"'+$binary+'"')) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$prefix.txt" -RedirectStandardError "$prefix-errors.txt"
$timedOut = !$process.WaitForExit($Seconds*1000)
if ($timedOut) {
    # Only the process started above is terminated. The run uses local disposable state.
    if (!$process.HasExited) { $process.Kill() }
    $process.WaitForExit()
}
Copy-Item (Join-Path $root 'tools/rpcs3/log/RPCS3.log') "$prefix.log"
$configRejected = [bool](Select-String -Path "$prefix.log" -Pattern 'invalid enum or integer' -Quiet)
@{started_utc=$started.ToString('o'); duration_seconds=([DateTime]::UtcNow-$started).TotalSeconds;
  pid=$process.Id; exit_code=$process.ExitCode; stopped_at_time_limit=$timedOut;
  config_sha256=(Get-FileHash $config -Algorithm SHA256).Hash.ToLowerInvariant();
  ppu_interpreter_experiment=[bool]$PpuInterpreter;
  config_rejected=$configRejected;
  validation='Headless trace only. Null video/audio; no GUI or gameplay validation.'} | ConvertTo-Json | Set-Content "$prefix.json"
Write-Host "Reference trace saved to $prefix.*"
if ($configRejected) { throw 'RPCS3 rejected configuration values; this run is not a valid decoder experiment' }
