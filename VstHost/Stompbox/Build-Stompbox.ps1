# Builds stompbox-capi.dll from the pinned Stompbox commit (see UPSTREAM.md) into VstHost\Stompbox\bin.
$ErrorActionPreference = 'Stop'

$Commit = '17bc65264ff89a6b8a19843264292ca7762fdad3'
$Repository = 'https://github.com/mikeoliphant/stompbox'
$Here = $PSScriptRoot
$Source = Join-Path $Here 'build\source'
$Build = Join-Path $Here 'build\cmake'
$Output = Join-Path $Here 'bin'

function Invoke-Checked([string]$File, [string[]]$Arguments)
{
	& $File @Arguments
	if ($LASTEXITCODE -ne 0) { throw "$File $($Arguments -join ' ') failed with exit code $LASTEXITCODE." }
}

if (-not (Test-Path (Join-Path $Source '.git')))
{
	New-Item -ItemType Directory -Force -Path $Source | Out-Null
	Invoke-Checked git @('-C', $Source, 'init', '--quiet')
	Invoke-Checked git @('-C', $Source, 'remote', 'add', 'origin', $Repository)
}
Invoke-Checked git @('-C', $Source, 'fetch', '--quiet', '--depth', '1', 'origin', $Commit)
Invoke-Checked git @('-C', $Source, 'checkout', '--quiet', '--force', $Commit)
Invoke-Checked git @('-C', $Source, 'submodule', 'update', '--init', '--recursive', '--depth', '1')

# Local change: the remote server listens on this PC only.
$Server = Join-Path $Source 'stompbox-core\StompboxServer.cpp'
$Text = [System.IO.File]::ReadAllText($Server)
$Upstream = 'iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);'
$Local = 'iResult = getaddrinfo("127.0.0.1", DEFAULT_PORT, &hints, &result);'
if (-not $Text.Contains($Local))
{
	if (-not $Text.Contains($Upstream)) { throw "StompboxServer.cpp no longer has the line the localhost patch replaces; review the upstream change." }
	[System.IO.File]::WriteAllText($Server, $Text.Replace($Upstream, $Local))
}

Invoke-Checked cmake @('-S', $Source, '-B', $Build, '-G', 'Visual Studio 17 2022', '-A', 'x64')
Invoke-Checked cmake @('--build', $Build, '--config', 'Release', '--target', 'stompbox-capi')

$Dll = Get-ChildItem -Path $Build -Recurse -Filter 'stompbox-capi.dll' | Where-Object { $_.FullName -match '\\Release\\' } | Select-Object -First 1
if (-not $Dll) { throw "The build finished but stompbox-capi.dll was not found under $Build." }
New-Item -ItemType Directory -Force -Path $Output | Out-Null
Copy-Item $Dll.FullName (Join-Path $Output 'stompbox-capi.dll') -Force
Write-Host "stompbox-capi.dll ($Commit) -> $Output"
