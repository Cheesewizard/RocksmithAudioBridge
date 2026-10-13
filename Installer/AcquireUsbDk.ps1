$ErrorActionPreference = 'Stop'

$packageDirectory = Join-Path $PSScriptRoot 'Resources\UsbDk'
$packagePath = Join-Path $packageDirectory 'UsbDk_1.0.22_x64.msi'
$licensePath = Join-Path $packageDirectory 'LICENSE.txt'
$expectedHash = '91F6F695E1E13C656024E6D3B55620BF08D8835EF05EE0496935BA6BB62466A5'

[IO.Directory]::CreateDirectory($packageDirectory) | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$client = New-Object Net.WebClient
try {
	if (-not [IO.File]::Exists($packagePath)) {
		$client.DownloadFile('https://github.com/daynix/UsbDk/releases/download/v1.00-22/UsbDk_1.0.22_x64.msi', $packagePath)
	}
	$stream = [IO.File]::OpenRead($packagePath)
	$sha = [Security.Cryptography.SHA256]::Create()
	try {
		$actualHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
	} finally {
		$stream.Dispose()
		$sha.Dispose()
	}
	if ($actualHash -ne $expectedHash) { throw 'The UsbDk package does not match the pinned official release.' }
	if (-not [IO.File]::Exists($licensePath)) {
		$client.DownloadFile('https://raw.githubusercontent.com/daynix/UsbDk/v1.00-22/LICENSE', $licensePath)
	}
} finally {
	$client.Dispose()
}
