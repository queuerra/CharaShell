param(
    [string]$AssetRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-dist/assets'),
    [ValidateSet('cuda','cpu')][string]$Backend = 'cuda'
)
$ErrorActionPreference = 'Stop'
if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { throw 'Irodori C++ runtime currently requires Windows x64.' }
& (Join-Path $PSScriptRoot 'download.ps1') -Kind Models -Ids tts -Architecture x64 -Backend $Backend -AssetRoot $AssetRoot
Write-Output "READY Irodori-TTS-v4.1-Small-MF C++: $AssetRoot/irodori"
