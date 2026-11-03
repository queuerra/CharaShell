param(
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [Parameter(Mandatory=$true)][string]$RuntimeDir,
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [Parameter(Mandatory=$true)][string]$QtPrefix,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
foreach ($pattern in @('llama.dll','ggml*.dll','libomp.dll','cudart64*.dll','cublas64*.dll','cublasLt64*.dll')) {
    Get-ChildItem -Path (Join-Path $RuntimeDir $pattern) -ErrorAction SilentlyContinue | Copy-Item -Destination $OutputDir -Force
}
$mode = '--release'
if ($Configuration -eq 'Debug') { $mode = '--debug' }
& (Join-Path $QtPrefix 'bin/windeployqt.exe') $mode --qmldir (Join-Path $ProjectRoot 'qml') (Join-Path $OutputDir 'charashell.exe')
if ($LASTEXITCODE) { throw 'Qt runtime deployment failed.' }
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'scripts') -Destination $OutputDir -Recurse -Force
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'LICENSE') -Destination $OutputDir -Force
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'resources/native-rig-NOTICE.txt') -Destination $OutputDir -Force
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'resources/icons/lucide/LICENSE') -Destination (Join-Path $OutputDir 'lucide-LICENSE.txt') -Force
$offscreen = Join-Path $QtPrefix 'plugins/platforms/qoffscreen.dll'
if ($Configuration -eq 'Debug') { $offscreen = Join-Path $QtPrefix 'plugins/platforms/qoffscreend.dll' }
if (Test-Path -LiteralPath $offscreen) { Copy-Item -LiteralPath $offscreen -Destination (Join-Path $OutputDir 'platforms') -Force }
$assets = Join-Path $OutputDir 'assets'
$sharedAssets = Join-Path $RuntimeDir 'assets'
New-Item -ItemType Directory -Force -Path $sharedAssets | Out-Null
if (!(Test-Path -LiteralPath $assets)) { New-Item -ItemType Junction -Path $assets -Target $sharedAssets | Out-Null }
$avatarDirectory = Join-Path $assets 'avatar'
New-Item -ItemType Directory -Force -Path $avatarDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'resources/avatar/source.psd') -Destination (Join-Path $avatarDirectory 'default.psd') -Force
