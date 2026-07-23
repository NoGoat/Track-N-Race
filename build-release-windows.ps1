param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$VersionNumber
)

$ErrorActionPreference = "Stop"

$rootDir = $PSScriptRoot
$releaseRoot = Join-Path $rootDir "release"
$releaseDir = Join-Path $releaseRoot $VersionNumber

if (Test-Path -LiteralPath $releaseDir) {
    throw "Release directory already exists: $releaseDir"
}

function Assert-LastCommand {
    param([string]$Description)

    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

Write-Host "Building Windows release '$VersionNumber'..."

$previousDiagnostics = $env:INCLUDE_PERFORMANCE_DIAGNOSTICS
$previousRcVersion = $env:RC_VERSION
try {
    $env:INCLUDE_PERFORMANCE_DIAGNOSTICS = "0"
    $env:RC_VERSION = $VersionNumber

    Write-Host "`n[1/3] Building Electron installer..."
    Push-Location (Join-Path $rootDir "electron-frontend")
    try {
        & npm.cmd run build
        Assert-LastCommand "Electron application build"

        & npm.cmd run dist -- --win nsis `
            "--config.extraMetadata.version=$VersionNumber"
        Assert-LastCommand "Electron installer build"
    }
    finally {
        Pop-Location
    }
}
finally {
    if ($null -eq $previousDiagnostics) {
        Remove-Item Env:INCLUDE_PERFORMANCE_DIAGNOSTICS -ErrorAction SilentlyContinue
    }
    else {
        $env:INCLUDE_PERFORMANCE_DIAGNOSTICS = $previousDiagnostics
    }
    if ($null -eq $previousRcVersion) {
        Remove-Item Env:RC_VERSION -ErrorAction SilentlyContinue
    }
    else {
        $env:RC_VERSION = $previousRcVersion
    }
}

$previousAppVersion = $env:TNR_APP_VERSION
try {
    $env:TNR_APP_VERSION = $VersionNumber

    Write-Host "`n[2/3] Building Qt installer..."
    & (Join-Path $rootDir "qt_frontend\build-installer.ps1")
    Assert-LastCommand "Qt installer build"

    Write-Host "`n[3/3] Building minimal executable..."
    & (Join-Path $rootDir "minimal_frontend\build.ps1")
    Assert-LastCommand "Minimal executable build"
}
finally {
    if ($null -eq $previousAppVersion) {
        Remove-Item Env:TNR_APP_VERSION -ErrorAction SilentlyContinue
    }
    else {
        $env:TNR_APP_VERSION = $previousAppVersion
    }
}

$artifacts = @(
    @{
        Source = Join-Path $rootDir "electron-frontend\dist\Track-N-Race - Electron - Installer.exe"
        Name = "Track-N-Race - Electron - Installer.exe"
    },
    @{
        Source = Join-Path $rootDir "qt_frontend\dist\Track-N-Race - Qt - Installer.exe"
        Name = "Track-N-Race - Qt - Installer.exe"
    },
    @{
        Source = Join-Path $rootDir "minimal_frontend\dist\windows\Track-N-Race - Minimal.exe"
        Name = "Track-N-Race - Minimal.exe"
    }
)

foreach ($artifact in $artifacts) {
    if (-not (Test-Path -LiteralPath $artifact.Source -PathType Leaf)) {
        throw "Expected build artifact was not produced: $($artifact.Source)"
    }
}

New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
New-Item -ItemType Directory -Path $releaseDir | Out-Null
foreach ($artifact in $artifacts) {
    Copy-Item -LiteralPath $artifact.Source `
        -Destination (Join-Path $releaseDir $artifact.Name)
}

Write-Host "`nWindows release complete: $releaseDir" -ForegroundColor Green
