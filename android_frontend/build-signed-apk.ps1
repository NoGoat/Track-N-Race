[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$VersionNumber,
    [string]$SdkRoot
)

# Build the release APK signed with the release key, verify its signature, and
# place it in release\<version>\ beside the desktop builds with its SHA-256
# checksum. Windows counterpart of build-signed-apk.sh.

$ErrorActionPreference = 'Stop'
$ProjectDir = $PSScriptRoot
$RootDir = Split-Path -Parent $ProjectDir

if ($VersionNumber -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw 'version-number must use major.minor.patch format (for example, 1.2.0).'
}
# Android orders installs by versionCode, so it must grow with every release:
# 1.6.1 -> 10601. Minor and patch therefore stay below 100.
$VersionMajor = [int]$Matches[1]
$VersionMinor = [int]$Matches[2]
$VersionPatch = [int]$Matches[3]
if ($VersionMinor -gt 99 -or $VersionPatch -gt 99) {
    throw 'Minor and patch must be below 100 to form the Android versionCode.'
}
$VersionCode = $VersionMajor * 10000 + $VersionMinor * 100 + $VersionPatch

$ApkPath = Join-Path $ProjectDir 'app\build\outputs\apk\release\app-release.apk'
$ReleaseDir = Join-Path $RootDir "release\$VersionNumber"
$ReleaseApk = Join-Path $ReleaseDir 'Track-N-Race-Android.apk'

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
    throw @"
A complete Android SDK was not found.
Install Android Studio (no emulator required), then use SDK Manager to install:
  - Android SDK Platform 37
  - Android SDK Build-Tools
  - NDK (Side by side) 27.0.12077973
  - CMake 3.22.1
  - Android SDK Command-line Tools (latest)
Accept the SDK licenses, then rerun with:
  .\build-signed-apk.ps1 $VersionNumber -SdkRoot "$env:LOCALAPPDATA\Android\Sdk"
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

function Assert-AndroidSdkPackages([string]$AndroidSdk) {
    $required = @(
        @{ Path = 'platforms\android-37.0\android.jar'; Name = 'Android SDK Platform 37' },
        @{ Path = 'ndk\27.0.12077973'; Name = 'NDK 27.0.12077973' },
        @{ Path = 'cmake\3.22.1'; Name = 'CMake 3.22.1' }
    )
    $missing = @($required | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $AndroidSdk $_.Path))
    } | ForEach-Object { $_.Name })
    if ($missing.Count -gt 0) {
        throw "Missing Android SDK packages: $($missing -join ', '). Install them with Android Studio SDK Manager, then rerun."
    }
}

# The newest Build-Tools' apksigner verifies the signature Gradle applied.
function Find-ApkSigner([string]$AndroidSdk) {
    $apksigner = Get-ChildItem -LiteralPath (Join-Path $AndroidSdk 'build-tools') -Directory -ErrorAction SilentlyContinue |
        Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } |
        ForEach-Object { Join-Path $_.FullName 'apksigner.bat' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -Last 1
    if (-not $apksigner) {
        throw 'apksigner was not found. Install Android SDK Build-Tools with Android Studio SDK Manager, then rerun.'
    }
    return $apksigner
}

function Find-Gradle {
    $localWrapper = Join-Path $ProjectDir 'gradlew.bat'
    if (Test-Path -LiteralPath $localWrapper) { return $localWrapper }
    $fromPath = Get-Command gradle -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }
    throw 'Gradle was not found. Install Gradle 9.6+ or generate a Gradle wrapper in android_frontend.'
}

function Find-Keytool {
    if ($env:JAVA_HOME) {
        $fromJavaHome = Join-Path $env:JAVA_HOME 'bin\keytool.exe'
        if (Test-Path -LiteralPath $fromJavaHome) { return $fromJavaHome }
    }
    $fromPath = Get-Command keytool -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }
    throw 'keytool was not found. Install JDK 21 or set JAVA_HOME, then rerun.'
}

# Mirrors app/build.gradle: the keystore's location and alias come from
# keystore.properties, else TNR_KEYSTORE_FILE and TNR_KEY_ALIAS. Checked here
# so a missing key fails before a long build, not after it.
function Resolve-ReleaseKey {
    $propertiesFile = Join-Path $ProjectDir 'keystore.properties'
    if (Test-Path -LiteralPath $propertiesFile) {
        $values = @{}
        foreach ($line in Get-Content -LiteralPath $propertiesFile) {
            if ($line -match '^\s*([^#!=\s]+)\s*=\s*(.*?)\s*$') { $values[$Matches[1]] = $Matches[2] }
        }
        foreach ($key in 'storeFile', 'keyAlias') {
            if (-not $values[$key]) { throw "keystore.properties has no value for '$key'. See keystore.properties.example." }
        }
        $storeFile = $values['storeFile']
        $keyAlias = $values['keyAlias']
    } else {
        if (-not $env:TNR_KEYSTORE_FILE -or -not $env:TNR_KEY_ALIAS) {
            throw 'The release key is not configured. Create android_frontend\keystore.properties from keystore.properties.example, or set TNR_KEYSTORE_FILE and TNR_KEY_ALIAS.'
        }
        $storeFile = $env:TNR_KEYSTORE_FILE
        $keyAlias = $env:TNR_KEY_ALIAS
    }
    if (-not [System.IO.Path]::IsPathRooted($storeFile)) { $storeFile = Join-Path $ProjectDir $storeFile }
    if (-not (Test-Path -LiteralPath $storeFile -PathType Leaf)) {
        throw "The release keystore was not found at $storeFile."
    }
    return @{ StoreFile = $storeFile; KeyAlias = $keyAlias }
}

function Read-Password([string]$Prompt) {
    $secure = Read-Host -Prompt $Prompt -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

$SigningVariables = 'TNR_KEYSTORE_FILE', 'TNR_KEY_ALIAS', 'TNR_KEYSTORE_PASSWORD', 'TNR_KEY_PASSWORD'

if (Test-Path -LiteralPath $ReleaseApk) { throw "Release APK already exists: $ReleaseApk" }

$ReleaseKey = Resolve-ReleaseKey
$ResolvedSdk = Find-AndroidSdk
$env:ANDROID_SDK_ROOT = $ResolvedSdk
$env:ANDROID_HOME = $ResolvedSdk
Assert-AndroidSdkLicenses $ResolvedSdk
Assert-AndroidSdkPackages $ResolvedSdk
$ApkSigner = Find-ApkSigner $ResolvedSdk
$Gradle = Find-Gradle
$Keytool = Find-Keytool

# A stale APK from an earlier build must not pass for this one.
Remove-Item -LiteralPath $ApkPath -ErrorAction SilentlyContinue

# The passwords are never stored in a file. They are asked for on each run and
# reach Gradle as environment variables, which, unlike -P arguments, other
# users cannot see in the process list. $env: outlives the script in the
# calling PowerShell session, so they are removed again in finally.
try {
    $env:TNR_KEYSTORE_PASSWORD = Read-Password 'Keystore password'
    if (-not $env:TNR_KEYSTORE_PASSWORD) { throw 'The keystore password is empty.' }
    $env:TNR_KEY_PASSWORD = Read-Password 'Key password (Enter if the same as the keystore password)'
    if (-not $env:TNR_KEY_PASSWORD) { $env:TNR_KEY_PASSWORD = $env:TNR_KEYSTORE_PASSWORD }
    $env:TNR_KEYSTORE_FILE = $ReleaseKey.StoreFile
    $env:TNR_KEY_ALIAS = $ReleaseKey.KeyAlias

    # A wrong keystore password or alias fails here, not after the build.
    & $Keytool -list -keystore $ReleaseKey.StoreFile -alias $ReleaseKey.KeyAlias -storepass:env TNR_KEYSTORE_PASSWORD *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "The keystore password is wrong, or the keystore has no key with the alias '$($ReleaseKey.KeyAlias)'."
    }

    Write-Host "Building the signed Android release APK $VersionNumber (versionCode $VersionCode)..." -ForegroundColor Cyan
    & $Gradle --no-daemon -p $ProjectDir ':app:assembleRelease' "-PtnrVersionName=$VersionNumber" "-PtnrVersionCode=$VersionCode"
    if ($LASTEXITCODE -ne 0) { throw "Gradle build failed with exit code $LASTEXITCODE." }
} finally {
    foreach ($name in $SigningVariables) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
}
if (-not (Test-Path -LiteralPath $ApkPath)) { throw "The release APK was not produced at $ApkPath." }

Write-Host 'Verifying the APK signature...' -ForegroundColor Cyan
$certs = (& $ApkSigner verify --verbose --print-certs $ApkPath | Out-String)
if ($LASTEXITCODE -ne 0) { throw "apksigner could not verify $ApkPath; it is unsigned or its signature is invalid." }
if ($certs -match 'CN=Android Debug') { throw "$ApkPath is signed with the Android debug key, not the release key." }
$certSha256 = if ($certs -match '(?m)^Signer #1 certificate SHA-256 digest: ([0-9a-f]+)\s*$') { $Matches[1] } else { 'see apksigner output above' }

New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null
Copy-Item -LiteralPath $ApkPath -Destination $ReleaseApk
$apkSha256 = (Get-FileHash -LiteralPath $ReleaseApk -Algorithm SHA256).Hash.ToLowerInvariant()
# sha256sum's format, so `sha256sum -c` checks it on Linux too.
Set-Content -LiteralPath "$ReleaseApk.sha256" -Value "$apkSha256  Track-N-Race-Android.apk"

Write-Host ''
Write-Host "Android release complete: $ReleaseApk" -ForegroundColor Green
Write-Host "APK SHA-256:         $apkSha256"
Write-Host "Signing certificate: $certSha256"
Write-Host 'Publish the APK SHA-256 beside the download. Register the signing certificate'
Write-Host 'SHA-256 for com.nogoat.tracknrace in the Android Developer Console.'
