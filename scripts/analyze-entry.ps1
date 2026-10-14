$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$slice = Join-Path $root 'work/entry-slice-001/entry.raw'
if (!(Test-Path $slice)) { throw 'Run scripts/slice_entry.py first; see README' }
$loader = Get-Content (Join-Path $root 'work/ppu-stage1/EBOOT.loader.json') -Raw | ConvertFrom-Json
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-21'
New-Item -ItemType Directory -Force (Join-Path $root 'tools/java-home') | Out-Null
$env:JAVA_TOOL_OPTIONS = '-Duser.home="' + (Join-Path $root 'tools/java-home') + '"'
$headless = (Get-ChildItem "$root/tools/ghidra/*/support/analyzeHeadless.bat" | Select-Object -First 1).FullName
$project = Join-Path $root 'work/ghidra'
New-Item -ItemType Directory -Force $project | Out-Null
$result = Join-Path $root 'work/ppu-stage1/entry-ghidra-raw-001.txt'
if (Test-Path $result) { throw 'Entry report already exists' }
& $headless $project drakengard-entry-raw-001 -import $slice -loader BinaryLoader -loader-baseAddr ($loader.entry_code -replace '^0x','') -processor 'PowerPC:BE:64:default' -noanalysis -scriptPath (Join-Path $root 'scripts/ghidra') -postScript TitleEntry.java $loader.entry_code $result
if ($LASTEXITCODE -ne 0 -or !(Test-Path $result)) { throw 'Entry exploration failed; inspect Ghidra log' }
