# build.ps1 - Compile the Native Telemetry Recorder
$ErrorActionPreference = "Stop"

Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "   TNRD Background Recorder - Build Script" -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan

# 1. Detect Visual Studio 2026 and bundled CMake
$vsWherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$cmakePath = "cmake" # Fallback to system PATH

if (Test-Path $vsWherePath) {
    Write-Host "Detecting Visual Studio installations..." -ForegroundColor Gray
    $vsPath = & $vsWherePath -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsPath) {
        $bundledCMake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $bundledCMake) {
            $cmakePath = $bundledCMake
            Write-Host "Using bundled Visual Studio CMake: $cmakePath" -ForegroundColor Green
        }
    }
}

# 2. Check if CMake is available
try {
    & $cmakePath --version | Out-Null
} catch {
    Write-Error "CMake could not be found. Please ensure CMake is installed and added to the PATH, or Visual Studio is installed with C++ development workload."
    exit 1
}

# 3. Configure CMake
Write-Host "`n[1/2] Configuring build files..." -ForegroundColor Yellow
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $scriptDir) { $scriptDir = "." }

& $cmakePath -S $scriptDir -B (Join-Path $scriptDir "build")

# 4. Build Release binary
Write-Host "`n[2/2] Compiling and linking binary..." -ForegroundColor Yellow
& $cmakePath --build (Join-Path $scriptDir "build") --config Release

$exePath = Join-Path $scriptDir "build\Release\Track N Race Background Recorder.exe"
if (Test-Path $exePath) {

    Write-Host "`n=============================================" -ForegroundColor Green
    Write-Host "   Build Succeeded successfully!" -ForegroundColor Green
    Write-Host "=============================================" -ForegroundColor Green
    Write-Host "Executable location: $exePath" -ForegroundColor White
    
    $choice = Read-Host "`nWould you like to launch the application now? (Y/N)"
    if ($choice -eq "Y" -or $choice -eq "y") {
        Write-Host "Launching $exePath..." -ForegroundColor Cyan
        Start-Process $exePath
    }
} else {
    Write-Error "Build finished but output executable could not be found."
    exit 1
}
