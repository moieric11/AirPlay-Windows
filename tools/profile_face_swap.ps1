param(
    [ValidateRange(3, 60)][int]$CaptureSeconds = 15,
    [string]$Executable,
    [string[]]$ApplicationArguments = @(),
    [switch]$NoPrompt
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $projectRoot 'build\Release\airplay-windows.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$nsysCommand = Get-Command nsys.exe -ErrorAction SilentlyContinue
if ($nsysCommand) { $nsysPath = $nsysCommand.Source }
else {
    $nsysPath = Get-ChildItem 'C:\Program Files\NVIDIA Corporation' -Directory -Filter 'Nsight Systems*' |
        Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'target-windows-x64\nsys.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $nsysPath) { throw 'NVIDIA Nsight Systems was not found.' }
if ((Split-Path $Executable -Leaf) -eq 'airplay-windows.exe' -and
    (Get-Process -Name 'airplay-windows*' -ErrorAction SilentlyContinue)) {
    throw 'Close AirPlay-Windows first, then run this script again.'
}
$reportDirectory = Join-Path $projectRoot 'build\profiles'
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$sessionName = 'AirPlayFaceswap-' + $stamp
$reportBase = Join-Path $reportDirectory ('faceswap-' + $stamp)
# Quote using Windows command-line rules; Start-Process joins ArgumentList.
function Quote-Argument([string]$Value) {
    '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
$arguments = @('profile', '--start-later=true', ('--session-new=' + $sessionName),
    ('--duration=' + $CaptureSeconds), '--kill=false', '--trace=cuda,nvtx,dx11',
    '--cuda-trace-scope=process-tree', '--cuda-graph-trace=graph',
    '--sample=none', '--cpuctxsw=none', '--discard-environment=true',
    ('--output=' + $reportBase), $Executable) + $ApplicationArguments
$previousMarkers = $env:AIRPLAY_FACE_SWAP_NVTX
try {
    $env:AIRPLAY_FACE_SWAP_NVTX = '1'
    $profiler = Start-Process -FilePath $nsysPath -ArgumentList ($arguments | ForEach-Object { Quote-Argument $_ }) `
        -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput ($reportBase + '.log') -RedirectStandardError ($reportBase + '.error.log')
} finally { $env:AIRPLAY_FACE_SWAP_NVTX = $previousMarkers }
Start-Sleep -Seconds 2
if ($profiler.HasExited) { throw ('Profiler exited early. See ' + $reportBase + '.error.log and .log') }
Write-Host 'Connect AirPlay, enable NVIDIA CUDA, Keep video on GPU and TensorRT.'
Write-Host 'Wait until the face swap is running smoothly and model loading has finished.'
if (-not $NoPrompt) { Read-Host 'Press Enter to capture 15 seconds (or the configured duration)' | Out-Null }
& $nsysPath start ('--session=' + $sessionName) --sample=none --cpuctxsw=none
if ($LASTEXITCODE -ne 0) { throw ('Could not start capture. See ' + $reportBase + '.log') }
Write-Host ('Capturing ' + $CaptureSeconds + ' seconds. Move your face normally.')
if (-not $profiler.WaitForExit(($CaptureSeconds + 45) * 1000)) {
    throw ('Report generation is still running. Check ' + $reportBase + '.log')
}
$report = $reportBase + '.nsys-rep'
if (-not (Test-Path -LiteralPath $report)) { throw ('No report generated. Check ' + $reportBase + '.log and .error.log') }
Write-Host ('Capture complete: ' + $report)
