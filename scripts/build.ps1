param(
    [Parameter(Mandatory=$true)][string]$QtPrefix,
    [string]$BuildRoot = (Join-Path $env:TEMP 'CharaShellBuild'),
    [switch]$WithoutAsr,
    [switch]$HttpOnly,
    [switch]$Cuda
)
# Run in a Visual Studio Developer PowerShell matching the Qt architecture.
# Stage sources into an ASCII path to avoid Qt tool path-encoding problems.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if ($BuildRoot -match '[^\x00-\x7F]') { throw 'BuildRoot must be an ASCII path.' }
if (!(Get-Command cl -ErrorAction SilentlyContinue)) { throw 'Run this script in Visual Studio Developer PowerShell.' }
& (Join-Path $PSScriptRoot 'bootstrap.ps1')
$architecture = 'x64'
if ($QtPrefix -match 'arm64') { $architecture = 'arm64' }
if ($Cuda -and $architecture -ne 'x64') { throw 'The pinned CUDA runtime requires an x64 Qt kit.' }
$backend = 'cpu'; $llamaDirectory = 'llama'
if ($Cuda) { $backend = 'cuda'; $llamaDirectory = 'llama-cuda' }
if (!$HttpOnly) {
    & (Join-Path $PSScriptRoot 'native.ps1')
    & (Join-Path $PSScriptRoot 'prepare-irodori.ps1') -AssetRoot (Join-Path $projectRoot 'build-dist/assets') -Backend $backend
    & (Join-Path $PSScriptRoot 'download.ps1') -Kind Servers -Ids llama -Backend $backend -Architecture $architecture -AssetRoot (Join-Path $projectRoot 'build-dist/assets')
}
if (!$WithoutAsr) {
    if ($HttpOnly) { & (Join-Path $PSScriptRoot 'native.ps1') }
    & (Join-Path $PSScriptRoot 'download.ps1') -Kind Models -Ids asr -Architecture $architecture -Backend $backend -AssetRoot (Join-Path $projectRoot 'build-dist/assets')
}
$source = Join-Path $BuildRoot 'source'
$build = Join-Path $BuildRoot 'build'
New-Item -ItemType Directory -Force -Path $source | Out-Null
foreach ($entry in @('CMakeLists.txt','LICENSE','include','src','qml','resources','third_party/miniaudio')) {
    if ($entry -eq 'third_party/miniaudio') {
        $dependencyRoot = Join-Path $source 'third_party'
        New-Item -ItemType Directory -Force -Path $dependencyRoot | Out-Null
        Copy-Item -LiteralPath (Join-Path $projectRoot $entry) -Destination $dependencyRoot -Recurse -Force
    } else { Copy-Item -LiteralPath (Join-Path $projectRoot $entry) -Destination $source -Recurse -Force }
}
$arguments = @('-S', $source, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_PREFIX_PATH=$QtPrefix", "-DMINIAUDIO_INCLUDE_DIR=$source/third_party/miniaudio")
if (!$WithoutAsr) { $arguments += '-DCHARASHELL_WITH_ASR=ON' }
else { $arguments += '-DCHARASHELL_WITH_ASR=OFF' }
if (!$HttpOnly -or !$WithoutAsr) {
    $sdkCopy = Join-Path $source 'native-include'
    New-Item -ItemType Directory -Force -Path $sdkCopy | Out-Null
    Copy-Item -Path (Join-Path $projectRoot 'third_party/native/include/*') -Destination $sdkCopy -Force
    $arguments += "-DNATIVE_INCLUDE_DIR=$sdkCopy"
}
if (!$HttpOnly) { $arguments += '-DCHARASHELL_NATIVE=ON' } else { $arguments += '-DCHARASHELL_NATIVE=OFF' }
& cmake @arguments
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
& cmake --build $build --parallel 4
if ($LASTEXITCODE) { throw 'Build failed.' }
$env:PATH = (Join-Path $QtPrefix 'bin') + ';' + $env:PATH
$output = Join-Path $projectRoot 'build-dist'
New-Item -ItemType Directory -Force -Path $output | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $output -Force
Copy-Item -LiteralPath (Join-Path $build 'charashell.exe') -Destination $output -Force
& (Join-Path $QtPrefix 'bin/windeployqt.exe') --release --qmldir (Join-Path $source 'qml') (Join-Path $output 'charashell.exe')
if ($LASTEXITCODE) { throw 'Qt deployment failed.' }
$offscreen = Join-Path $QtPrefix 'plugins/platforms/qoffscreen.dll'
if (Test-Path -LiteralPath $offscreen) {
    Copy-Item -LiteralPath $offscreen -Destination (Join-Path $output 'platforms') -Force
}
if (!$HttpOnly) {
    Get-ChildItem -LiteralPath (Join-Path $output "assets/$llamaDirectory") -Filter '*.dll' | Copy-Item -Destination $output -Force
    if ($Cuda) {
        if (!$env:CUDA_PATH) { throw 'CUDA_PATH is required to deploy the CUDA toolkit runtime.' }
        foreach ($pattern in @('cudart64*.dll','cublas64*.dll','cublasLt64*.dll')) {
            Get-ChildItem -Path (Join-Path $env:CUDA_PATH "bin/$pattern") | Copy-Item -Destination $output -Force
        }
    }
}
Copy-Item -LiteralPath $PSScriptRoot -Destination $output -Recurse -Force
Write-Output "Ready: $output/charashell.exe"
