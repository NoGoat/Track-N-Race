param(
    [string]$QtPrefix = $(if ($env:QT_PREFIX) { $env:QT_PREFIX } else { "C:\Qt\6.11.1\msvc2022_64" }),
    # -WithBreeze bundles the prebuilt Breeze style stack (see breeze_stack/) next
    # to the app. $BreezeRuntimeDir is where libintl/iconv live (the gettext bin).
    [switch]$WithBreeze,
    [string]$BreezePrefix = "",
    [string]$BreezeRuntimeDir = "C:\vcpkg\installed\x64-windows\bin"
)

# build.ps1 - Build the Track N Race Background Recorder on Windows
$ErrorActionPreference = "Stop"

Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "   TNRD Background Recorder - Build Script" -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan

# 1. Detect Visual Studio and bundled CMake
$vsWherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$cmakePath = "cmake"

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

# 2. Verify CMake is available
try {
    & $cmakePath --version | Out-Null
} catch {
    Write-Error "CMake not found. Install CMake or the Visual Studio C++ workload."
    exit 1
}

# 3. Configure CMake
Write-Host "`n[1/2] Configuring..." -ForegroundColor Yellow

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
if (-not $scriptDir) { $scriptDir = "." }
$buildDir = Join-Path $scriptDir "build"

# MSVC equivalents of -O3 -march=native -funroll-loops:
#   /O2   maximize speed  |  /Oi  enable intrinsics  |  /arch:AVX2  modern CPU target
$msvcReleaseFlags = "/O2 /Oi /arch:AVX2 /DNDEBUG"

$cmakeArgs = @(
    "-S", $scriptDir,
    "-B", $buildDir,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_CXX_FLAGS_RELEASE=$msvcReleaseFlags",
    "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON"
)

if ($QtPrefix) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtPrefix"
    Write-Host "Qt prefix: $QtPrefix" -ForegroundColor Gray
}

if ($WithBreeze) {
    if (-not $BreezePrefix) { $BreezePrefix = Join-Path $scriptDir "build\breeze_stack\prefix" }
    if (-not (Test-Path $BreezePrefix)) {
        Write-Error "-WithBreeze: stack prefix not found: $BreezePrefix`nBuild it first (see breeze_stack\CMakeLists.txt)."
        exit 1
    }
    Write-Host "Bundling Breeze from: $BreezePrefix" -ForegroundColor Gray
    Write-Host "Breeze runtime DLLs:  $BreezeRuntimeDir" -ForegroundColor Gray
    $cmakeArgs += "-DBREEZE_STACK_PREFIX=$BreezePrefix"
    $cmakeArgs += "-DBREEZE_WIN_RUNTIME_DIR=$BreezeRuntimeDir"
}

& $cmakePath @cmakeArgs
$configureExitCode = $LASTEXITCODE
if ($configureExitCode -ne 0) {
    Write-Host "CMake configuration failed (exit code $configureExitCode)." -ForegroundColor Red
    exit $configureExitCode
}

# 4. Build Release binary (parallel across all logical cores)
Write-Host "`n[2/2] Compiling..." -ForegroundColor Yellow
$jobs = $env:NUMBER_OF_PROCESSORS
& $cmakePath --build $buildDir --config Release --parallel $jobs
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    Write-Host "Build failed (exit code $buildExitCode)." -ForegroundColor Red
    exit $buildExitCode
}

# 5. Verify output
$exePath = Join-Path $buildDir "Release\Track-N-Race - Qt.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "Build failed: output executable not found at $exePath"
    exit 1
}

Write-Host "`n=============================================" -ForegroundColor Green
Write-Host "   Build Succeeded!" -ForegroundColor Green
Write-Host "=============================================" -ForegroundColor Green
Write-Host "Executable: $exePath" -ForegroundColor White

# 6. Deploy Qt DLLs
$windeployqt = Join-Path $QtPrefix "bin\windeployqt.exe"
if (Test-Path $windeployqt) {
    Write-Host "`n[+] Deploying Qt DLLs..." -ForegroundColor Yellow
    & $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw $exePath
    Write-Host "Qt DLLs deployed." -ForegroundColor Green
} else {
    Write-Warning "windeployqt not found at $windeployqt - Qt DLLs not copied. The exe may fail to launch."
}
