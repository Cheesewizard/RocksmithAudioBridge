<#
.SYNOPSIS
	Builds RSMods.dll (upstream RSMods, External/RSMods) with this repo's Mod folder compiled in as the external mod
	"RocksmithAudioBridge", and writes the build's own mods.manifest.json.

.DESCRIPTION
	Upstream compiles every .cpp under DLL/ExternalMods/<folder>/. This repo is the product (driver, VST host, GUI,
	installer), so only Mod/ goes in: the script links External/RSMods/DLL/ExternalMods/RocksmithAudioBridge to Mod/
	with a directory junction and builds upstream's DLL project.

	The output goes to build/mod/ and is never copied into the game. Upstream's post-build event copies straight
	over the game's xinput1_3.dll, which would replace the 4.x Audio Bridge DLL there, so it is switched off.

.PARAMETER Configuration
	Release (default) or Debug.

.PARAMETER SkipManifest
	Build only; don't dump mods.manifest.json.
#>
param(
	[ValidateSet('Release', 'Debug')]
	[string]$Configuration = 'Release',
	[switch]$SkipManifest
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$modSource = Join-Path $repoRoot 'Mod'
$upstream = Join-Path $repoRoot 'External\RSMods'
$upstreamDll = Join-Path $upstream 'DLL'
$modLink = Join-Path $upstreamDll 'ExternalMods\RocksmithAudioBridge'
$outDir = Join-Path $repoRoot 'build\mod\'
$intDir = Join-Path $repoRoot "build\mod-obj\$Configuration\"

if (-not (Test-Path (Join-Path $upstreamDll 'DLL.vcxproj'))) {
	throw "External/RSMods is missing. Run: git submodule update --init External/RSMods"
}

if (Test-Path $modLink) {
	$existing = Get-Item $modLink -Force
	if ($existing.LinkType -ne 'Junction' -or ($existing.Target | Select-Object -First 1) -ne $modSource) {
		throw "$modLink exists and is not a junction to $modSource. Remove it by hand; this script never deletes it."
	}
}
else {
	New-Item -ItemType Junction -Path $modLink -Target $modSource | Out-Null
	Write-Host "Linked $modLink -> $modSource"
}

$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (vswhere).' }

& $msbuild (Join-Path $upstreamDll 'DLL.vcxproj') -nologo -m -v:m `
	"-p:Configuration=$Configuration" `
	'-p:Platform=Win32' `
	"-p:SolutionDir=$upstream\" `
	"-p:OutDir=$outDir" `
	"-p:IntDir=$intDir" `
	'-p:PostBuildEventUseInBuild=false'
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed ($LASTEXITCODE)." }

$dll = Join-Path $outDir 'xinput1_3.dll'
$special = (Get-Item $dll).VersionInfo.SpecialBuild
Write-Host "Built $dll ($special)"

if (-not $SkipManifest) {
	$manifest = Join-Path $outDir 'mods.manifest.json'
	# BuildAndRun.ps1 -DumpManifest only looks in upstream's own output folders, so call the export directly.
	if (Test-Path $manifest) { Remove-Item $manifest }
	& "$env:WINDIR\SysWOW64\rundll32.exe" "$dll,DumpManifest" $manifest | Out-Null
	$deadline = (Get-Date).AddSeconds(30)
	while (-not (Test-Path $manifest) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
	if (-not (Test-Path $manifest)) { throw "DumpManifest wrote nothing to $manifest." }

	# The GUI silently falls back to its built-in manifest when this one doesn't parse.
	$parsed = Get-Content $manifest -Raw | ConvertFrom-Json
	Write-Host "Manifest: $manifest ($(@($parsed).Count) settings)"
}
