param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build\Release'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\packages'),
    [string]$VisualCppRuntimeDirectory = '',
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')][string]$Version = 'preview'
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$stage = Join-Path $outputRoot ('.release-stage-' + [guid]::NewGuid().ToString('N'))
$zipName = "airplay-windows-$Version-x64.zip"
$zipPath = Join-Path $outputRoot $zipName
$runtimeFiles = @(
    'face_swap_worker.py', 'face_swap_gpu_worker.py', 'face_swap_detection.py',
    'face_swap_progress.py', 'face_swap_tensorrt.py', 'face_swap_models.py', 'face_swap_occlusion.py', 'face_swap_parser.py', 'face_swap_mask.py', 'face_swap_detectors.py', 'face_swap_landmarker.py', 'face_swap_portrait.py',
    'download_face_swap_models.py', 'prepare_face_swap_python.ps1', 'face_swap_models_manifest.json',
    'requirements-face-swap.txt', 'requirements-face-swap-gpu.txt',
    'requirements-face-swap-directml.txt', 'requirements-face-swap-cpu.txt', 'requirements-face-swap-tensorrt.txt'
)
try {
    New-Item -ItemType Directory -Path $stage -ErrorAction Stop | Out-Null
    Copy-Item -LiteralPath (Join-Path $buildRoot 'airplay-windows.exe') -Destination $stage
    Get-ChildItem -LiteralPath $buildRoot -Filter '*.dll' -File | Copy-Item -Destination $stage
    # App-local release CRT permits testing on a clean Windows sandbox without
    # requiring an installed Visual C++ Redistributable. Never ship debug CRTs.
    $crtFiles = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
    $missingCrt = @($crtFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $stage $_)) })
    if ($missingCrt.Count -gt 0) {
        if (-not $VisualCppRuntimeDirectory) {
            $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
            if (Test-Path -LiteralPath $vswhere) {
                $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
                if ($installation) {
                    $redistRoot = Join-Path $installation 'VC\Redist\MSVC'
                    $crt = Get-ChildItem -Path (Join-Path $redistRoot '*\x64\Microsoft.VC*.CRT') -Directory |
                        Sort-Object FullName -Descending | Select-Object -First 1
                    if ($crt) { $VisualCppRuntimeDirectory = $crt.FullName }
                }
            }
        }
        foreach ($name in $missingCrt) {
            if (-not $VisualCppRuntimeDirectory -or -not (Test-Path -LiteralPath (Join-Path $VisualCppRuntimeDirectory $name))) {
                throw "Missing release runtime $name. Supply -VisualCppRuntimeDirectory with the x64 redistributable CRT folder."
            }
            Copy-Item -LiteralPath (Join-Path $VisualCppRuntimeDirectory $name) -Destination $stage
        }
    }
    if ($VisualCppRuntimeDirectory) {
        # Optional Python/GPU wheels can use additional release CRT components.
        Get-ChildItem -LiteralPath $VisualCppRuntimeDirectory -Filter '*.dll' -File |
            Copy-Item -Destination $stage -Force
    }
    foreach ($name in @('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot $name) -Destination $stage
    }
    $docs = Join-Path $stage 'docs'
    New-Item -ItemType Directory -Path $docs | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $repoRoot 'docs') -Filter '*.md' -File |
        Where-Object Name -ne 'EXPLORATION-AUDIT.md' | Copy-Item -Destination $docs
    foreach ($relativePath in @('images/interface-preview.png', "releases/$Version.md")) {
        $sourceDoc = Join-Path (Join-Path $repoRoot 'docs') $relativePath
        if (Test-Path -LiteralPath $sourceDoc) {
            $destinationDoc = Join-Path $docs $relativePath
            New-Item -ItemType Directory -Path (Split-Path -Parent $destinationDoc) -Force | Out-Null
            Copy-Item -LiteralPath $sourceDoc -Destination $destinationDoc
        }
    }
    $faceRoot = Join-Path $stage 'face_swap'
    $engine = Join-Path $faceRoot 'engine'
    New-Item -ItemType Directory -Path $engine | Out-Null
    foreach ($name in $runtimeFiles) {
        Copy-Item -LiteralPath (Join-Path $buildRoot "face_swap\engine\$name") -Destination $engine
    }
    $bridge = Join-Path $buildRoot 'face_swap\engine\airplay-cuda-bridge.dll'
    if (Test-Path -LiteralPath $bridge) { Copy-Item -LiteralPath $bridge -Destination $engine }
    Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\FACE_SWAP_DISTRIBUTION.md') -Destination (Join-Path $faceRoot 'README.md')
    # Create the weights folder without copying local weights, photos or caches.
    New-Item -ItemType Directory -Path (Join-Path $faceRoot 'swappers') | Out-Null
    Set-Content -LiteralPath (Join-Path $faceRoot 'swappers\README.txt') -Encoding UTF8 -Value 'Place your original ONNX models here. See ../README.md.'
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -Force
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $zipName" | Set-Content -LiteralPath "$zipPath.sha256" -Encoding ASCII
    Write-Output $zipPath
    Write-Output "$zipPath.sha256"
} finally {
    # This script owns only its unique staging directory, never the build/output root.
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    if (-not $resolvedStage.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        -not ([IO.Path]::GetFileName($resolvedStage)).StartsWith('.release-stage-')) { throw 'Unexpected staging path' }
    if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
}
