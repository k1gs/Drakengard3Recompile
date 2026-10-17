[CmdletBinding()]
param([ValidateRange(5,300)][int]$Seconds=30, [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Run='001', [switch]$CaptureFrames, [switch]$CaptureRsx, [switch]$Interactive)
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
$diagnostics = @{}
if ($env:PORT_SPU_PROFILE) { $diagnostics.PORT_SPU_PROFILE=$env:PORT_SPU_PROFILE }
if ($env:PORT_MSDSP_NATIVE) { $diagnostics.PORT_MSDSP_NATIVE=$env:PORT_MSDSP_NATIVE }
$diagnosticNames = @(
    'D3D12_DBG', 'D3D12_IQ', 'RSX_CAP_DIR', 'GCM_FIFO_KICK',
    'PS3_POLLTOP', 'PORT_CLOCK_PROFILE', 'PS3_SCBLOCK_PROF', 'PORT_HLE_PROFILE',
    'PORT_HLE_PROFILE_TID', 'PS3_SYNCLOG', 'PERF', 'PSOMISSDBG',
    'HASHDBG', 'SPU_INTERP_UNLIFTED', 'PS3_WAITBT', 'PS3_SPU_PCWATCH',
    'SPU_DUMP_OVL', 'YDKJ_SPU_TRACE', 'SPU_CALLTRACE', 'PORT_EVENT_TRACE',
    'PORT_PPU_SAMPLE', 'GCM_DRAINDBG', 'GCM_CTXDBG', 'GCM_RECDBG',
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
    $values.CELLMARK_DUMP='12'
    $values.CELLMARK_DUMP_EVERY='120'
    $values.CELLMARK_DUMP_MINDRAWS='1'
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
    $timedOut = !$tracked.WaitForExit($Seconds*1000)
    if ($timedOut -and !$tracked.HasExited) { $tracked.Kill(); $tracked.WaitForExit() }
    @{
        started_utc=$started.ToString('o'); duration_seconds=([DateTime]::UtcNow-$started).TotalSeconds;
        pid=$process.Id; exit_code=$tracked.ExitCode; stopped_at_time_limit=$timedOut;
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
