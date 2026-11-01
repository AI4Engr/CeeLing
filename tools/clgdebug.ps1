# PowerShell version of tools/clgdebug.sh: compile a CeeLing program for the debugger and start gdb, stopped at the first statement of main().
#   tools\clgdebug.ps1 FILE.clg [more gdb arguments, e.g. -ex 'break FILE.clg:12']
# cling -g FILE.clg -> FILE.debug.cpp, g++ -g -O0 -> FILE.debug.exe (next to FILE.clg, so gdb finds the .clg source), then gdb -x tools\cling.gdb with a breakpoint on main.
# $env:CLING names the CeeLing compiler (default: build\cling.exe in the repository), $env:CXX the C++ compiler (default g++); g++ and gdb must be on PATH.
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Source,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$GdbArgs
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$cling = $env:CLING
if (-not $cling) { $cling = Join-Path $repo 'build\cling.exe' }
if (-not (Test-Path $cling)) { throw "no CeeLing compiler at ${cling}: build it or set `$env:CLING" }
$cxx = if ($env:CXX) { $env:CXX } else { 'g++' }
foreach ($tool in @($cxx, 'gdb')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is not on PATH" }
}
if (-not (Test-Path $Source)) { throw "no such file: $Source" }
$full = (Resolve-Path $Source).Path
$dir = Split-Path -Parent $full
$file = Split-Path -Leaf $full
$base = [System.IO.Path]::GetFileNameWithoutExtension($file)
Push-Location $dir
try {
    & $cling -g $file --cpp "$base.debug.cpp" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cling did not accept $file" }
    & $cxx -std=c++17 -pthread -g -O0 "$base.debug.cpp" -o "$base.debug.exe"
    if ($LASTEXITCODE -ne 0) { throw 'g++ failed' }
    Write-Host "built $dir\$base.debug.exe from $file; starting gdb (type 'step', 'next', 'bt', 'print x', 'continue', 'quit')"
    $script = (Join-Path $repo 'tools\cling.gdb') -replace '\\', '/'
    $ErrorActionPreference = 'Continue'    # (gdb writes harmless notes to stderr; PowerShell must not treat them as errors)
    & gdb -q -x $script -ex 'break main' -ex 'run' @GdbArgs ".\$base.debug.exe"
} finally {
    Pop-Location
}
