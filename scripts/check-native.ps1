[CmdletBinding()]
param([string[]]$Checks)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build/game/DrakengardRecompile.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'Build the game executable first' }

# The executable owns the check list; keep expected fail-closed exits explicit.
$available = @(& $exe --list-checks | Where-Object { $_ -like '--check-*' })
if ($LASTEXITCODE -ne 0 -or !$available.Count) { throw 'Cannot enumerate native checks' }
if (!$Checks) { $Checks = $available }
foreach ($check in $Checks) {
    if ($check -notin $available) { throw "Unknown native check: $check" }
}

foreach ($check in $Checks) {
    & $exe $check
    $expected = if ($check -in @('--check-spu-interpreter-halt','--check-spu-task-null-branch')) { 8 } else { 0 }
    if ($LASTEXITCODE -ne $expected) {
        throw "$check returned $LASTEXITCODE; expected $expected"
    }
}
Write-Host "Native checks passed: $($Checks.Count)"
exit 0
