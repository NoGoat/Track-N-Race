param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z._-]*$')]
    [string]$VersionName
)

$ErrorActionPreference = "Stop"

$rootDir = $PSScriptRoot
$releaseDir = Join-Path $rootDir "release-$VersionName"

if (Test-Path -LiteralPath $releaseDir) {
    throw "Release directory already exists: $releaseDir"
}

function Assert-LastCommand {
    param([string]$Description)

    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

Write-Host "Building Windows release '$VersionName'..."

$previousDiagnostics = $env:INCLUDE_PERFORMANCE_DIAGNOSTICS
try {
    $env:INCLUDE_PERFORMANCE_DIAGNOSTICS = "0"

    Write-Host "`n[1/3] Building Electron installer..."
    Push-Location (Join-Path $rootDir "electron-frontend")
    try {
        & npm.cmd run build
        Assert-LastCommand "Electron application build"

        & npm.cmd run dist -- --win nsis
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
}

Write-Host "`n[2/3] Building Qt installer..."
& (Join-Path $rootDir "qt_frontend\build-installer.ps1")
Assert-LastCommand "Qt installer build"

Write-Host "`n[3/3] Building minimal executable..."
& (Join-Path $rootDir "minimal_frontend\build.ps1")
Assert-LastCommand "Minimal executable build"

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

New-Item -ItemType Directory -Path $releaseDir | Out-Null
foreach ($artifact in $artifacts) {
    Copy-Item -LiteralPath $artifact.Source `
        -Destination (Join-Path $releaseDir $artifact.Name)
}

Write-Host "`nWindows release complete: $releaseDir" -ForegroundColor Green
