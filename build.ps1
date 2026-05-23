$ErrorActionPreference = "Stop"

$BuildType = Read-Host "What kind of build is this? (minor/major/overhaul)"

$PackageJsonPath = ".\package.json"
if (-Not (Test-Path $PackageJsonPath)) {
    Write-Host "Error: package.json not found!" -ForegroundColor Red
    exit 1
}

$PackageJson = Get-Content $PackageJsonPath -Raw | ConvertFrom-Json
$CurrentVersion = $PackageJson.version

$VersionParts = $CurrentVersion.Split('.')
if ($VersionParts.Length -ne 3) {
    Write-Host "Error: Unexpected version format in package.json ($CurrentVersion). Expected x.y.z" -ForegroundColor Red
    exit 1
}

$Major = [int]$VersionParts[0]
$Minor = [int]$VersionParts[1]
$Patch = [int]$VersionParts[2]

switch ($BuildType.ToLower().Trim()) {
    "minor" { $Patch++ }
    "major" { $Minor++; $Patch = 0 }
    "overhaul" { $Major++; $Minor = 0; $Patch = 0 }
    default {
        Write-Host "Invalid build type. Expected 'minor', 'major', or 'overhaul'." -ForegroundColor Red
        exit 1
    }
}

$NewVersion = "$Major.$Minor.$Patch"
Write-Host "Updating version from $CurrentVersion to $NewVersion..." -ForegroundColor Cyan

# Replace version in package.json using regex to preserve formatting
$Content = Get-Content $PackageJsonPath -Raw
$Content = $Content -replace "`"version`"\s*:\s*`"$CurrentVersion`"", "`"version`": `"$NewVersion`""
Set-Content -Path $PackageJsonPath -Value $Content -NoNewline

Write-Host "Building application..." -ForegroundColor Cyan
Write-Host "> npm run build"
npm run build
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed!" -ForegroundColor Red; exit $LASTEXITCODE }

Write-Host "> npm run dist"
npm run dist
if ($LASTEXITCODE -ne 0) { Write-Host "Dist failed!" -ForegroundColor Red; exit $LASTEXITCODE }

$Commit = Read-Host "Do you want to commit the version upgrade and push it? (y/n)"
if ($Commit.ToLower().StartsWith('y')) {
    Write-Host "Committing and pushing package.json..." -ForegroundColor Cyan
    git add package.json
    git commit -m "chore: bump version to $NewVersion"
    git push
    if ($LASTEXITCODE -ne 0) { Write-Host "Push failed!" -ForegroundColor Red; exit $LASTEXITCODE }
    Write-Host "Successfully committed and pushed package.json!" -ForegroundColor Green
} else {
    Write-Host "Skipping commit and push." -ForegroundColor Yellow
}

Write-Host "Done." -ForegroundColor Green
