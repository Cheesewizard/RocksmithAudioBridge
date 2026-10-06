param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ build tools are required to build the window recorder.' }
$cmake = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "Visual Studio CMake tools are missing: $cmake" }
$buildDirectory = Join-Path $PSScriptRoot 'obj/WindowCapture'
& $cmake -S (Join-Path $PSScriptRoot 'Native/WindowCapture') -B $buildDirectory -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Window recorder configuration failed.' }
& $cmake --build $buildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw 'Window recorder build failed.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDirectory 'Release/rswindowcapture.dll') -Destination (Join-Path $OutputDirectory 'rswindowcapture.dll') -Force
