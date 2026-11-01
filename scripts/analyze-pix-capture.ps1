[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CapturePath,
    [string]$OutputDirectory,
    [ValidateRange(5,300)][int]$Seconds=120
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$capture=Get-Item -LiteralPath $CapturePath
if($capture.Extension -ne '.wpix' -or !$capture.Length){throw 'Choose a nonempty PIX .wpix file'}
$tools=& (Join-Path $PSScriptRoot 'doctor-gpu-tools.ps1') -Json | ConvertFrom-Json
if($tools.developer_mode -eq $false) {
    throw 'PIX replay requires Windows Developer Mode. Enable it only with explicit user authorization.'
}
$pix=$tools.pix | Sort-Object { [version]$_.version } -Descending | Select-Object -First 1
if(!$pix){throw 'pixtool.exe not found'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root ('work/pix-analysis-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$out=[IO.Path]::GetFullPath($OutputDirectory)
$allowed=@('work','reports') | ForEach-Object { [IO.Path]::GetFullPath((Join-Path $root $_))+[IO.Path]::DirectorySeparatorChar }
if(!($allowed | Where-Object { $out.StartsWith($_,[StringComparison]::OrdinalIgnoreCase) })) {
    throw 'Keep PIX analysis output inside the ignored work/ or reports/ directory'
}
foreach($name in @('events.csv','stdout.log','stderr.log','analysis.json','pix.log')) {
    if(Test-Path -LiteralPath (Join-Path $out $name)){throw 'Choose a fresh analysis output directory'}
}
New-Item -ItemType Directory -Force $out | Out-Null
$events=Join-Path $out 'events.csv'
# Paths on Windows cannot contain quotes. Quote each argument for Start-Process,
# whose ArgumentList is joined into a native command line on Windows PowerShell.
$args=@('open-capture',$capture.FullName,
    'perform-single-playback','save-event-list',$events) | ForEach-Object { '"'+$_+'"' }
$process=$null;$timedOut=$false;$watch=[Diagnostics.Stopwatch]::StartNew()
try {
    $process=Start-Process -FilePath $pix.path -ArgumentList $args -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError (Join-Path $out 'stderr.log')
    if(!$process.WaitForExit($Seconds*1000)){$timedOut=$true;throw 'PIX analysis time limit reached'}
    $process.Refresh()
    if($process.ExitCode -ne 0){throw "PIX analysis failed ($($process.ExitCode)); inspect the saved logs"}
    if(!(Test-Path -LiteralPath $events) -or !(Get-Item -LiteralPath $events).Length) {
        throw 'PIX returned success without a nonempty event list'
    }
    Write-Host "PIX replay and event export succeeded: $events"
} finally {
    if($process -and !$process.HasExited) {
        # The timeout owns this process and its playback children, never an
        # unrelated PIX session. PowerShell 5 uses the native tree-kill fallback.
        if($PSVersionTable.PSVersion.Major -ge 7){$process.Kill($true)}
        else {& taskkill.exe /PID $process.Id /T /F | Out-Null}
        $process.WaitForExit()
    }
    [ordered]@{capture=$capture.FullName;tool=$pix.path;version=$pix.version;
        elapsed_seconds=$watch.Elapsed.TotalSeconds;timed_out=$timedOut;
        exit_code=if($process -and $process.HasExited){$process.ExitCode}else{$null};
        events=$events} | ConvertTo-Json | Set-Content (Join-Path $out 'analysis.json')
}
