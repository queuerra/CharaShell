param(
    [ValidateSet('All','Models','Dependencies','Servers')][string]$Kind = 'All',
    [ValidateSet('arm64','x64')][string]$Architecture = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }),
    [string]$AssetRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'assets'),
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'assets.json'),
    [string[]]$Ids = @(),
    [ValidateSet('cpu','cuda')][string]$Backend = 'cpu',
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$selected = @($manifest.files | Where-Object {
    ($Kind -eq 'All' -or $_.kind -eq $Kind) -and (!$_.architecture -or $_.architecture -eq $Architecture) -and (!$_.backend -or $_.backend -eq $Backend) -and (!$Ids.Count -or $_.id -in $Ids)
})
if (!$selected.Count) { throw 'No assets selected.' }
if ($Architecture -ne 'x64' -and @($selected | Where-Object { $_.id -in @('tts','asr') }).Count) {
    throw 'The pinned speech.cpp runtimes require Windows x64.'
}
$root = [IO.Path]::GetFullPath($AssetRoot)
function Resolve-AssetPath([string]$Relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $root $Relative))
    if (!$path.StartsWith($root.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Asset path escapes destination: $Relative"
    }
    return $path
}
foreach ($asset in $selected) {
    $asset.filename = $asset.filename.Replace('{architecture}', $Architecture)
    [void](Resolve-AssetPath $asset.filename)
    if ($asset.extract) { [void](Resolve-AssetPath (Join-Path $asset.extract $asset.required)) }
    if ($asset.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        ([long]$asset.size -le 0 -and [long]$asset.estimatedSize -le 0)) { throw "Invalid metadata: $($asset.id)" }
}
if ($PlanOnly) {
    $selected | Select-Object id,filename,size,url | ConvertTo-Json -Depth 4
    return
}
New-Item -ItemType Directory -Force -Path $root | Out-Null
# OS file lock prevents two GUI/build processes writing the same partial files.
$lock = [IO.File]::Open((Join-Path $root '.download.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
function Test-DownloadedFile($Asset, [string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $length = (Get-Item -LiteralPath $Path).Length
    if ([long]$Asset.size -gt 0 -and $length -ne [long]$Asset.size) { return $false }
    if ([long]$Asset.size -eq 0 -and $length -gt [long]$Asset.estimatedSize * 2) { return $false }
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','') -eq $Asset.sha256 }
    finally { $hasher.Dispose(); $stream.Dispose() }
}
function Receive-Asset($Asset) {
    $destination = Resolve-AssetPath $Asset.filename
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    if (Test-DownloadedFile $Asset $destination) {
        $length = (Get-Item -LiteralPath $destination).Length
        Write-Output "CACHED $($Asset.filename) $length/$length 0"; return $destination
    }
    $partial = $destination + '.part'
    if (Test-DownloadedFile $Asset $partial) {
        Move-Item -LiteralPath $partial -Destination $destination -Force
        return $destination
    }
    for ($attempt = 1; $attempt -le 3; ++$attempt) {
        $response = $null; $inputStream = $null; $outputStream = $null
        try {
            $offset = 0L
            if (Test-Path -LiteralPath $partial) { $offset = (Get-Item -LiteralPath $partial).Length }
            if (([long]$Asset.size -gt 0 -and $offset -ge [long]$Asset.size) -or
                ([long]$Asset.size -eq 0 -and $offset -gt [long]$Asset.estimatedSize * 2)) { $offset = 0L }
            $request = [Net.HttpWebRequest]::Create($Asset.url)
            $request.UserAgent = 'CharaShell-Setup/1'; $request.Timeout = 120000; $request.ReadWriteTimeout = 120000
            if ($offset) { $request.AddRange($offset) }
            if ($Asset.url.StartsWith('https://huggingface.co/') -and $env:HF_TOKEN) {
                $request.Headers['Authorization'] = 'Bearer ' + $env:HF_TOKEN
            }
            $response = $request.GetResponse()
            if ([int]$response.StatusCode -eq 206) {
                if ($response.Headers['Content-Range'] -notmatch "^bytes $offset-") { throw 'Invalid resumed byte range.' }
            } elseif ([int]$response.StatusCode -eq 200) { $offset = 0L }
            else { throw "Unexpected HTTP status: $($response.StatusCode)" }
            $expectedSize = [long]$Asset.size
            $knownSize = $expectedSize -gt 0
            if ($expectedSize -eq 0 -and $response.Headers['Content-Range'] -match '/(\d+)$') {
                $expectedSize = [long]$Matches[1]; $knownSize = $true
            }
            if ($expectedSize -eq 0 -and $response.ContentLength -gt 0) {
                $expectedSize = $offset + $response.ContentLength; $knownSize = $true
            }
            if ($expectedSize -eq 0) { $expectedSize = [long]$Asset.estimatedSize }
            if ($expectedSize -gt [long]$Asset.estimatedSize * 2 -and [long]$Asset.size -eq 0) {
                throw 'Response is larger than the model size limit.'
            }
            $mode = [IO.FileMode]::Create
            if ($offset) { $mode = [IO.FileMode]::Append }
            $outputStream = [IO.File]::Open($partial, $mode, [IO.FileAccess]::Write, [IO.FileShare]::None)
            $inputStream = $response.GetResponseStream()
            $buffer = New-Object byte[] (1024 * 1024)
            $received = $offset; $clock = [Diagnostics.Stopwatch]::StartNew(); $lastReport = -1000L
            Write-Output "DOWNLOAD $($Asset.filename) $received/$expectedSize 0"
            while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $outputStream.Write($buffer, 0, $count); $received += $count
                if (($knownSize -and $received -gt $expectedSize) -or
                    (!$knownSize -and $received -gt [long]$Asset.estimatedSize * 2)) {
                    throw 'Response is larger than the expected asset.'
                }
                if ($clock.ElapsedMilliseconds - $lastReport -ge 1000) {
                    $rate = if ($clock.ElapsedMilliseconds -gt 0) { [long](($received - $offset) * 1000 / $clock.ElapsedMilliseconds) } else { 0 }
                    Write-Output "PROGRESS $($Asset.filename) $received/$expectedSize $rate"; $lastReport = $clock.ElapsedMilliseconds
                }
            }
            $outputStream.Dispose(); $outputStream = $null
            Write-Output "VERIFY $($Asset.id)"
            if (!(Test-DownloadedFile $Asset $partial)) { throw 'Size or SHA-256 mismatch; retrying download.' }
            Move-Item -LiteralPath $partial -Destination $destination -Force
            Write-Output "PROGRESS $($Asset.filename) $received/$expectedSize 0"
            return $destination
        } catch {
            if ($attempt -eq 3) { throw }
            Write-Output "RETRY $($Asset.id): $($_.Exception.Message)"
        } finally {
            if ($inputStream) { $inputStream.Dispose() }
            if ($outputStream) { $outputStream.Dispose() }
            if ($response) { $response.Dispose() }
        }
    }
}
function Expand-Asset($Asset, [string]$Archive) {
    $destination = Resolve-AssetPath $Asset.extract
    $marker = Join-Path $destination ('.' + $Asset.filename.Replace('/','_').Replace('\','_') + '.ready')
    $required = Resolve-AssetPath (Join-Path $Asset.extract $Asset.required)
    if ((Test-Path -LiteralPath $required) -and (Test-Path -LiteralPath $marker) -and
        (Get-Content -LiteralPath $marker -Raw).Trim() -eq $Asset.sha256) { return }
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Write-Output "EXTRACT $($Asset.id)"
    # tar handles zip and bz2 on Windows. Validate entries before extracting.
    $entries = & tar -tf $Archive
    if ($LASTEXITCODE) { throw 'Cannot list archive.' }
    foreach ($entry in $entries) {
        if ($entry -match '(^[/\\]|^[A-Za-z]:|(^|[/\\])\.\.([/\\]|$))') { throw 'Unsafe archive path.' }
    }
    & tar -xf $Archive -C $destination
    if ($LASTEXITCODE) { throw "Cannot extract $($Asset.id)." }
    if (!(Test-Path -LiteralPath $required)) { throw "Archive layout changed: missing $($Asset.required)" }
    Set-Content -LiteralPath $marker -Value $Asset.sha256 -Encoding ASCII
}
try {
    foreach ($asset in $selected) {
        # Suppress progress from the return value while streaming it to the host.
        $path = $null
        Receive-Asset $asset | ForEach-Object {
            if ($_ -is [string] -and $_ -eq (Resolve-AssetPath $asset.filename)) { $path = $_ }
            else { Write-Output $_ }
        }
        if ($asset.extract) { Expand-Asset $asset $path }
    }
    if ($Kind -eq 'All' -or $Kind -eq 'Models') {
        $config = @{
            asrPath = (Resolve-AssetPath 'asr/Haas-616M-ja-v1.0-Q8_0.gguf')
            asrLibraryPath = (Resolve-AssetPath 'asr/native/speech.dll')
            llmPath = (Resolve-AssetPath 'models/gemma-4-e2b-qat/gemma-4-E2B_q4_0-it.gguf')
            ttsPath = (Resolve-AssetPath 'irodori/q8/Irodori-TTS-866M-MF-v4.1-Q8_0.gguf')
            llmModel = 'gemma-4-e2b-qat'; ttsModel = 'irodori-tts'; language = 'ja'
        }
        $config | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'models.json') -Encoding UTF8
    }
    Write-Output 'READY'
} finally { $lock.Dispose() }
