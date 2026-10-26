# Fetches the pinned release of the Tone designer's starter tones (StarterTones.json) into Resources\Tones, where the
# installer embeds it (StarterTones.cs). The zip is checked against its pinned SHA-256; a mismatch stops the build.
# Needs the GitHub CLI (gh), signed in, while Cheesewizard/RocksmithAudioBridge-Tones is private.
$ErrorActionPreference = 'Stop'

$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'StarterTones.json') -Raw | ConvertFrom-Json
$folder = Join-Path $PSScriptRoot 'Resources\Tones'
$target = Join-Path $folder 'StarterTones.zip'
if (-not $pin.sha256) { throw "StarterTones.json pins no SHA-256 for $($pin.tag) yet: set it from the release's .sha256 file." }

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

if ([IO.File]::Exists($target) -and (Get-Sha256 $target) -eq $pin.sha256) { exit 0 }

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
	throw "The starter tones ($($pin.tag)) are not in $folder. Install the GitHub CLI (winget install GitHub.cli), run gh auth login, and build again."
}
[IO.Directory]::CreateDirectory($folder) | Out-Null
$download = Join-Path $folder 'download.tmp'
if ([IO.Directory]::Exists($download)) { [IO.Directory]::Delete($download, $true) }
try {
	& gh release download $pin.tag -R $pin.repository -p $pin.asset -D $download
	if ($LASTEXITCODE -ne 0) { throw "gh could not download $($pin.asset) from $($pin.repository) $($pin.tag)." }
	$zip = Join-Path $download $pin.asset
	$hash = Get-Sha256 $zip
	if ($hash -ne $pin.sha256) { throw "$($pin.asset) has SHA-256 $hash, not the pinned $($pin.sha256). Nothing was changed." }
	if ([IO.File]::Exists($target)) { [IO.File]::Delete($target) }
	[IO.File]::Move($zip, $target)
} finally {
	if ([IO.Directory]::Exists($download)) { [IO.Directory]::Delete($download, $true) }
}
Write-Output "Starter tones $($pin.tag) -> $target"
