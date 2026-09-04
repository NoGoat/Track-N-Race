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
  - Android SDK Platform 36
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
    throw 'Gradle was not found. Install Gradle 8.13+ or generate a Gradle wrapper in android_frontend.'
}

function Assert-AndroidSdkPackages([string]$AndroidSdk) {
    $required = @(
        @{ Path = 'platforms\android-36\android.jar'; Name = 'Android SDK Platform 36' },
        @{ Path = 'ndk\27.0.12077973'; Name = 'NDK 27.0.12077973' },
        @{ Path = 'cmake\3.22.1'; Name = 'CMake 3.22.1' },
        @{ Path = 'platform-tools\adb.exe'; Name = 'Android SDK Platform-Tools' }
    )
    $missing = @($required | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $AndroidSdk $_.Path))
    } | ForEach-Object { $_.Name })
    if ($missing.Count -gt 0) {
        throw "Missing Android SDK packages: $($missing -join ', '). Install them with Android Studio SDK Manager, then rerun."
    }
}

function Find-Npm {
    $node = Get-Command node -ErrorAction SilentlyContinue
    $npm = Get-Command npm.cmd -ErrorAction SilentlyContinue
    if (-not $node -or -not $npm) {
        throw 'Node.js 22.12+ and npm are required to build the React/Capacitor frontend.'
    }

    $nodeVersionText = & $node.Source -p 'process.versions.node'
    if ($LASTEXITCODE -ne 0) { throw 'Could not determine the installed Node.js version.' }
    $nodeVersion = [Version]$nodeVersionText
    if ($nodeVersion -lt [Version]'22.12.0') {
        throw "Node.js 22.12+ is required; found $nodeVersionText."
    }
    return $npm.Source
}

function Get-AuthorizedDevices([string]$AdbPath) {
    return @(& $AdbPath devices |
        Select-Object -Skip 1 |
        ForEach-Object {
            if ($_ -match '^([^\s]+)\s+device(?:\s|$)') { $Matches[1] }
        })
}

function Get-WirelessDebuggingEndpoints([string]$AdbPath) {
    $output = @(& $AdbPath mdns services 2>$null)
    if ($LASTEXITCODE -ne 0) { return @() }

    return @($output |
        ForEach-Object {
            # Typical output is: <service name> _adb-tls-connect._tcp. <ip>:<port>
            if ($_ -match '_adb-tls-connect\._tcp\.?\s+([^\s]+:\d+)\s*$') { $Matches[1] }
        } |
        Sort-Object -Unique)
}

function Connect-WirelessDevices([string]$AdbPath) {
    $endpoints = @(Get-WirelessDebuggingEndpoints $AdbPath)
    if ($endpoints.Count -eq 0) { return }

    Write-Host 'No authorized Android device found. Trying paired Wireless Debugging devices...' -ForegroundColor Yellow
    foreach ($endpoint in $endpoints) {
        Write-Host "  Connecting to $endpoint"
        & $AdbPath connect $endpoint | Out-Host
    }
}

function Resolve-DeviceSerials([string]$AdbPath, [string]$RequestedSerial) {
    if ($RequestedSerial) {
        $devices = @(Get-AuthorizedDevices $AdbPath)
        if ($RequestedSerial -notin $devices -and $RequestedSerial -match ':\d+$') {
            & $AdbPath connect $RequestedSerial | Out-Host
            $devices = @(Get-AuthorizedDevices $AdbPath)
        }
        if ($RequestedSerial -notin $devices) {
            throw "Device '$RequestedSerial' is not connected and authorized."
        }
        return @($RequestedSerial)
    }

    $devices = @(Get-AuthorizedDevices $AdbPath)
    if ($devices.Count -eq 0) {
        Connect-WirelessDevices $AdbPath
        $devices = @(Get-AuthorizedDevices $AdbPath)
    }

    $devices = @($devices | Sort-Object -Unique)
    if ($devices.Count -gt 0) { return $devices }

    throw @"
No authorized Android device was found over USB or Wireless Debugging.
On the phone, enable Developer options > Wireless debugging and keep the phone and PC on the same network.
For first-time setup, choose 'Pair device with pairing code' and run:
  adb pair <phone-ip>:<pairing-port>
Then rerun this script. If mDNS discovery is unavailable, pass the phone's Wireless debugging IP and port:
  .\build-and-run.ps1 -DeviceSerial '<phone-ip>:<connect-port>'
"@
}

$ResolvedSdk = $null
if (-not $SkipBuild) {
    $ResolvedSdk = Find-AndroidSdk
    $env:ANDROID_SDK_ROOT = $ResolvedSdk
    $env:ANDROID_HOME = $ResolvedSdk
    Assert-AndroidSdkLicenses $ResolvedSdk
    Assert-AndroidSdkPackages $ResolvedSdk
}
$Adb = Find-Adb $ResolvedSdk

& $Adb start-server | Out-Null
$ResolvedDeviceSerials = @(Resolve-DeviceSerials $Adb $DeviceSerial)
if ($ResolvedDeviceSerials.Count -eq 1) {
    Write-Host "Using Android device $($ResolvedDeviceSerials[0])"
} else {
    Write-Host "Using $($ResolvedDeviceSerials.Count) Android devices: $($ResolvedDeviceSerials -join ', ')"
}

if (-not $SkipBuild) {
    $Npm = Find-Npm
    Write-Host 'Installing React/Capacitor dependencies...' -ForegroundColor Cyan
    & $Npm --prefix $ProjectDir install --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw "npm install failed with exit code $LASTEXITCODE." }

    Write-Host 'Building and syncing the Capacitor web frontend...' -ForegroundColor Cyan
    & $Npm --prefix $ProjectDir run build
    if ($LASTEXITCODE -ne 0) { throw "Web frontend build failed with exit code $LASTEXITCODE." }

    $Gradle = Find-Gradle
    Write-Host 'Building the Android APK...' -ForegroundColor Cyan
    & $Gradle --no-daemon -p $ProjectDir :app:assembleDebug
    if ($LASTEXITCODE -ne 0) { throw "Gradle build failed with exit code $LASTEXITCODE." }
}

if (-not (Test-Path -LiteralPath $ApkPath)) {
    throw "APK not found at $ApkPath. Run without -SkipBuild first."
}

$deviceFailures = @()
foreach ($serial in $ResolvedDeviceSerials) {
    $AdbArgs = @('-s', $serial)
    Write-Host "Installing Track N Race on $serial..." -ForegroundColor Cyan
    & $Adb @AdbArgs install -r $ApkPath
    if ($LASTEXITCODE -ne 0) {
        $deviceFailures += "$serial (install exit code $LASTEXITCODE)"
        Write-Warning "Install failed on $serial; continuing with the remaining devices."
        continue
    }

    & $Adb @AdbArgs shell am force-stop com.tracknrace.android | Out-Null
    & $Adb @AdbArgs shell am start -n com.tracknrace.android/.MainActivity
    if ($LASTEXITCODE -ne 0) {
        $deviceFailures += "$serial (launch exit code $LASTEXITCODE)"
        Write-Warning "Launch failed on $serial; continuing with the remaining devices."
        continue
    }

    Write-Host "Track N Race installed and launched on $serial." -ForegroundColor Green
}

if ($deviceFailures.Count -gt 0) {
    throw "Install or launch failed for: $($deviceFailures -join ', ')."
}

Write-Host "Track N Race installed and launched on $($ResolvedDeviceSerials.Count) device(s)." -ForegroundColor Green
Write-Host 'In the F1 game telemetry settings, send UDP to this phone on port 20777.'
