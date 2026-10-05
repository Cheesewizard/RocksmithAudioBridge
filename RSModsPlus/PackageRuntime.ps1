param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$destinationPath = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destinationPath) { throw 'Choose a new output directory; existing packages are never overwritten.' }
$gui = Join-Path $repo 'Installer/Resources/RSModsGUI'
$hostFile = Join-Path $repo 'Installer/Resources/Master/xinput1_3.dll'
$files = @{
    'xinput1_3.dll' = $hostFile
    'RocksmithAudioBridgeAsio.dll' = Join-Path $repo 'Installer/Resources/Release/RocksmithAudioBridgeAsio.dll'
    'RSMods.exe' = Join-Path $gui 'RSMods.exe'
    'RocksmithAudioBridge.dll' = Join-Path $gui '../RocksmithAudioBridge.dll'
}
foreach ($file in $files.Values) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing public runtime build: $file" }
}
foreach ($name in @('RSMods.exe', 'RocksmithAudioBridge.dll')) {
    $bytes = [IO.File]::ReadAllBytes($files[$name])
    if ($bytes.Length -lt 64) { throw "Invalid runtime executable: $name" }
    $header = [BitConverter]::ToInt32($bytes, 60)
    if ($header -lt 0 -or $header -gt $bytes.Length - 6 -or
        [BitConverter]::ToUInt32($bytes, $header) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $header + 4) -ne 0x8664) {
        throw "The GUI and managed runtime must both be x64: $name"
    }
}
$hostBytes = [IO.File]::ReadAllBytes($hostFile)
foreach ($encoding in @([Text.Encoding]::ASCII,[Text.Encoding]::Unicode)) {
    $text = $encoding.GetString($hostBytes)
    foreach ($forbidden in @('RSModsPlus.Research','RocksmithAudioBridgeDebug.dll','reload_probe','write_memory','HeapValidate','NBN HEAP CHECK','early-tick-')) {
        if ($text.Contains($forbidden)) { throw "Public host contains developer functionality: $forbidden" }
    }
}
$proxyBytes = [IO.File]::ReadAllBytes($files['RocksmithAudioBridgeAsio.dll'])
if ($proxyBytes.Length -lt 64) { throw 'Invalid Audio Bridge proxy: RocksmithAudioBridgeAsio.dll' }
$proxyHeader = [BitConverter]::ToInt32($proxyBytes, 60)
if ($proxyHeader -lt 0 -or $proxyHeader -gt $proxyBytes.Length - 6 -or
    [BitConverter]::ToUInt32($proxyBytes, $proxyHeader) -ne 0x4550 -or
    [BitConverter]::ToUInt16($proxyBytes, $proxyHeader + 4) -ne 0x14c) {
    throw 'The Audio Bridge proxy must be a valid x86 DLL: RocksmithAudioBridgeAsio.dll'
}
New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
foreach ($name in $files.Keys) { Copy-Item -LiteralPath $files[$name] -Destination (Join-Path $destinationPath $name) }
Write-Output $destinationPath
