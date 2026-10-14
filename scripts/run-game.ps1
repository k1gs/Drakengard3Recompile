[CmdletBinding()]
param([ValidateRange(5,300)][int]$Seconds=30, [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Run='001')
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
$values = @{
    PS3_VFS_ROOT=(Join-Path $root 'work/extracted');
    PS3_HDD0_ROOT=(Join-Path $root 'work/native-state/dev_hdd0');
    PS3_DEV_FLASH=(Join-Path $root 'tools/rpcs3/dev_flash');
    PS3_BOOT_PATH='/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN';
    PS3_HLE_UNRESOLVED='fail'
    PS3_EDAT_CACHE=(Join-Path $runDir 'edat-cache')
    SPU_DUMP_MISS=$jobDir
}
$previous = @{}
$process = $null
$started = [DateTime]::UtcNow
try {
    foreach ($name in $values.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $values[$name], 'Process')
    }
    $process = Start-Process -FilePath $exe -ArgumentList ('"'+$elf+'"') -WorkingDirectory $runDir -WindowStyle Hidden -PassThru -RedirectStandardOutput "$prefix.log" -RedirectStandardError "$prefix-errors.log"
    # Retain a process handle before exit; Windows PowerShell's Start-Process
    # object can otherwise lose ExitCode when the child finishes.
    $tracked = [System.Diagnostics.Process]::GetProcessById($process.Id)
    $null = $tracked.Handle
    $timedOut = !$tracked.WaitForExit($Seconds*1000)
    if ($timedOut -and !$tracked.HasExited) { $tracked.Kill(); $tracked.WaitForExit() }
    @{
        started_utc=$started.ToString('o'); duration_seconds=([DateTime]::UtcNow-$started).TotalSeconds;
        pid=$process.Id; exit_code=$tracked.ExitCode; stopped_at_time_limit=$timedOut;
        exe_sha256=(Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant();
        elf_sha256=(Get-FileHash $elf -Algorithm SHA256).Hash.ToLowerInvariant();
        environment=$values; working_directory=$runDir;
        validation='Bounded native boot trace; no visual/menu/gameplay validation. Unknown imports return errors, not fake success.'
    } | ConvertTo-Json -Depth 4 | Set-Content "$prefix.json"
} finally {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}
Write-Host "Native boot trace saved to $prefix.*"
