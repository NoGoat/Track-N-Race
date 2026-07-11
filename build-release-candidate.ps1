$ErrorActionPreference = "Stop"

$RcVersion = Read-Host "What version should the version panel show? (e.g. 1.3.0-rc1)"
if ([string]::IsNullOrWhiteSpace($RcVersion)) {
    Write-Host "Error: version cannot be empty." -ForegroundColor Red
    exit 1
}

try {
    $env:RC_VERSION = $RcVersion
    Write-Host "Building release candidate labeled as $RcVersion (package.json version is left untouched)..." -ForegroundColor Cyan

    Write-Host "Building Native Telemetry Recorder..." -ForegroundColor Cyan
    Push-Location native_recorder
    & .\build.ps1 -NoLaunch
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Native recorder build failed!" -ForegroundColor Red
        Pop-Location
        exit $LASTEXITCODE
    }
    Pop-Location

    Write-Host "Building application..." -ForegroundColor Cyan
    Write-Host "> npm run build"
    npm run build
    if ($LASTEXITCODE -ne 0) { Write-Host "Build failed!" -ForegroundColor Red; exit $LASTEXITCODE }

    # --config.extraMetadata.version overrides the version electron-builder uses for
    # artifact filenames and the packaged app's own package.json / app.getVersion(),
    # without ever writing back to the real package.json on disk.
    Write-Host "> npm run dist -- --config.extraMetadata.version=$RcVersion"
    npm run dist -- --config.extraMetadata.version=$RcVersion
    if ($LASTEXITCODE -ne 0) { Write-Host "Dist failed!" -ForegroundColor Red; exit $LASTEXITCODE }

    Write-Host "> npm run dist:win:portable -- --config.extraMetadata.version=$RcVersion"
    npm run dist:win:portable -- --config.extraMetadata.version=$RcVersion
    if ($LASTEXITCODE -ne 0) { Write-Host "Portable dist failed!" -ForegroundColor Red; exit $LASTEXITCODE }

    Write-Host "Done. Release candidate '$RcVersion' built (package.json version unchanged)." -ForegroundColor Green
}
finally {
    Remove-Item Env:\RC_VERSION -ErrorAction SilentlyContinue
}
