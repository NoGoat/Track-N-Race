param(
    [string]$BuildDir = "$PSScriptRoot\build-windows",
    [string]$StageDir = "$PSScriptRoot\dist\windows"
)

$ErrorActionPreference = "Stop"

& "$PSScriptRoot\build.ps1" -WithBreeze -BuildDir $BuildDir -DistDir $StageDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sourceFile = Join-Path $StageDir "Track-N-Race - Minimal.exe"
$outputFile = Join-Path $PSScriptRoot "dist\Track-N-Race - Minimal - Portable.exe"
if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
    throw "Built executable was not found at: $sourceFile"
}

Copy-Item -LiteralPath $sourceFile -Destination $outputFile -Force
Write-Host "Portable executable: $outputFile" -ForegroundColor Green
