param(
    [Parameter(Mandatory=$true)][string]$QtPrefix,
    [string]$BuildRoot = (Join-Path $env:TEMP 'CharaShellQmake'),
    [switch]$HttpOnly,
    [switch]$WithoutAsr,
    [switch]$Cuda,
    [string]$RuntimeDir = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if ($BuildRoot -match '[^\x00-\x7F]') { throw 'BuildRoot must be an ASCII path.' }
if (!(Get-Command nmake -ErrorAction SilentlyContinue)) { throw 'Run in Visual Studio Developer PowerShell matching Qt architecture.' }
$architecture = 'x64'
if ($QtPrefix -match 'arm64') { $architecture = 'arm64' }
if ($Cuda -and $architecture -ne 'x64') { throw 'The pinned CUDA runtime requires an x64 Qt kit.' }
$backend = 'cpu'; $llamaDirectory = 'llama'
if ($Cuda) { $backend = 'cuda'; $llamaDirectory = 'llama-cuda' }
& (Join-Path $PSScriptRoot 'bootstrap.ps1')
$runtime = Join-Path $projectRoot 'build-dist'
if ($RuntimeDir) { $runtime = [IO.Path]::GetFullPath($RuntimeDir) }
New-Item -ItemType Directory -Force -Path $runtime | Out-Null
$modelAssets = Join-Path $projectRoot 'build-dist/assets'
New-Item -ItemType Directory -Force -Path $modelAssets | Out-Null
if ($RuntimeDir -and !(Test-Path -LiteralPath (Join-Path $runtime 'assets'))) {
    New-Item -ItemType Junction -Path (Join-Path $runtime 'assets') -Target $modelAssets | Out-Null
}
$runtimeAssets = Join-Path $BuildRoot 'runtime-assets'
if (!$HttpOnly) {
    & (Join-Path $PSScriptRoot 'native.ps1')
    & (Join-Path $PSScriptRoot 'prepare-irodori.ps1') -AssetRoot $modelAssets -Backend $backend
    & (Join-Path $PSScriptRoot 'download.ps1') -Kind Servers -Ids llama -Backend $backend -Architecture $architecture -AssetRoot $runtimeAssets
    Get-ChildItem -LiteralPath (Join-Path $runtimeAssets $llamaDirectory) -Filter '*.dll' | Copy-Item -Destination $runtime -Force
    if ($Cuda) {
        if (!$env:CUDA_PATH) { throw 'CUDA_PATH is required to deploy the CUDA toolkit runtime.' }
        foreach ($pattern in @('cudart64*.dll','cublas64*.dll','cublasLt64*.dll')) {
            Get-ChildItem -Path (Join-Path $env:CUDA_PATH "bin/$pattern") | Copy-Item -Destination $runtime -Force
        }
    }
}
if (!$WithoutAsr) {
    if ($HttpOnly) { & (Join-Path $PSScriptRoot 'native.ps1') }
    & (Join-Path $PSScriptRoot 'download.ps1') -Kind Models -Ids asr -Architecture $architecture -Backend $backend -AssetRoot $modelAssets
}
$runtimeConfig = Join-Path $projectRoot 'third_party/qtcreator-runtime.pri'
$configLines = @('isEmpty(CHARASHELL_RUNTIME_DIR): CHARASHELL_RUNTIME_DIR = "' + $runtime.Replace('\','/') + '"')
$configLines | Set-Content -LiteralPath $runtimeConfig -Encoding UTF8
$alias = Join-Path $BuildRoot 'project'
if (Test-Path -LiteralPath $alias) {
    $item = Get-Item -LiteralPath $alias -Force
    if ($item.LinkType -ne 'Junction' -or [IO.Path]::GetFullPath([string]$item.Target) -ne [IO.Path]::GetFullPath($projectRoot)) { throw 'Existing project alias points elsewhere.' }
} else { New-Item -ItemType Junction -Path $alias -Target $projectRoot | Out-Null }
$build = Join-Path $BuildRoot 'qmake-release'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$qmakeArgs = @((Join-Path $alias 'CharaShell.pro'),'CONFIG+=release','CONFIG-=debug',"CHARASHELL_RUNTIME_DIR=$($runtime.Replace('\','/'))")
if ($HttpOnly) { $qmakeArgs += 'CONFIG+=http_only' }
if ($WithoutAsr) { $qmakeArgs += 'CONFIG+=without_asr' }
Push-Location $build
try {
    & (Join-Path $QtPrefix 'bin/qmake.exe') @qmakeArgs
    if ($LASTEXITCODE) { throw 'qmake failed.' }
    & nmake /nologo
    if ($LASTEXITCODE) { throw 'qmake application build failed.' }
} finally { Pop-Location }
Copy-Item -LiteralPath (Join-Path $build 'charashell.exe') -Destination $runtime -Force
Write-Output "Open in Qt Creator: $alias/CharaShell.pro"
Write-Output "Release build directory: $build"
Write-Output "Ready: $build/charashell.exe"
