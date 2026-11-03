param([switch]$Force)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$headerDirectory = Join-Path $projectRoot 'third_party/miniaudio'
$header = Join-Path $headerDirectory 'miniaudio.h'
$expected = '7E4F3F13C8FE66DF2080AC3DD12A89193E3C2463CB7F067C798ABD7331CD8EE6'
New-Item -ItemType Directory -Force -Path $headerDirectory | Out-Null
if ($Force -or !(Test-Path -LiteralPath $header)) {
    Invoke-WebRequest 'https://raw.githubusercontent.com/mackron/miniaudio/0.11.23/miniaudio.h' -OutFile $header
}
$headerStream = [IO.File]::OpenRead($header)
$hasher = [Security.Cryptography.SHA256]::Create()
try { $actual = [BitConverter]::ToString($hasher.ComputeHash($headerStream)).Replace('-','') }
finally { $hasher.Dispose(); $headerStream.Dispose() }
if ($actual -ne $expected) {
    throw 'miniaudio checksum mismatch. Run bootstrap.ps1 -Force to fetch the pinned official header.'
}
Write-Output 'miniaudio 0.11.23 is ready.'
