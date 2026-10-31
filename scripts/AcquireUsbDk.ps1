$ErrorActionPreference = 'Stop'

$packageDirectory = Join-Path $PSScriptRoot '..\build\resources\UsbDk'
$packagePath = Join-Path $packageDirectory 'UsbDk_1.0.22_x64.msi'
$licensePath = Join-Path $packageDirectory 'LICENSE.txt'
$expectedHash = '91F6F695E1E13C656024E6D3B55620BF08D8835EF05EE0496935BA6BB62466A5'

function Get-Sha256([string]$path) {
	$stream = [IO.File]::OpenRead($path)
	$sha = [Security.Cryptography.SHA256]::Create()
	try {
		return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
	} finally {
		$stream.Dispose()
		$sha.Dispose()
	}
}

# Downloads go to a .tmp file and are moved into place only when complete, so a broken download is never
# taken for the package on the next build.
function Save-Download([Net.WebClient]$client, [string]$url, [string]$path, [string]$hash) {
	$temporaryPath = $path + '.tmp'
	if ([IO.File]::Exists($temporaryPath)) { [IO.File]::Delete($temporaryPath) }
	try {
		$client.DownloadFile($url, $temporaryPath)
		if ($hash -and (Get-Sha256 $temporaryPath) -ne $hash) { throw 'The UsbDk package does not match the pinned official release.' }
		[IO.File]::Move($temporaryPath, $path)
	} finally {
		if ([IO.File]::Exists($temporaryPath)) { [IO.File]::Delete($temporaryPath) }
	}
}

[IO.Directory]::CreateDirectory($packageDirectory) | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$client = New-Object Net.WebClient
try {
	if (-not [IO.File]::Exists($packagePath)) {
		Save-Download $client 'https://github.com/daynix/UsbDk/releases/download/v1.00-22/UsbDk_1.0.22_x64.msi' $packagePath $expectedHash
	}
	if ((Get-Sha256 $packagePath) -ne $expectedHash) { throw 'The UsbDk package does not match the pinned official release.' }
	if (-not [IO.File]::Exists($licensePath)) {
		Save-Download $client 'https://raw.githubusercontent.com/daynix/UsbDk/v1.00-22/LICENSE' $licensePath $null
	}
} finally {
	$client.Dispose()
}
