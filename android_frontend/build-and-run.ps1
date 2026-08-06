[CmdletBinding()]
param(
    [string]$DeviceSerial,
    [string]$SdkRoot,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$ProjectDir = $PSScriptRoot
$ApkPath = Join-Path $ProjectDir 'app\build\outputs\apk\debug\app-debug.apk'

function Test-FullAndroidSdk([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    # A standalone Platform-Tools zip is useful for ADB but is not an SDK that
    # Gradle can use to provision platforms, CMake or the NDK.
    $sdkMarkers = @('platforms', 'build-tools', 'cmdline-tools', 'licenses', 'ndk')
    return $null -ne ($sdkMarkers | Where-Object { Test-Path -LiteralPath (Join-Path $Path $_) } | Select-Object -First 1)
}

function Find-AndroidSdk {
    $candidates = @($SdkRoot, $env:ANDROID_SDK_ROOT, $env:ANDROID_HOME, (Join-Path $env:LOCALAPPDATA 'Android\Sdk')) |
        Where-Object { $_ } |
        Select-Object -Unique
    foreach ($candidate in $candidates) {
        if (Test-FullAndroidSdk $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $platformToolsOnly = Get-Command adb -ErrorAction SilentlyContinue
    $adbNote = if ($platformToolsOnly) {
        "`nADB was found at '$($platformToolsOnly.Source)', but its standalone Platform-Tools folder is not a complete Android SDK."
    } else { '' }
    throw @"
A complete Android SDK was not found.$adbNote
Install Android Studio (no emulator required), then use SDK Manager to install:
  - Android SDK Platform 35
  - Android SDK Build-Tools
  - Android SDK Platform-Tools
  - NDK (Side by side) 27.0.12077973
  - CMake 3.22.1
  - Android SDK Command-line Tools (latest)
Accept the SDK licenses, then rerun with:
  .\build-and-run.ps1 -SdkRoot "$env:LOCALAPPDATA\Android\Sdk"
"@
}

function Assert-AndroidSdkLicenses([string]$AndroidSdk) {
    $licenseFile = Join-Path $AndroidSdk 'licenses\android-sdk-license'
    if ((Test-Path -LiteralPath $licenseFile) -and (Get-Item -LiteralPath $licenseFile).Length -gt 0) { return }

    $sdkManager = Get-ChildItem -LiteralPath (Join-Path $AndroidSdk 'cmdline-tools') `
        -Filter sdkmanager.bat -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($sdkManager) {
        throw @"
Android SDK licenses have not been accepted. Review and accept them with:
  & "$sdkManager" --sdk_root="$AndroidSdk" --licenses
Then rerun this script.
"@
    }
    throw 'Android SDK licenses have not been accepted. Accept them in Android Studio SDK Manager, then rerun this script.'
}

function Find-Adb([string]$AndroidSdk) {
    if ($AndroidSdk) {
        $sdkAdb = Join-Path $AndroidSdk 'platform-tools\adb.exe'
        if (Test-Path -LiteralPath $sdkAdb) { return $sdkAdb }
    }
    $fromPath = Get-Command adb -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }
    throw 'adb was not found. Install Android SDK Platform-Tools or set ANDROID_SDK_ROOT.'
}

function Find-Gradle {
    $localWrapper = Join-Path $ProjectDir 'gradlew.bat'
    if (Test-Path -LiteralPath $localWrapper) { return $localWrapper }
    $fromPath = Get-Command gradle -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }
    throw 'Gradle was not found. Install Gradle 8.9+ or generate a Gradle wrapper in android_frontend.'
}

$ResolvedSdk = $null
if (-not $SkipBuild) {
    $ResolvedSdk = Find-AndroidSdk
    $env:ANDROID_SDK_ROOT = $ResolvedSdk
    $env:ANDROID_HOME = $ResolvedSdk
    Assert-AndroidSdkLicenses $ResolvedSdk
}
$Adb = Find-Adb $ResolvedSdk
$AdbArgs = @()
if ($DeviceSerial) { $AdbArgs += @('-s', $DeviceSerial) }

& $Adb @AdbArgs start-server | Out-Null
$deviceLines = @(& $Adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' })
if (-not $DeviceSerial -and $deviceLines.Count -ne 1) {
    throw "Expected exactly one authorized device, found $($deviceLines.Count). Pass -DeviceSerial when several are connected."
}

if (-not $SkipBuild) {
    $Gradle = Find-Gradle
    & $Gradle --no-daemon -p $ProjectDir :app:assembleDebug
    if ($LASTEXITCODE -ne 0) { throw "Gradle build failed with exit code $LASTEXITCODE." }
}

if (-not (Test-Path -LiteralPath $ApkPath)) {
    throw "APK not found at $ApkPath. Run without -SkipBuild first."
}

& $Adb @AdbArgs install -r $ApkPath
if ($LASTEXITCODE -ne 0) { throw "ADB install failed with exit code $LASTEXITCODE." }

& $Adb @AdbArgs shell am force-stop com.tracknrace.android | Out-Null
& $Adb @AdbArgs shell am start -n com.tracknrace.android/.MainActivity
if ($LASTEXITCODE -ne 0) { throw "Could not launch Track N Race on the device." }

Write-Host 'Track N Race installed and launched.' -ForegroundColor Green
Write-Host 'In the F1 game telemetry settings, send UDP to this phone on port 20777.'
