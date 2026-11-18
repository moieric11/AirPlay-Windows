param(
    [Parameter(Mandatory = $true)][ValidateSet('cuda','directml','cpu')][string]$Backend,
    [Parameter(Mandatory = $true)][string]$ProgressFile,
    [Parameter(Mandatory = $true)][string]$Workspace,
    [switch]$TensorRT
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$utf8 = New-Object Text.UTF8Encoding($false)
$ProgressFile = [IO.Path]::GetFullPath($ProgressFile)
$assets = if ((Split-Path -Leaf $PSScriptRoot) -eq 'engine') { Join-Path $PSScriptRoot '..' }
    else { Join-Path $PSScriptRoot '..\face_swap' }
$assets = [IO.Path]::GetFullPath($assets)
$workRoot = [IO.Path]::GetFullPath($Workspace)
if ([IO.Path]::GetDirectoryName($workRoot) -ne $assets -or
    -not ([IO.Path]::GetFileName($workRoot)).StartsWith('.python-setup-')) {
    throw 'Invalid Python preparation workspace'
}
function Report([string]$Phase, [string]$Message, [long]$Received = 0, [long]$Total = 0, [string]$Python = '') {
    $contents = "$Phase`n$Received $Total`n$($Message.Replace("`n",' ').Replace("`r",' '))`n$Python`n"
    $temporary = "$ProgressFile.tmp"
    [IO.File]::WriteAllText($temporary, $contents, $utf8)
    # File.Move cannot replace an existing file on .NET Framework.
    if (Test-Path -LiteralPath $ProgressFile) { [IO.File]::Replace($temporary, $ProgressFile, [System.Management.Automation.Language.NullString]::Value) }
    else { [IO.File]::Move($temporary, $ProgressFile) }
}
function Download([string]$Uri, [string]$Path) {
    $request = [Net.HttpWebRequest]::Create($Uri)
    $request.Timeout = 30000
    $request.ReadWriteTimeout = 30000
    $response = $null; $inputStream = $null; $outputStream = $null
    try {
        $response = $request.GetResponse()
        $inputStream = $response.GetResponseStream()
        $outputStream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew)
        $buffer = New-Object byte[] 65536
        [long]$received = 0
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $outputStream.Write($buffer, 0, $count); $received += $count
            if ($timer.ElapsedMilliseconds -ge 250) {
                Report 'downloading' 'Downloading Python...' $received ([Math]::Max(0,$response.ContentLength))
                $timer.Restart()
            }
        }
    } finally {
        if ($outputStream) { $outputStream.Dispose() }
        if ($inputStream) { $inputStream.Dispose() }
        if ($response) { $response.Dispose() }
    }
}
function RunPython([string[]]$Arguments) {
    & $script:python -I @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Python command failed (exit $LASTEXITCODE). See the preparation log." }
}
try {
    if (-not (Test-Path -LiteralPath $workRoot)) { New-Item -ItemType Directory -Path $workRoot -ErrorAction Stop | Out-Null }
    if (Get-ChildItem -LiteralPath $workRoot -Force) { throw 'Python preparation workspace must be empty' }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $archiveHash = '9283876d58c017e0e846f95b490da3bca0fc0a6ee1134b2870677cfb7eec3c67'
    $cache = Join-Path $assets 'downloads'
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $archive = Join-Path $cache 'python-3.11.9-x64.nupkg'
    Report 'downloading' 'Downloading Python...'
    $valid = (Test-Path -LiteralPath $archive) -and ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -eq $archiveHash)
    if (-not $valid) {
        $partial = Join-Path $workRoot 'python.part'
        Download 'https://api.nuget.org/v3-flatcontainer/python/3.11.9/python.3.11.9.nupkg' $partial
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $archiveHash) { throw 'Python archive SHA-256 verification failed' }
        # Replace only this managed download cache, never an existing environment.
        Copy-Item -LiteralPath $partial -Destination $archive -Force
    }
    Report 'extracting' 'Extracting Python...'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $unpacked = Join-Path $workRoot 'unpacked'
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $unpacked)
    $environment = Join-Path $unpacked 'tools'
    $script:python = Join-Path $environment 'python.exe'
    if (-not (Test-Path -LiteralPath $script:python)) { throw 'Python package does not contain python.exe' }
    # Python extension modules also need the release CRT on clean machines.
    # Use the redistributable files shipped with the app, never system copies.
    $application = Split-Path -Parent $assets
    foreach ($pattern in @('msvcp140*.dll','vcruntime140*.dll','concrt140.dll','vccorlib140.dll')) {
        Get-ChildItem -LiteralPath $application -Filter $pattern -File |
            Copy-Item -Destination $environment -Force
    }
    Report 'installing' 'Installing Python dependencies...'
    RunPython @('-m','pip','--isolated','install','--disable-pip-version-check','--no-warn-script-location','--cache-dir',(Join-Path $cache 'pip'),
        '--index-url','https://pypi.org/simple','--only-binary=:all:','pip==25.3')
    $requirements = if ($Backend -eq 'directml') { 'requirements-face-swap-directml.txt' }
        elseif ($Backend -eq 'cpu') { 'requirements-face-swap-cpu.txt' }
        elseif ($TensorRT) { 'requirements-face-swap-tensorrt.txt' }
        else { 'requirements-face-swap-gpu.txt' }
    RunPython @('-m','pip','--isolated','install','--disable-pip-version-check','--no-warn-script-location','--cache-dir',(Join-Path $cache 'pip'),
        '--index-url','https://pypi.org/simple','--only-binary=:all:','-r',(Join-Path $PSScriptRoot $requirements))
    if ($Backend -eq 'cuda') {
        # Also provision NVRTC and headers for CuPy kernels on driver-only PCs.
        RunPython @('-m','pip','--isolated','install','--disable-pip-version-check','--no-warn-script-location','--cache-dir',(Join-Path $cache 'pip'),
            '--index-url','https://pypi.org/simple','--only-binary=:all:','cupy-cuda12x[ctk]==14.2.0')
    }
    # Its default dependency would install a second, incompatible ORT package.
    RunPython @('-m','pip','--isolated','install','--disable-pip-version-check','--no-warn-script-location','--cache-dir',(Join-Path $cache 'pip'),
        '--index-url','https://pypi.org/simple','--only-binary=:all:','--no-deps','insightface==2.1')
    Report 'verifying' 'Checking Python environment...'
    $verify = @'
import sys, struct
import cv2, numpy, onnx, onnxruntime as ort, scipy, skimage, requests
from insightface.model_zoo import get_model
from insightface.app.common import Face
from insightface.utils.face_align import norm_crop2, estimate_norm
assert sys.version_info[:2] == (3, 11) and struct.calcsize('P') == 8
backend = sys.argv[1]
provider = {'cuda': 'CUDAExecutionProvider', 'directml': 'DmlExecutionProvider', 'cpu': 'CPUExecutionProvider'}[backend]
assert provider in ort.get_available_providers(), 'Missing ONNX Runtime provider: ' + provider
if backend == 'cuda':
    import cupy
print('Python runtime verified:', sys.version, provider)
'@
    RunPython @('-c',$verify,$Backend)
    if ($Backend -eq 'cuda' -and $TensorRT) {
        RunPython @('-c',"from importlib.metadata import version; from pathlib import Path; import sys; assert version('tensorrt-cu12-libs') == '10.9.0.34'; assert list((Path(sys.prefix)/'Lib/site-packages/tensorrt_libs').glob('nvinfer*.dll'))")
    }
    $name = if ($Backend -eq 'cuda') { 'python' } else { "python-$Backend" }
    $destination = Join-Path $assets $name
    if (Test-Path -LiteralPath $destination) {
        $destination = Join-Path $assets ($name + '-managed-' + [guid]::NewGuid().ToString('N'))
    }
    # A cancelled/failed install never replaces or edits a working interpreter.
    [IO.Directory]::Move($environment,$destination)
    $script:python = Join-Path $destination 'python.exe'
    # Check again at the final location so relocation errors are caught here.
    RunPython @('-c',$verify,$Backend)
    Report 'complete' 'Python environment ready.' 0 0 $script:python
    exit 0
} catch {
    try { Report 'failed' ($_.Exception.Message) } catch { }
    Write-Error $_ -ErrorAction Continue
    exit 1
}
