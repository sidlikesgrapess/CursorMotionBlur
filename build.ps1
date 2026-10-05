# Builds dist\CursorMotionBlur.exe with the C# compiler that ships with Windows (.NET Framework 4.x) - no SDK needed.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc" }

New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
& $csc -nologo -optimize+ -target:winexe `
    -win32icon:"$root\assets\icon.ico" `
    -r:System.Windows.Forms.dll -r:System.Drawing.dll `
    -out:"$root\dist\CursorMotionBlur.exe" `
    "$root\src\*.cs"
if ($LASTEXITCODE -ne 0) { throw "build failed" }
"Built $root\dist\CursorMotionBlur.exe"
