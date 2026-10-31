<#
.SYNOPSIS
	Builds every part of Rocksmith Audio Bridge in order, stages the installer payload and publishes the installer.

.DESCRIPTION
	The payload layout is InstallerNext/InstallerNext.Core/PayloadLayout.cs:

	  native\        xinput1_3.dll + .pdb          Mod/Build-ModDll.ps1 (upstream RSMods with Mod/ compiled in)
	  rsmods\        mods.manifest.json            dumped from that exact DLL, validated (loose, NOT in the configurator zip)
	  configurator\  RSModsGUI.zip + manifest      upstream's Build/Publish-Configurator.ps1 + New-ConfiguratorPayload.ps1
	  game\          ASIO driver x86 + x64 guest, DriverHost64, VST host, stompbox-capi.dll, PedalSkin\
	  helper\        RocksmithAudioBridge.Helper.exe + tools, ONNX Runtime, soxr, rswindowcapture
	  app\           the desktop app (App/), self-contained win-x64
	  notices\       LICENSE, NOTICE, THIRD-PARTY-NOTICES.txt and the licence texts they point at
	  tones\         StarterTones.zip (optional, -WithStarterTones; needs gh signed in while the tones repo is private)

	Everything is built from this one checkout in one run, because the driver's exports, the control pipe and the VST
	host's shared memory version must ship in lockstep with the DLL. build-info.txt in the payload records the commit
	and every file's SHA-256.

	Output (all under build\, which git ignores):
	  build\release\payload\            the staged payload
	  build\release\payload.zip         the same, zipped (embedded into the installer exe)
	  build\release\installer\RocksmithAudioBridge-Setup.exe   single-file, self-contained

	Nothing is copied into the game folder: every native build runs with -p:DeployToGame=false, and the mod DLL build
	never deploys (Mod/Build-ModDll.ps1).

.PARAMETER SkipModDll
	Use the existing build\mod\ outputs instead of running Mod/Build-ModDll.ps1.
.PARAMETER SkipNative
	Use the existing build\native\Release outputs (driver, DriverHost64, VST host) instead of building them.
.PARAMETER SkipConfigurator
	Use the existing External\RSMods\Build\Artifacts\installer payload instead of publishing upstream's configurator.
.PARAMETER SkipHelper
	Use the existing Helper\bin\Release\net472 output. The helper build needs the FretNet ONNX models:
	RSMODSPLUS_TOOLS_DIR (folder holding ml-training\models) or -p:MlModelPath / -p:MlBassModelPath.
.PARAMETER StageOnly
	All of the Skip switches: stage and package what is already built.
.PARAMETER WithStarterTones
	Fetch the pinned starter tones (scripts/AcquireStarterTones.ps1) into tones\.
.PARAMETER PluginsFolder
	A prepared Plugins folder (NeuralAmpModeler.vst3, Airwindows Consolidated.vst3, bundled-plugins.json, Notices\,
	SOURCE-AND-LICENSES.txt) staged as plugins\ and installed to <game>\RocksmithAudioBridge\Plugins\.
.PARAMETER NoInstaller
	Stop after the payload; do not publish the installer exe.
#>
[CmdletBinding()]
param(
	[ValidateSet('Release')]
	[string]$Configuration = 'Release',
	[switch]$SkipModDll,
	[switch]$SkipNative,
	[switch]$SkipConfigurator,
	[switch]$SkipHelper,
	[switch]$StageOnly,
	[switch]$WithStarterTones,
	[switch]$NoInstaller,
	[string]$PluginsFolder
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($StageOnly) { $SkipModDll = $SkipNative = $SkipConfigurator = $SkipHelper = $true }

$repo = Split-Path -Parent $PSScriptRoot
$upstream = Join-Path $repo 'External\RSMods'
$release = Join-Path $repo 'build\release'
$payload = Join-Path $release 'payload'
$nativeOut = Join-Path $repo 'build\native\Release'

function Invoke-Step([string]$name, [scriptblock]$action) {
	Write-Host "=== $name ===" -ForegroundColor Cyan
	$global:LASTEXITCODE = 0
	& $action
	if ($LASTEXITCODE -ne 0) { throw "$name failed (exit $LASTEXITCODE)." }
}

function Get-MSBuild {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	$found = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
	if (-not $found) { throw 'MSBuild not found (vswhere). Install Visual Studio 2022 with the C++ workload.' }
	return $found
}

function Copy-Into([string]$source, [string]$destination) {
	if (-not (Test-Path -LiteralPath $source)) { throw "Missing build output: $source" }
	New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
	Copy-Item -LiteralPath $source -Destination $destination -Force
}

function Copy-Folder([string]$source, [string]$destination, [string[]]$exclude = @('*.pdb', '*.ilk', '*.exp', '*.lib', '*.iobj', '*.ipdb')) {
	if (-not (Test-Path -LiteralPath $source)) { throw "Missing build output folder: $source" }
	New-Item -ItemType Directory -Force -Path $destination | Out-Null
	Get-ChildItem -LiteralPath $source -Recurse -File | Where-Object {
		$name = $_.Name; -not ($exclude | Where-Object { $name -like $_ })
	} | ForEach-Object {
		$relative = $_.FullName.Substring($source.TrimEnd('\').Length + 1)
		$target = Join-Path $destination $relative
		New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
		Copy-Item -LiteralPath $_.FullName -Destination $target -Force
	}
}

# ---- 0. inputs the builds embed -----------------------------------------------------------------------------------
Invoke-Step 'UsbDk MSI (pinned, embedded by App/RocksmithAudioBridge.Core)' { & (Join-Path $repo 'scripts\AcquireUsbDk.ps1') }

# ---- 1. the game DLL and its manifest -------------------------------------------------------------------------------
if (-not $SkipModDll) {
	Invoke-Step 'Mod DLL (upstream RSMods + Mod/) and manifest dump' { & (Join-Path $repo 'Mod\Build-ModDll.ps1') -Configuration $Configuration }
}
$modDll = Join-Path $repo 'build\mod\xinput1_3.dll'
$modManifest = Join-Path $repo 'build\mod\mods.manifest.json'
$special = (Get-Item -LiteralPath $modDll).VersionInfo.SpecialBuild
if ($special -notlike '*RocksmithAudioBridge*') { throw "build\mod\xinput1_3.dll is not our build (SpecialBuild '$special')." }

Invoke-Step 'Installer CLI (manifest validator)' {
	dotnet build (Join-Path $repo 'InstallerNext\InstallerNext.Cli\InstallerNext.Cli.csproj') -c $Configuration --nologo -v q
}
$cli = Join-Path $repo "InstallerNext\InstallerNext.Cli\bin\$Configuration\net8.0-windows\RocksmithAudioBridge.Setup.Cli.exe"
Invoke-Step 'Validate mods.manifest.json' { & $cli --validate-manifest $modManifest }

# ---- 2. upstream's configurator payload ----------------------------------------------------------------------------
if (-not $SkipConfigurator) {
	Invoke-Step 'Upstream configurator publish' { & (Join-Path $upstream 'Build\Publish-Configurator.ps1') -Configuration $Configuration }
	Invoke-Step 'Upstream configurator payload' { & (Join-Path $upstream 'Build\New-ConfiguratorPayload.ps1') }
}
$configuratorOut = Join-Path $upstream 'Build\Artifacts\installer'

# ---- 3. native pieces ----------------------------------------------------------------------------------------------
if (-not $SkipNative) {
	$msbuild = Get-MSBuild
	$common = @('-nologo', '-m', '-v:m', "-p:Configuration=$Configuration", '-p:DeployToGame=false')
	Invoke-Step 'ASIO driver x86' { & $msbuild (Join-Path $repo 'Driver\AsioProxy\RocksmithAudioBridge.vcxproj') @common '-p:Platform=Win32' }
	Invoke-Step 'ASIO driver x64 guest' { & $msbuild (Join-Path $repo 'Driver\AsioProxy\RocksmithAudioBridge.vcxproj') @common '-p:Platform=x64' }
	Invoke-Step 'DriverHost64' { & $msbuild (Join-Path $repo 'Driver\AsioProxy\DriverHost64\RocksmithAudioBridgeDriverHost.vcxproj') @common '-p:Platform=x64' }
	$stompbox = Join-Path $repo 'VstHost\Stompbox\Build-Stompbox.ps1'
	if ((Test-Path $stompbox) -and -not (Test-Path (Join-Path $repo 'VstHost\Stompbox\bin\stompbox-capi.dll'))) {
		Invoke-Step 'Stompbox engine (optional)' { & $stompbox }
	}
	Invoke-Step 'VST host (JUCE, AGPLv3)' { & $msbuild (Join-Path $repo 'VstHost\RocksmithAudioBridgeVstHost.vcxproj') @common '-p:Platform=x64' }
}

# ---- 4. helper exe ---------------------------------------------------------------------------------------------------
if (-not $SkipHelper) {
	if (-not $env:RSMODSPLUS_TOOLS_DIR) { Write-Warning 'RSMODSPLUS_TOOLS_DIR is not set: the helper build needs the FretNet ONNX models (pass -p:MlModelPath/-p:MlBassModelPath through Directory.Build.rsp, or set it).' }
	Invoke-Step 'Helper exe (net472 x64: ONNX Runtime, soxr, WindowCapture)' {
		dotnet build (Join-Path $repo 'Helper\Helper.csproj') -c $Configuration --nologo -v q
	}
}
$helperOut = Join-Path $repo "Helper\bin\$Configuration\net472"

# ---- 5. desktop app --------------------------------------------------------------------------------------------------
$appOut = Join-Path $release 'app-publish'
Invoke-Step 'Desktop app publish (self-contained win-x64)' {
	if (Test-Path $appOut) { Remove-Item -Recurse -Force $appOut }
	dotnet publish (Join-Path $repo 'App\RocksmithAudioBridge.App.csproj') -c $Configuration -r win-x64 --self-contained true `
		-p:DebugType=none -p:DebugSymbols=false -o $appOut --nologo
}

# ---- 6. stage the payload --------------------------------------------------------------------------------------------
Write-Host '=== Stage payload ===' -ForegroundColor Cyan
if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
New-Item -ItemType Directory -Force -Path $payload | Out-Null

Copy-Into $modDll (Join-Path $payload 'native\xinput1_3.dll')
Copy-Into (Join-Path $repo 'build\mod\xinput1_3.pdb') (Join-Path $payload 'native\xinput1_3.pdb')
Copy-Into $modManifest (Join-Path $payload 'rsmods\mods.manifest.json')
Copy-Into (Join-Path $configuratorOut 'RSModsGUI.zip') (Join-Path $payload 'configurator\RSModsGUI.zip')
Copy-Into (Join-Path $configuratorOut 'RSModsGUI.manifest.json') (Join-Path $payload 'configurator\RSModsGUI.manifest.json')
# The loose manifest must never be part of the configurator payload: a later official install deletes every file the
# previous payload listed that its own lacks.
$configuratorManifest = Get-Content -Raw (Join-Path $payload 'configurator\RSModsGUI.manifest.json') | ConvertFrom-Json
if ($configuratorManifest.files | Where-Object { $_.path -eq 'mods.manifest.json' -or $_.path -like 'AudioBridge/*' }) {
	throw 'The configurator payload lists mods.manifest.json or AudioBridge/: keep our files out of it.'
}

foreach ($file in 'RocksmithAudioBridgeAsio.dll', 'RocksmithAudioBridgeAsio64.dll', 'RocksmithAudioBridgeDriverHost.exe', 'RocksmithAudioBridgeVstHost.exe') {
	Copy-Into (Join-Path $nativeOut $file) (Join-Path $payload "game\$file")
}
if (Test-Path (Join-Path $nativeOut 'stompbox-capi.dll')) { Copy-Into (Join-Path $nativeOut 'stompbox-capi.dll') (Join-Path $payload 'game\stompbox-capi.dll') }
if (Test-Path (Join-Path $nativeOut 'PedalSkin')) { Copy-Folder (Join-Path $nativeOut 'PedalSkin') (Join-Path $payload 'game\PedalSkin') }
# The bundled NAM and Airwindows VST3 plug-ins (separate files under their own licences, with bundled-plugins.json and
# SOURCE-AND-LICENSES.txt) go to <game>\RocksmithAudioBridge\Plugins\. TODO: they have no build or download step in this
# repo yet; until they do, pass the prepared folder with -PluginsFolder.
if ($PluginsFolder) { Copy-Folder $PluginsFolder (Join-Path $payload 'plugins') @() }
else { Write-Warning 'No -PluginsFolder: the payload carries no bundled VST3 plug-ins.' }

Copy-Folder $helperOut (Join-Path $payload 'helper') @('*.pdb', '*.xml')
Copy-Folder $appOut (Join-Path $payload 'app') @('*.pdb')

$notices = Join-Path $payload 'notices'
Copy-Into (Join-Path $repo 'LICENSE') (Join-Path $notices 'LICENSE.txt')
Copy-Into (Join-Path $repo 'NOTICE') (Join-Path $notices 'NOTICE.txt')
Copy-Into (Join-Path $repo 'THIRD-PARTY-NOTICES.txt') (Join-Path $notices 'THIRD-PARTY-NOTICES.txt')
$licences = @{
	'ThirdParty\VstHost\LICENSE.txt'                    = 'VstHost\LICENSE.txt'
	'ThirdParty\VstHost\JUCE-LICENSE.txt'               = 'VstHost\Lib\JUCE\LICENSE.txt'
	'ThirdParty\VstHost\THIRD-PARTY-NOTICES.txt'        = 'VstHost\THIRD-PARTY-NOTICES.txt'
	'ThirdParty\VstHost\Tone3000-LICENSE.txt'           = 'VstHost\Lib\Tone3000\LICENSE.txt'
	'ThirdParty\VstHost\NeuralAmpModelerCore-LICENSE.txt' = 'VstHost\Lib\Tone3000\NeuralAmpModelerCore\LICENSE.txt'
	'ThirdParty\VstHost\AudioDSPTools-LICENSE.txt'      = 'VstHost\Lib\Tone3000\AudioDSPTools\LICENSE.txt'
	'ThirdParty\VstHost\Eigen-COPYING.MPL2.txt'         = 'VstHost\Lib\Tone3000\NeuralAmpModelerCore\Dependencies\eigen\COPYING.MPL2'
	'ThirdParty\Stompbox\LICENSE.txt'                   = 'VstHost\Stompbox\LICENSE.txt'
	'ThirdParty\Stompbox\THIRD-PARTY-NOTICES.txt'       = 'VstHost\Stompbox\THIRD-PARTY-NOTICES.txt'
	'ThirdParty\SignalsmithStretch\LICENSE.txt'         = 'Mod\ThirdParty\SignalsmithStretch\LICENSE.txt'
	'ThirdParty\SignalsmithStretch\signalsmith-linear-LICENSE.txt' = 'Mod\ThirdParty\SignalsmithStretch\signalsmith-linear\LICENSE.txt'
	'ThirdParty\VexFlow\LICENSE.txt'                    = 'Mod\NoteByNote\Ui\ThirdParty\VexFlow-LICENSE.txt'
	'ThirdParty\UsbDk\LICENSE.txt'                      = 'build\resources\UsbDk\LICENSE.txt'
	'ThirdParty\soxr\COPYING.LGPL'                      = 'RSModsPlus\Native\soxr\COPYING.LGPL'
	'ThirdParty\soxr\LICENCE'                           = 'RSModsPlus\Native\soxr\LICENCE'
}
foreach ($entry in $licences.GetEnumerator()) { Copy-Into (Join-Path $repo $entry.Value) (Join-Path $notices $entry.Key) }
$onnx = Join-Path $env:USERPROFILE '.nuget\packages\microsoft.ml.onnxruntime\1.24.4'
if ($env:NUGET_PACKAGES) { $onnx = Join-Path $env:NUGET_PACKAGES 'microsoft.ml.onnxruntime\1.24.4' }
Copy-Into (Join-Path $onnx 'LICENSE') (Join-Path $notices 'ThirdParty\onnxruntime\LICENSE.txt')
Copy-Into (Join-Path $onnx 'ThirdPartyNotices.txt') (Join-Path $notices 'ThirdParty\onnxruntime\ThirdPartyNotices.txt')

if ($WithStarterTones) {
	Invoke-Step 'Starter tones (pinned release)' { & (Join-Path $repo 'scripts\AcquireStarterTones.ps1') }
	Copy-Into (Join-Path $repo 'build\resources\Tones\StarterTones.zip') (Join-Path $payload 'tones\StarterTones.zip')
}

# Lockstep record: one commit, every file's hash.
$commit = (& git -C $repo rev-parse HEAD 2>$null)
$lines = @("commit $commit", "built $((Get-Date).ToUniversalTime().ToString('u'))", "dll SpecialBuild $special")
$lines += Get-ChildItem -Recurse -File $payload | Sort-Object FullName | ForEach-Object {
	"$((Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash)  $($_.FullName.Substring($payload.Length + 1))"
}
[IO.File]::WriteAllLines((Join-Path $payload 'app\build-info.txt'), $lines)

Invoke-Step 'Dry run of the staged payload against an empty game folder' {
	$fake = Join-Path $release 'dry-run-game'
	if (Test-Path $fake) { Remove-Item -Recurse -Force $fake }
	New-Item -ItemType Directory -Force -Path (Join-Path $fake 'dlc') | Out-Null
	Set-Content -Path (Join-Path $fake 'dlc\x_p.psarc') -Value 'x'
	Set-Content -Path (Join-Path $fake 'cache.psarc') -Value 'x'
	& $cli --print-plan --game $fake --payload $payload | Select-String -Pattern 'BLOCKER' | ForEach-Object { throw "Payload incomplete: $_" }
	$global:LASTEXITCODE = 0
	Remove-Item -Recurse -Force $fake
}

$zip = Join-Path $release 'payload.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($payload, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
Write-Host "Payload: $payload ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB zipped)" -ForegroundColor Green

# ---- 7. installer ------------------------------------------------------------------------------------------------------
if (-not $NoInstaller) {
	$installerOut = Join-Path $release 'installer'
	Invoke-Step 'Installer publish (single file, payload embedded)' {
		if (Test-Path $installerOut) { Remove-Item -Recurse -Force $installerOut }
		dotnet publish (Join-Path $repo 'InstallerNext\InstallerNext.App\InstallerNext.App.csproj') -c $Configuration -r win-x64 --self-contained true `
			-p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=none -p:DebugSymbols=false `
			"-p:PayloadZip=$zip" -o $installerOut --nologo
	}
	Get-ChildItem $installerOut -File | Where-Object Name -ne 'RocksmithAudioBridge-Setup.exe' | Remove-Item -Force
	# TODO(signing): sign RocksmithAudioBridge-Setup.exe, the app and the native binaries here when a certificate exists
	# (signtool sign /fd sha256 /tr <timestamp url> ...). No paid signing today.
	Write-Host "Installer: $(Join-Path $installerOut 'RocksmithAudioBridge-Setup.exe')" -ForegroundColor Green
}
