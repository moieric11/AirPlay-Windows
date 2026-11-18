param(
    [Parameter(Mandatory = $true)][string]$ModelDirectory,
    [int]$Device = 0
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$pythonPath = Join-Path $projectRoot '.venv-face-swap\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $pythonPath)) {
    throw 'Install the isolated face-swap environment first: see docs/FACE_SWAP.md'
}
$modelRoot = (Resolve-Path -LiteralPath $ModelDirectory).Path
$detector = Join-Path $modelRoot 'det_10g.onnx'
$recognizer = Join-Path $modelRoot 'w600k_r50.onnx'
$swapper = Join-Path $modelRoot 'inswapper_128.onnx'
if (-not (Test-Path -LiteralPath $swapper)) {
    $swapper = Join-Path $modelRoot 'inswapper_128.fp16.onnx'
}
foreach ($model in @($detector, $recognizer, $swapper)) {
    if (-not (Test-Path -LiteralPath $model)) { throw "Model missing: $model" }
}
$logDirectory = Join-Path $projectRoot 'build\Release\face_swap\logs'
$cacheRoot = Join-Path $projectRoot 'build\Release\face_swap\swappers'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$outputLog = Join-Path $logDirectory "face-swap-$stamp.log"
$errorLog = Join-Path $logDirectory "face-swap-$stamp.error.log"
$workerPath = Join-Path $PSScriptRoot 'face_swap_worker.py'
# Quoted existing file paths cannot contain a double quote on Windows.
$workerArguments = @('-u', ('"' + $workerPath + '"'),
    '--detector', ('"' + $detector + '"'),
    '--recognizer', ('"' + $recognizer + '"'),
    '--swapper', ('"' + $swapper + '"'), '--cache-root', ('"' + $cacheRoot + '"'), '--device', $Device.ToString())
$worker = Start-Process -FilePath $pythonPath -ArgumentList $workerArguments `
    -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $outputLog -RedirectStandardError $errorLog
Write-Output "Face-swap worker PID: $($worker.Id)"
Write-Output "Log: $outputLog"
Write-Output "Errors: $errorLog"
Write-Output 'Wait for "CUDA face-swap worker ready" in the log, then disable/enable face swap in the receiver.'
