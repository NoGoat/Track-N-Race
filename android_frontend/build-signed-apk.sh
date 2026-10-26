#!/usr/bin/env bash
# build-signed-apk.sh — Build the release APK signed with the release key,
# verify its signature, and place it in release/<version>/ beside the desktop
# builds with its SHA-256 checksum. Linux counterpart of build-signed-apk.ps1.
# Usage: ./build-signed-apk.sh <version-number> [--sdk-root <path>]
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$PROJECT_DIR/.." && pwd)"
VERSION_NUMBER=""
SDK_ROOT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk-root) SDK_ROOT="$2"; shift 2 ;;
        -*) echo "Unknown argument: $1" >&2; exit 1 ;;
        *)
            [[ -z "$VERSION_NUMBER" ]] || { echo "Unexpected argument: $1" >&2; exit 1; }
            VERSION_NUMBER="$1"; shift 1 ;;
    esac
done

cyan()   { printf '\033[36m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
warn()   { printf '\033[33mWARNING: %s\033[0m\n' "$*" >&2; }
die()    { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }

if [[ -z "$VERSION_NUMBER" ]]; then
    echo "Usage: $0 <version-number> [--sdk-root <path>]" >&2
    exit 2
fi
if [[ ! "$VERSION_NUMBER" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "Error: version-number must use major.minor.patch format (for example, 1.2.0)." >&2
    exit 2
fi
# Android orders installs by versionCode, so it must grow with every release:
# 1.6.1 -> 10601. Minor and patch therefore stay below 100.
VERSION_MAJOR=$((10#${BASH_REMATCH[1]}))
VERSION_MINOR=$((10#${BASH_REMATCH[2]}))
VERSION_PATCH=$((10#${BASH_REMATCH[3]}))
if (( VERSION_MINOR > 99 || VERSION_PATCH > 99 )); then
    echo "Error: minor and patch must be below 100 to form the Android versionCode." >&2
    exit 2
fi
VERSION_CODE=$((VERSION_MAJOR * 10000 + VERSION_MINOR * 100 + VERSION_PATCH))

APK_PATH="$PROJECT_DIR/app/build/outputs/apk/release/app-release.apk"
RELEASE_DIR="$ROOT_DIR/release/$VERSION_NUMBER"
RELEASE_APK="$RELEASE_DIR/Track-N-Race-Android.apk"

is_full_android_sdk() {
    local path="$1"
    [[ -n "$path" && -d "$path" ]] || return 1
    # A standalone Platform-Tools zip is useful for ADB but is not an SDK that
    # Gradle can use to provision platforms, CMake or the NDK.
    local marker
    for marker in platforms build-tools cmdline-tools licenses ndk; do
        [[ -e "$path/$marker" ]] && return 0
    done
    return 1
}

find_android_sdk() {
    local candidate
    for candidate in "$SDK_ROOT" "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "$HOME/Android/Sdk"; do
        if is_full_android_sdk "$candidate"; then
            (cd "$candidate" && pwd)
            return
        fi
    done
    die "A complete Android SDK was not found.
Install Android Studio (no emulator required), then use SDK Manager to install:
  - Android SDK Platform 37
  - Android SDK Build-Tools
  - NDK (Side by side) 27.0.12077973
  - CMake 3.22.1
  - Android SDK Command-line Tools (latest)
Accept the SDK licenses, then rerun with:
  ./build-signed-apk.sh $VERSION_NUMBER --sdk-root \"\$HOME/Android/Sdk\""
}

assert_android_sdk_licenses() {
    local android_sdk="$1"
    [[ -s "$android_sdk/licenses/android-sdk-license" ]] && return

    local sdk_manager
    sdk_manager="$(find "$android_sdk/cmdline-tools" -name sdkmanager -type f 2>/dev/null | head -n 1 || true)"
    if [[ -n "$sdk_manager" ]]; then
        die "Android SDK licenses have not been accepted. Review and accept them with:
  \"$sdk_manager\" --sdk_root=\"$android_sdk\" --licenses
Then rerun this script."
    fi
    die 'Android SDK licenses have not been accepted. Accept them in Android Studio SDK Manager, then rerun this script.'
}

assert_android_sdk_packages() {
    local android_sdk="$1"
    local -a missing=()
    [[ -e "$android_sdk/platforms/android-37.0/android.jar" ]] || missing+=('Android SDK Platform 37')
    [[ -e "$android_sdk/ndk/27.0.12077973" ]] || missing+=('NDK 27.0.12077973')
    [[ -e "$android_sdk/cmake/3.22.1" ]] || missing+=('CMake 3.22.1')
    if [[ ${#missing[@]} -gt 0 ]]; then
        local joined
        joined="$(printf '%s, ' "${missing[@]}")"
        die "Missing Android SDK packages: ${joined%, }. Install them with Android Studio SDK Manager, then rerun."
    fi
}

# The newest Build-Tools' apksigner verifies the signature Gradle applied.
find_apksigner() {
    local android_sdk="$1" apksigner
    apksigner="$(ls -1d "$android_sdk"/build-tools/*/apksigner 2>/dev/null | sort -V | tail -n 1 || true)"
    [[ -n "$apksigner" ]] || die 'apksigner was not found. Install Android SDK Build-Tools with Android Studio SDK Manager, then rerun.'
    echo "$apksigner"
}

find_gradle() {
    if [[ -x "$PROJECT_DIR/gradlew" ]]; then
        echo "$PROJECT_DIR/gradlew"
        return
    fi
    command -v gradle 2>/dev/null && return
    die 'Gradle was not found. Install Gradle 9.6+ or generate a Gradle wrapper in android_frontend.'
}

# app/build.gradle pins jvmToolchain(21), and the settings do not enable
# toolchain auto-provisioning, so Gradle needs a local JDK 21. Linux distros
# rarely ship one by default, so check up front instead of failing mid-build.
assert_java() {
    local java_bin="java"
    [[ -n "${JAVA_HOME:-}" ]] && java_bin="$JAVA_HOME/bin/java"
    if ! command -v "$java_bin" >/dev/null 2>&1; then
        die 'A JDK was not found. Install JDK 21 (e.g. sudo apt install openjdk-21-jdk) or set JAVA_HOME, then rerun.'
    fi
    local version
    version="$("$java_bin" -version 2>&1 | sed -n 's/.*version "\([0-9]*\).*/\1/p' | head -n 1)"
    if [[ "$version" != "21" ]]; then
        warn "Gradle is running on JDK ${version:-unknown}; this project's toolchain is JDK 21. If the build cannot find a Java 21 toolchain, install JDK 21 and point JAVA_HOME at it."
    fi
}

properties_value() {
    sed -nE "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*(.*[^[:space:]])[[:space:]]*\$/\1/p" "$1" | tail -n 1
}

find_keytool() {
    if [[ -n "${JAVA_HOME:-}" && -x "$JAVA_HOME/bin/keytool" ]]; then
        echo "$JAVA_HOME/bin/keytool"
        return
    fi
    command -v keytool 2>/dev/null && return
    die 'keytool was not found. Install JDK 21 or set JAVA_HOME, then rerun.'
}

# Mirrors app/build.gradle: the keystore's location and alias come from
# keystore.properties, else TNR_KEYSTORE_FILE and TNR_KEY_ALIAS. Fills
# STORE_FILE and KEY_ALIAS. Checked here so a missing key fails before a long
# build, not after it.
resolve_release_key() {
    local properties="$PROJECT_DIR/keystore.properties"
    if [[ -f "$properties" ]]; then
        STORE_FILE="$(properties_value "$properties" storeFile)"
        KEY_ALIAS="$(properties_value "$properties" keyAlias)"
        [[ -n "$STORE_FILE" ]] || die "keystore.properties has no value for 'storeFile'. See keystore.properties.example."
        [[ -n "$KEY_ALIAS" ]] || die "keystore.properties has no value for 'keyAlias'. See keystore.properties.example."
    else
        STORE_FILE="${TNR_KEYSTORE_FILE:-}"
        KEY_ALIAS="${TNR_KEY_ALIAS:-}"
        [[ -n "$STORE_FILE" && -n "$KEY_ALIAS" ]] ||
            die "The release key is not configured. Create android_frontend/keystore.properties from keystore.properties.example, or set TNR_KEYSTORE_FILE and TNR_KEY_ALIAS."
    fi
    [[ "$STORE_FILE" = /* ]] || STORE_FILE="$PROJECT_DIR/$STORE_FILE"
    [[ -f "$STORE_FILE" ]] || die "The release keystore was not found at $STORE_FILE."
}

# The passwords are never stored in a file. They are asked for on each run and
# reach Gradle as environment variables, which, unlike -P arguments, other
# users cannot see in the process list.
prompt_release_passwords() {
    [[ -t 0 ]] || die 'The keystore passwords are entered interactively; run this script from a terminal.'
    read -rsp "Keystore password: " TNR_KEYSTORE_PASSWORD; echo
    [[ -n "$TNR_KEYSTORE_PASSWORD" ]] || die 'The keystore password is empty.'
    read -rsp "Key password (Enter if the same as the keystore password): " TNR_KEY_PASSWORD; echo
    [[ -n "$TNR_KEY_PASSWORD" ]] || TNR_KEY_PASSWORD="$TNR_KEYSTORE_PASSWORD"
    export TNR_KEYSTORE_FILE="$STORE_FILE" TNR_KEY_ALIAS="$KEY_ALIAS" TNR_KEYSTORE_PASSWORD TNR_KEY_PASSWORD

    # A wrong keystore password or alias fails here, not after the build.
    "$KEYTOOL" -list -keystore "$STORE_FILE" -alias "$KEY_ALIAS" -storepass:env TNR_KEYSTORE_PASSWORD >/dev/null 2>&1 ||
        die "The keystore password is wrong, or the keystore has no key with the alias '$KEY_ALIAS'."
}

[[ ! -e "$RELEASE_APK" ]] || die "Release APK already exists: $RELEASE_APK"

STORE_FILE=""
KEY_ALIAS=""
resolve_release_key
RESOLVED_SDK="$(find_android_sdk)"
export ANDROID_SDK_ROOT="$RESOLVED_SDK"
export ANDROID_HOME="$RESOLVED_SDK"
assert_android_sdk_licenses "$RESOLVED_SDK"
assert_android_sdk_packages "$RESOLVED_SDK"
assert_java
APKSIGNER="$(find_apksigner "$RESOLVED_SDK")"
GRADLE="$(find_gradle)"
KEYTOOL="$(find_keytool)"
prompt_release_passwords

# A stale APK from an earlier build must not pass for this one.
rm -f "$APK_PATH"

cyan "Building the signed Android release APK $VERSION_NUMBER (versionCode $VERSION_CODE)..."
"$GRADLE" --no-daemon -p "$PROJECT_DIR" :app:assembleRelease \
    "-PtnrVersionName=$VERSION_NUMBER" "-PtnrVersionCode=$VERSION_CODE" ||
    die "Gradle build failed with exit code $?."

[[ -f "$APK_PATH" ]] || die "The release APK was not produced at $APK_PATH."

cyan "Verifying the APK signature..."
CERTS="$("$APKSIGNER" verify --verbose --print-certs "$APK_PATH")" ||
    die "apksigner could not verify $APK_PATH; it is unsigned or its signature is invalid."
if grep -q 'CN=Android Debug' <<< "$CERTS"; then
    die "$APK_PATH is signed with the Android debug key, not the release key."
fi
CERT_SHA256="$(sed -nE 's/^Signer #1 certificate SHA-256 digest: ([0-9a-f]+)$/\1/p' <<< "$CERTS" | head -n 1)"

mkdir -p "$RELEASE_DIR"
cp "$APK_PATH" "$RELEASE_APK"
(cd "$RELEASE_DIR" && sha256sum "$(basename "$RELEASE_APK")" > "$(basename "$RELEASE_APK").sha256")

echo
green "Android release complete: $RELEASE_APK"
echo "APK SHA-256:         $(cut -d' ' -f1 "$RELEASE_APK.sha256")"
echo "Signing certificate: ${CERT_SHA256:-see apksigner output above}"
echo 'Publish the APK SHA-256 beside the download. Register the signing certificate'
echo 'SHA-256 for com.nogoat.tracknrace in the Android Developer Console.'
