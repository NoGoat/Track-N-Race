param(
    [string]$MakeNsisPath = "",
    [string]$BuildDir = "$PSScriptRoot\build-windows",
    [string]$StageDir = "$PSScriptRoot\dist\windows"
)

$ErrorActionPreference = "Stop"

function Resolve-MakeNsis {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "makensis.exe was not found at: $ExplicitPath"
        }
        return [System.IO.Path]::GetFullPath($ExplicitPath)
    }

    $command = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"),
        (Join-Path $env:ProgramFiles "NSIS\makensis.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }

    if ($env:LOCALAPPDATA) {
        $cacheRoot = Join-Path $env:LOCALAPPDATA "electron-builder\Cache\nsis"
        $cached = Get-ChildItem -LiteralPath $cacheRoot -Filter makensis.exe -File -Recurse `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cached) { return $cached.FullName }
    }

    throw "NSIS was not found. Install NSIS or pass -MakeNsisPath."
}

& "$PSScriptRoot\build.ps1" -WithBreeze -BuildDir $BuildDir -DistDir $StageDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sourceDir = [System.IO.Path]::GetFullPath($StageDir)
$distDir = Join-Path $PSScriptRoot "dist"
$outputFile = [System.IO.Path]::GetFullPath(
    (Join-Path $distDir "Track-N-Race - Minimal - Installer.exe"))
New-Item -ItemType Directory -Path $distDir -Force | Out-Null

$makeNsis = Resolve-MakeNsis $MakeNsisPath
& $makeNsis "/DSOURCE_DIR=$sourceDir" "/DOUTPUT_FILE=$outputFile" `
    "$PSScriptRoot\installer.nsi"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Installer: $outputFile" -ForegroundColor Green
