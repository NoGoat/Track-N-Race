param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$VersionNumber,

    [Parameter(Position = 1)]
    [ValidateSet("Electron", "Qt", "Minimal")]
    [string[]]$Components,

    [switch]$Overwrite
)

$ErrorActionPreference = "Stop"

$rootDir = $PSScriptRoot
$releaseRoot = Join-Path $rootDir "release"
$releaseDir = Join-Path $releaseRoot $VersionNumber

if (-not $Components -or $Components.Count -eq 0) {
    while ($true) {
        $selectionText = Read-Host (
            "Components to build (Electron, Qt, Minimal, or All; comma-separated)"
        )
        $tokens = @(
            $selectionText -split '[,\s]+' |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
        )

        if ($tokens.Count -eq 1 -and $tokens[0] -ieq "All") {
            $Components = @("Electron", "Qt", "Minimal")
            break
        }

        $invalidTokens = @(
            $tokens | Where-Object {
                $_ -inotmatch '^(Electron|Qt|Minimal)$'
            }
        )
        if ($tokens.Count -gt 0 -and $invalidTokens.Count -eq 0) {
            $Components = @(
                foreach ($component in @("Electron", "Qt", "Minimal")) {
                    if ($tokens -icontains $component) {
                        $component
                    }
                }
            )
            break
        }

        Write-Host "Choose Electron, Qt, Minimal, All, or a comma-separated selection." `
            -ForegroundColor Yellow
    }
}

if (Test-Path -LiteralPath $releaseDir) {
    if (-not $Overwrite) {
        $overwriteChoice = Read-Host (
            "Release '$VersionNumber' already exists. Clear it and rebuild? (y/N)"
        )
        $Overwrite = $overwriteChoice -match '^[Yy]$'
    }

    if (-not $Overwrite) {
        throw "Release directory already exists: $releaseDir"
    }

    $releaseRootFull = [System.IO.Path]::GetFullPath($releaseRoot)
    $releaseDirFull = [System.IO.Path]::GetFullPath($releaseDir)
    if (-not $releaseDirFull.StartsWith(
            $releaseRootFull + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clear a directory outside the release root: $releaseDirFull"
    }

    Remove-Item -LiteralPath $releaseDirFull -Recurse -Force
    Write-Host "Cleared existing release directory: $releaseDirFull" `
        -ForegroundColor Yellow
}

function Assert-LastCommand {
    param([string]$Description)

    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

Write-Host (
    "Building Windows release '$VersionNumber': " +
    ($Components -join ", ")
)

$buildStep = 0
$buildStepCount = $Components.Count

$artifacts = @()

if ($Components -contains "Electron") {
    $buildStep++
    $previousDiagnostics = $env:INCLUDE_PERFORMANCE_DIAGNOSTICS
    $previousRcVersion = $env:RC_VERSION
    try {
        $env:INCLUDE_PERFORMANCE_DIAGNOSTICS = "0"
        $env:RC_VERSION = $VersionNumber

        Write-Host "`n[$buildStep/$buildStepCount] Building Electron installer..."
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

    $artifacts += @{
        Source = Join-Path $rootDir "electron-frontend\dist\Track-N-Race - Electron - Installer.exe"
        Name = "Track-N-Race - Electron - Installer.exe"
    }
}

if (($Components -contains "Qt") -or ($Components -contains "Minimal")) {
    $previousAppVersion = $env:TNR_APP_VERSION
    try {
        $env:TNR_APP_VERSION = $VersionNumber

        if ($Components -contains "Qt") {
            $buildStep++
            Write-Host "`n[$buildStep/$buildStepCount] Building Qt installer..."
            & (Join-Path $rootDir "qt_frontend\build-installer.ps1")
            Assert-LastCommand "Qt installer build"
        }

        if ($Components -contains "Minimal") {
            $buildStep++
            Write-Host "`n[$buildStep/$buildStepCount] Building minimal executable..."
            & (Join-Path $rootDir "minimal_frontend\build.ps1")
            Assert-LastCommand "Minimal executable build"
        }
    }
    finally {
        if ($null -eq $previousAppVersion) {
            Remove-Item Env:TNR_APP_VERSION -ErrorAction SilentlyContinue
        }
        else {
            $env:TNR_APP_VERSION = $previousAppVersion
        }
    }
}

if ($Components -contains "Qt") {
    $artifacts += @{
        Source = Join-Path $rootDir "qt_frontend\dist\Track-N-Race - Qt - Installer.exe"
        Name = "Track-N-Race - Qt - Installer.exe"
    }
}

if ($Components -contains "Minimal") {
    $artifacts += @{
        Source = Join-Path $rootDir "minimal_frontend\dist\windows\Track-N-Race - Minimal.exe"
        Name = "Track-N-Race - Minimal.exe"
    }
}

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
