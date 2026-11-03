$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$sdk = Join-Path $projectRoot 'third_party/native/include'
New-Item -ItemType Directory -Force -Path $sdk | Out-Null
$headers = @{
    'llama.h' = @('include/llama.h','C0854771D3760A41642C4E59429C5B3B9FC774887C59D37EEADC4527681732CD')
    'ggml.h' = @('ggml/include/ggml.h','2B4DD92FA992CB4EF484917A6C62FEF5097698744160CCC09214FD0C459C92C7')
    'ggml-alloc.h' = @('ggml/include/ggml-alloc.h','D114067F9099C4A0E2BBC0625C676D4894D9CF0EA27168EE072C7DE0E71BB51D')
    'ggml-backend.h' = @('ggml/include/ggml-backend.h','0E5E8E1F16D11EBBF501E38BC6705D79EEE819B5F0B17476533F2100ED9674E2')
    'ggml-cpu.h' = @('ggml/include/ggml-cpu.h','316279E004CDEB8E6EF78599ACB602BF79A8ABDF897FED9FD1914808C1518C6E')
    'ggml-opt.h' = @('ggml/include/ggml-opt.h','3586DE1BC8A934B5C72339E2B6937B0641E8F149B512231E666F67DE0736EEA2')
    'gguf.h' = @('ggml/include/gguf.h','E56714AAB702E5CE62EE587A409643C08F7E93E8FBB77F48EF7CC85075F96FA4')
    'speech.h' = @('include/speech.h','1567C5E09C854A1A145EEF82C335273C5AA02341487CD12E57355C789A79852A')
}
function Get-Sha([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return '' }
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','') }
    finally { $hasher.Dispose(); $stream.Dispose() }
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
foreach ($name in $headers.Keys) {
    $file = Join-Path $sdk $name
    if ((Get-Sha $file) -eq $headers[$name][1]) { continue }
    $url = 'https://raw.githubusercontent.com/ggml-org/llama.cpp/b11960/' + $headers[$name][0]
    if ($name -eq 'speech.h') { $url = 'https://raw.githubusercontent.com/nyosegawa/speech.cpp/7ac99697cd680c78beef482955279ef15d4421c7/include/speech.h' }
    $partial = $file + '.part'
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $partial
    if ((Get-Sha $partial) -ne $headers[$name][1]) { throw "Header hash mismatch: $name" }
    Move-Item -LiteralPath $partial -Destination $file -Force
}
Write-Output "Ready: native LLM and speech API headers in $sdk"
