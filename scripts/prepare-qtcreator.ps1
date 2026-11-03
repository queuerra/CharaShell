param(
    [Parameter(Mandatory=$true)][string]$QtPrefix,
    [string]$BuildRoot = (Join-Path $env:TEMP 'CharaShellQmake'),
    [switch]$Cuda,
    [string]$RuntimeDir = ''
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build-qmake.ps1') -QtPrefix $QtPrefix -BuildRoot $BuildRoot -Cuda:$Cuda -RuntimeDir $RuntimeDir
Write-Output 'Select the matching MSVC / Qt kit and the Release configuration. Run target: charashell.'
Write-Output 'The project junction edits the original workspace, not a copied source tree.'
