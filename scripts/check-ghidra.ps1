$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-21'
# Keep the validation profile and projects local to this workspace.
$env:JAVA_TOOL_OPTIONS = '-Duser.home="' + (Join-Path $root 'tools/java-home') + '"'
New-Item -ItemType Directory -Force (Join-Path $root 'tools/java-home') | Out-Null
$headless = (Get-ChildItem "$root/tools/ghidra/*/support/analyzeHeadless.bat" | Select-Object -First 1).FullName
$fixtures = Join-Path $root 'work/synthetic'
New-Item -ItemType Directory -Force $fixtures | Out-Null
[IO.File]::WriteAllBytes((Join-Path $fixtures 'ppu.raw'), [byte[]](0x38,0x60,0,1,0x4e,0x80,0,0x20))
[IO.File]::WriteAllBytes((Join-Path $fixtures 'spu.raw'), [byte[]](0,0,0,0))
foreach ($case in @(@{file='ppu.raw'; lang='PowerPC:BE:64:default'}, @{file='spu.raw'; lang='SPU:BE:128:default'})) {
    & $headless $fixtures ('smoke-' + $case.file) -import (Join-Path $fixtures $case.file) -loader BinaryLoader -processor $case.lang -noanalysis -scriptPath (Join-Path $root 'scripts/ghidra') -postScript SmokeCheck.java -deleteProject
    if ($LASTEXITCODE -ne 0) { throw "Ghidra failed: $($case.lang)" }
}
