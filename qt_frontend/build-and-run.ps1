[CmdletBinding()]
param(
    [string]$QtPrefix = "",
    [string]$BreezePrefix = "",
    [string]$BreezeRuntimeDir = ""
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $scriptDir) { $scriptDir = "." }

$buildScript = Join-Path $scriptDir "build.ps1"
$releaseDir = Join-Path $scriptDir "build\Release"
$exePath = Join-Path $releaseDir "Track-N-Race - Qt.exe"

if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
    Write-Error "Qt build script not found at $buildScript"
    exit 1
}

$buildArgs = @{ WithBreeze = $true }
if ($QtPrefix) { $buildArgs.QtPrefix = $QtPrefix }
if ($BreezePrefix) { $buildArgs.BreezePrefix = $BreezePrefix }
if ($BreezeRuntimeDir) { $buildArgs.BreezeRuntimeDir = $BreezeRuntimeDir }

& $buildScript @buildArgs
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    Write-Host "Build failed (exit code $buildExitCode). The application was not launched." -ForegroundColor Red
    exit $buildExitCode
}

if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    Write-Error "Build completed but the application was not found at $exePath"
    exit 1
}

Write-Host "`nLaunching: $exePath" -ForegroundColor Green
Start-Process -FilePath $exePath -WorkingDirectory $releaseDir

