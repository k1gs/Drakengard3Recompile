[CmdletBinding()]
param([ValidateRange(5,300)][int]$Seconds=30, [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Run='001', [switch]$CaptureFrames, [switch]$CaptureRsx, [switch]$Interactive, [switch]$KeyboardOnly, [Alias('SkipIntro')][switch]$SkipMovies, [ValidateRange(1,600)][int]$CaptureCount=12, [ValidateRange(1,3600)][int]$CaptureEvery=120)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build/game/DrakengardRecompile.exe'
$elf = Join-Path $root 'work/extracted/PS3_GAME/USRDIR/EBOOT.elf'
$prefix = Join-Path $root "reports/game-run-$Run"
if (Test-Path "$prefix.json") { throw 'Choose a fresh Run ID' }
if ((Get-FileHash $elf -Algorithm SHA256).Hash -ne '454fb591e9192cf9d4a5c65912eb4cd8c982ba52e26474bcfc9dc6ee1dc12acc') {
    throw 'ELF does not match the build used for this lift'
}
$runDir = Join-Path $root "work/native-runs/$Run"
New-Item -ItemType Directory -Force $runDir | Out-Null
$jobDir = Join-Path $runDir 'spu-job-dumps'
New-Item -ItemType Directory -Force $jobDir | Out-Null
$fifoDir = Join-Path $runDir 'gpu-fifo-dumps'
New-Item -ItemType Directory -Force $fifoDir | Out-Null
$values = @{
    PS3_VFS_ROOT=(Join-Path $root 'work/extracted');
    PS3_HDD0_ROOT=(Join-Path $root 'work/native-state/dev_hdd0');
    PS3_DEV_FLASH=(Join-Path $root 'tools/rpcs3/dev_flash');
    PS3_BOOT_PATH='/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN';
    PS3_HLE_UNRESOLVED='fail'
    PS3_EDAT_CACHE=(Join-Path $runDir 'edat-cache')
    SPU_DUMP_MISS=$jobDir
    PORT_FIFO_DUMP_DIR=$fifoDir
}
$previous = @{}
$values.PORT_VISIBLE_TEST=if ($Interactive) { '1' } else { '0' }
if ($Interactive -and !('NativeTestInput' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NativeTestInput {
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    public static bool ExitRequested(int gamePid) {
        uint foregroundPid;
        GetWindowThreadProcessId(GetForegroundWindow(), out foregroundPid);
        return foregroundPid == (uint)gamePid && (GetAsyncKeyState(0x7B) & 0x8000) != 0;
    }
}
'@
}
if ($KeyboardOnly) { $values.PORT_KEYBOARD_ONLY='1' }
if ($SkipMovies) { $values.PORT_SKIP_MOVIES='1' }
$diagnostics = @{}
if ($env:PORT_SPU_PROFILE) { $diagnostics.PORT_SPU_PROFILE=$env:PORT_SPU_PROFILE }
if ($env:PORT_MSDSP_NATIVE) { $diagnostics.PORT_MSDSP_NATIVE=$env:PORT_MSDSP_NATIVE }
$diagnosticNames = @(
    'D3D12_DBG', 'D3D12_IQ', 'RSX_CAP', 'RSX_CAP_DIR', 'CELLMARK_DUMP_SKIP', 'GCM_FIFO_KICK',
    'PS3_POLLTOP', 'PORT_CLOCK_PROFILE', 'PS3_SCBLOCK_PROF', 'PORT_HLE_PROFILE',
    'PORT_HLE_PROFILE_TID', 'PORT_HLE_PROFILE_NAME', 'PS3_SYNCLOG', 'PERF', 'PSOMISSDBG', 'RSX_ACCUM_FRAME',
    'HASHDBG', 'SPU_INTERP_UNLIFTED', 'PS3_WAITBT', 'PS3_SPU_PCWATCH',
    'SPU_DUMP_OVL', 'YDKJ_SPU_TRACE', 'SPU_CALLTRACE', 'PORT_EVENT_TRACE',
    'PORT_PPU_SAMPLE', 'GCM_DRAINDBG', 'GCM_CTXDBG', 'GCM_RECDBG',
    'PORT_QUEUE_WAIT_TRACE', 'PAD_FILE', 'PAD_SCRIPT',
    'PORT_CLEAR_TRACE', 'PORT_CAPTURE_TRIGGER', 'PORT_CAPTURE_INTERVAL', 'PORT_SPU_ENTRY_CAPTURE', 'PORT_AUDIO_DECODER_NATIVE',
    'PORT_GPU_WRITERS', 'PORT_ALLOC_TRACE', 'PORT_LF_TRACE', 'PORT_RESOURCE_TRACE',
    'PPU_GUARD_EA', 'PPU_CASWATCH', 'VP_NOFIXPROJ', 'RSX_CAP_OFFSCREEN',
    'DEPTH_OFF', 'CULL_OFF', 'CULL_FLIP', 'PORT_RENDER_TRACE',
    'PORT_JC_TRACE', 'NO_ALPHATEST'
)
foreach ($name in $diagnosticNames) {
    $value = [Environment]::GetEnvironmentVariable($name, 'Process')
    if ($null -ne $value) { $diagnostics[$name]=$value }
}
if ($CaptureFrames) {
    # Renderer GPU readback, scoped to this child and this ignored run directory.
    $values.CELLMARK_DUMP="$CaptureCount"
    $values.CELLMARK_DUMP_EVERY="$CaptureEvery"
    $values.CELLMARK_DUMP_MINDRAWS='0'
    $values.CELLMARK_DUMP_DIR=$runDir
}
if ($CaptureRsx) {
    $values.RSX_CAP='200:3:120'
    $values.RSX_CAP_DIR=(Join-Path $runDir 'rsx-captures')
}
$exeHash = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$process = $null
# An exclusive lease also works when the process sandbox hides other runners.
# The OS releases it if this runner is interrupted or dies.
$lease = [System.IO.File]::Open((Join-Path $root 'work/native-run.lock'), [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
$started = [DateTime]::UtcNow
try {
    foreach ($name in $values.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $values[$name], 'Process')
    }
    $launcherArgs = @((Join-Path $PSScriptRoot 'start_native.py'), '--exe', $exe, '--elf', $elf, '--cwd', $runDir, '--stdout', "$prefix.log", '--stderr', "$prefix-errors.log")
    if ($Interactive) { $launcherArgs += '--interactive' }
    $gamePid = & (Join-Path $root '.venv/Scripts/python.exe') @launcherArgs
    if ($LASTEXITCODE -ne 0) { throw 'Native process launch failed' }
    $process = [System.Diagnostics.Process]::GetProcessById([int]$gamePid)
    # Retain a process handle before exit; Windows PowerShell's Start-Process
    # object can otherwise lose ExitCode when the child finishes.
    $tracked = [System.Diagnostics.Process]::GetProcessById($process.Id)
    $null = $tracked.Handle
    $testTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $stopReason = 'process_exit'
    while (!$tracked.WaitForExit(50)) {
        if ($Interactive -and [NativeTestInput]::ExitRequested($tracked.Id)) {
            $stopReason = 'f12'; break
        }
        if ($testTimer.Elapsed.TotalSeconds -ge $Seconds) {
            $stopReason = 'time_limit'; break
        }
    }
    $timedOut = $stopReason -eq 'time_limit'
    if (!$tracked.HasExited) { $tracked.Kill(); $tracked.WaitForExit() }
    @{
        started_utc=$started.ToString('o'); duration_seconds=([DateTime]::UtcNow-$started).TotalSeconds;
        pid=$process.Id; exit_code=$tracked.ExitCode; stopped_at_time_limit=$timedOut;
        stop_reason=$stopReason; time_limit_seconds=$Seconds;
        interactive_window=$Interactive.IsPresent;
        log_transport='native inherited file handles';
        exe_sha256=$exeHash;
        elf_sha256=(Get-FileHash $elf -Algorithm SHA256).Hash.ToLowerInvariant();
        environment=$values; working_directory=$runDir;
        inherited_diagnostics=$diagnostics;
        validation='Bounded native boot trace; no visual/menu/gameplay validation. Unknown imports return errors, not fake success.'
    } | ConvertTo-Json -Depth 4 | Set-Content "$prefix.json"
} finally {
    if ($null -ne $process -and !$process.HasExited) {
        $process.Kill()
        $process.WaitForExit()
    }
    $lease.Dispose()
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}
Write-Host "Native boot trace saved to $prefix.*"
