param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ build tools are required to build the bundled resampler.' }
$cmake = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "Visual Studio CMake tools are missing: $cmake" }
$buildDirectory = Join-Path $PSScriptRoot 'obj/soxr'
& $cmake -S (Join-Path $PSScriptRoot 'Native/soxr') -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DWITH_OPENMP=OFF -DWITH_LSR_BINDINGS=OFF -DBUILD_SHARED_LIBS=ON
if ($LASTEXITCODE -ne 0) { throw 'Resampler configuration failed.' }
& $cmake --build $buildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw 'Resampler build failed.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDirectory 'bin/Release/soxr.dll') -Destination (Join-Path $OutputDirectory 'soxr.dll')
