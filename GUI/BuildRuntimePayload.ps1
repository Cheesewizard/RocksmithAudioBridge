param([string]$References,[string]$Output,[string]$ProjectDirectory)
$ErrorActionPreference='Stop'
$ProjectDirectory=[IO.Path]::GetFullPath($ProjectDirectory).TrimEnd('\','/')
Add-Type -AssemblyName System.IO.Compression
$repo=Split-Path $ProjectDirectory
$files=@{}
Get-ChildItem (Join-Path $ProjectDirectory 'Lib') -Recurse -File | Where-Object { $_.Extension -ne '.pdb' } | ForEach-Object {
	$relative=$_.FullName.Substring((Join-Path $ProjectDirectory 'Lib').Length+1).Replace('\','/')
	$files[$relative]=$_.FullName
}
foreach ($file in [IO.File]::ReadAllLines($References)) {
	if ([IO.Path]::GetFileName($file) -ne 'RocksmithAudioBridge.dll' -and [IO.Path]::GetExtension($file) -eq '.dll') { $files[[IO.Path]::GetFileName($file)]=$file }
}
$onnx=Join-Path $env:USERPROFILE '.nuget/packages/microsoft.ml.onnxruntime/1.24.4'
Get-ChildItem (Join-Path $onnx 'runtimes/win-x64/native') -Filter '*.dll' | ForEach-Object { $files[$_.Name]=$_.FullName }
$files['soxr.dll']=Join-Path $repo 'RSModsPlus/Native/bin/soxr.dll'
$files['rswindowcapture.dll']=Join-Path $ProjectDirectory 'Native/bin/rswindowcapture.dll'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$version=Get-ChildItem (Join-Path $installation 'VC/Redist/MSVC') -Directory | Where-Object Name -Match '^\d' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
Get-ChildItem (Join-Path $version.FullName 'x64/Microsoft.VC143.CRT') -Filter '*.dll' | ForEach-Object { $files[$_.Name]=$_.FullName }
$files['ThirdParty/onnxruntime/LICENSE']=Join-Path $onnx 'LICENSE'
$files['ThirdParty/onnxruntime/ThirdPartyNotices.txt']=Join-Path $onnx 'ThirdPartyNotices.txt'
$files['ThirdParty/soxr/COPYING.LGPL']=Join-Path $repo 'RSModsPlus/Native/soxr/COPYING.LGPL'
$files['ThirdParty/soxr/LICENCE']=Join-Path $repo 'RSModsPlus/Native/soxr/LICENCE'
$stream=[IO.File]::Create($Output)
try {
	$zip=New-Object IO.Compression.ZipArchive($stream,[IO.Compression.ZipArchiveMode]::Create,$true)
	try {
		foreach ($name in ($files.Keys | Sort-Object)) {
			$entry=$zip.CreateEntry($name,[IO.Compression.CompressionLevel]::Optimal)
			$entry.LastWriteTime=[DateTimeOffset]::new(2026,1,1,0,0,0,[TimeSpan]::Zero)
			$inputStream=[IO.File]::OpenRead($files[$name]); $outputStream=$entry.Open()
			try { $inputStream.CopyTo($outputStream) } finally { $inputStream.Dispose(); $outputStream.Dispose() }
		}
	} finally { $zip.Dispose() }
} finally { $stream.Dispose() }
