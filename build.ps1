# Builds dist\CursorMotionBlur.exe with clang from llvm-mingw (https://github.com/mstorsjo/llvm-mingw): a single small exe that
# needs nothing but Windows 10/11. Pass -Renderer cpu to build the CPU renderer instead of the GPU one.
param([ValidateSet('gpu', 'cpu')] [string]$Renderer = 'gpu', [string]$Out = "$PSScriptRoot\dist\CursorMotionBlur.exe")
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$Out = [IO.Path]::GetFullPath($Out)
if (-not (Get-Command clang -ErrorAction SilentlyContinue)) { throw 'clang not found: install llvm-mingw and put its bin folder on PATH' }

New-Item -ItemType Directory -Force "$root\dist" | Out-Null
Push-Location "$root\src"
try {
    & windres app.rc -O coff -o "$root\dist\app.res"
    if ($LASTEXITCODE -ne 0) { throw 'resource build failed' }
    [string[]]$render = if ($Renderer -eq 'gpu') { 'render_gpu.cpp', '-ld3d11', '-ldcomp' } else { 'render_cpu.c' }
    & clang -O2 -s -municode -mwindows -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -Wall `
        main.c engine.c ui.c $render "$root\dist\app.res" `
        -luser32 -lgdi32 -lshell32 -ladvapi32 -lcomctl32 -lwinhttp -lwinmm -lshcore `
        -o $Out
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
} finally { Pop-Location; Remove-Item "$root\dist\app.res" -ErrorAction SilentlyContinue }
"Built $Out ($Renderer renderer, $((Get-Item $Out).Length) bytes)"
