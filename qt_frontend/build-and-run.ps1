# Usage: .uild-and-run.ps1 [-QtPrefix ...] [-BreezePrefix ...] [-BreezeRuntimeDir ...] [--first-launch]
#   --first-launch (or -FirstLaunch) opens first-time setup on launch, even on
#   an install that has already finished it.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$QtPrefix = "",
    [string]$BreezePrefix = "",
    [string]$BreezeRuntimeDir = "",
    [switch]$FirstLaunch,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AppArgs = @()
)

$ErrorActionPreference = "Stop"

$launchArgs = @()
foreach ($arg in $AppArgs) {
    if ($arg -ieq "--first-launch" -or $arg -ieq "--first-time-setup") {
        $FirstLaunch = $true
    } else {
        Write-Error "Unknown argument: $arg"
        exit 1
    }
}
if ($FirstLaunch) { $launchArgs += "--first-launch" }

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

Write-Host "`nLaunching: $exePath $($launchArgs -join ' ')" -ForegroundColor Green
if ($launchArgs.Count -gt 0) {
    Start-Process -FilePath $exePath -WorkingDirectory $releaseDir -ArgumentList $launchArgs
} else {
    Start-Process -FilePath $exePath -WorkingDirectory $releaseDir
}

