$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$resourceFile = Join-Path $repositoryRoot 'Installer\Properties\Resources.resx'
$resourceXml = [xml](Get-Content -LiteralPath $resourceFile -Raw)

function Get-ResourcePath([string]$name) {
    $resource = $resourceXml.Root.data | Where-Object { $_.name -eq $name }
    if ($null -eq $resource) { throw "Missing resource declaration: $name" }
    $relativePath = ([string]$resource.value).Split(';', 2)[0]
    return [IO.Path]::GetFullPath((Join-Path (Split-Path $resourceFile) $relativePath))
}

function Get-PeMachine([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Not a PE image: $path"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 60)
    if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x4550) {
        throw "Invalid PE image: $path"
    }
    return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

$expected = @{
    xinput1_3 = Join-Path $repositoryRoot 'Installer\Resources\Master\xinput1_3.dll'
    RocksmithAudioBridge = Join-Path $repositoryRoot 'Installer\Resources\Release\RocksmithAudioBridgeAsio.dll'
    RocksmithAudioBridgeGuest64 = Join-Path $repositoryRoot 'Installer\Resources\Release\RocksmithAudioBridgeAsio64.dll'
    RocksmithAudioBridgeLibrary = Join-Path $repositoryRoot 'Installer\Resources\RSModsGUI\RocksmithAudioBridge.dll'
}

foreach ($name in $expected.Keys) {
    $referenced = Get-ResourcePath $name
    $authoritative = [IO.Path]::GetFullPath($expected[$name])
    if ($referenced -ne $authoritative) {
        throw "$name resource points to '$referenced', expected '$authoritative'."
    }
    if (-not (Test-Path -LiteralPath $authoritative -PathType Leaf)) {
        throw "Authoritative package input is missing: $authoritative"
    }
    # Files loaded by the 32-bit game are x86; the managed runtime is loaded by the 64-bit RSMods.exe and the
    # External amp guest driver by 64-bit amp sims.
    $machine = if ($name -eq 'RocksmithAudioBridgeLibrary' -or $name -eq 'RocksmithAudioBridgeGuest64') { 0x8664 } else { 0x14C }
    if ((Get-PeMachine $authoritative) -ne $machine) {
        throw ("{0} must be a PE image for machine 0x{1:X}: {2}" -f $name, $machine, $authoritative)
    }
    $hash = (Get-FileHash -LiteralPath $authoritative -Algorithm SHA256).Hash
    Write-Output ("PASS: {0} -> {1} ({2})" -f $name, $authoritative, $hash)
}

# Installer\Resources\RocksmithAudioBridge.dll is the managed runtime (RSMods GUI build output), not a
# proxy copy, so it is not compared against the proxy.
$legacyInputs = @(
    (Join-Path $repositoryRoot 'Installer\Resources\xinput1_3.dll')
)
foreach ($legacyInput in $legacyInputs) {
    if (Test-Path -LiteralPath $legacyInput -PathType Leaf) {
        $legacyHash = (Get-FileHash -LiteralPath $legacyInput -Algorithm SHA256).Hash
        $authoritativeName = [IO.Path]::GetFileName($legacyInput)
        $authoritativePath = if ($authoritativeName -eq 'xinput1_3.dll') { $expected.xinput1_3 } else { $expected.RocksmithAudioBridge }
        $authoritativeHash = (Get-FileHash -LiteralPath $authoritativePath -Algorithm SHA256).Hash
        if ($legacyHash -ne $authoritativeHash) {
            Write-Warning "Stale legacy package input is ignored by Resources.resx: $legacyInput differs from $authoritativePath."
            continue
        }
        Write-Warning "Legacy package input remains but matches the authoritative output: $legacyInput"
    }
}

# Every GUI configuration builds into Installer\Resources\RSModsGUI, the folder the installer embeds, so a
# Debug build followed by an installer-only build would package a Debug RSMods.exe. The managed files the
# installer ships must be optimized (Release) builds: a Debug build carries DebuggableAttribute with
# DisableOptimizations (0x100).
foreach ($managed in @('Installer\Resources\RSModsGUI\RSMods.exe', 'Installer\Resources\RSModsGUI\RocksmithAudioBridge.dll')) {
    $managedPath = Join-Path $repositoryRoot $managed
    if (-not (Test-Path -LiteralPath $managedPath -PathType Leaf)) { throw "Authoritative package input is missing: $managedPath" }
    $assembly = [Reflection.Assembly]::ReflectionOnlyLoadFrom($managedPath)
    $debuggable = [Reflection.CustomAttributeData]::GetCustomAttributes($assembly) |
        Where-Object { $_.AttributeType.FullName -eq 'System.Diagnostics.DebuggableAttribute' } | Select-Object -First 1
    $modes = if ($debuggable) { [int]$debuggable.ConstructorArguments[0].Value } else { 0 }
    if ($modes -band 0x100) {
        throw "$managed is a Debug build (optimizations disabled). Build the Master or Release configuration before packaging."
    }
    Write-Output ("PASS: {0} is an optimized build" -f $managed)
}
