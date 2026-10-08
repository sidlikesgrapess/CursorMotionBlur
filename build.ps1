# Builds dist\CursorMotionBlur.exe with clang from llvm-mingw (https://github.com/mstorsjo/llvm-mingw): one small exe that
# needs nothing but Windows 10/11.
param([string]$Out = "$PSScriptRoot\dist\CursorMotionBlur.exe")
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$Out = [IO.Path]::GetFullPath($Out)
if (-not (Get-Command clang -ErrorAction SilentlyContinue)) { throw 'clang not found: install llvm-mingw and put its bin folder on PATH' }

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Push-Location "$root\src"
try {
    & windres app.rc -O coff -o "$root\dist\app.res"
    if ($LASTEXITCODE -ne 0) { throw 'resource build failed' }
    & clang -O2 -s -municode -mwindows -fno-asynchronous-unwind-tables -Wall `
        main.c engine.c render.c ui.c "$root\dist\app.res" `
        -luser32 -lgdi32 -lshell32 -ladvapi32 -lcomctl32 -lwinhttp -lwinmm -lshcore `
        -o $Out
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
} finally { Pop-Location; Remove-Item "$root\dist\app.res" -ErrorAction SilentlyContinue }
"Built $Out ($((Get-Item $Out).Length) bytes)"
