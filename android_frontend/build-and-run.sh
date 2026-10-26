#!/usr/bin/env bash
# build-and-run.sh — Build Track N Race for Android and install it on every
# connected phone. Linux counterpart of build-and-run.ps1.
# Usage: ./build-and-run.sh [--device-serial <serial>] [--sdk-root <path>]
#                           [--skip-build] [--build-type Debug|Release]
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_SERIAL=""
SDK_ROOT=""
SKIP_BUILD=false
BUILD_TYPE="Debug"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device-serial) DEVICE_SERIAL="$2"; shift 2 ;;
        --sdk-root) SDK_ROOT="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=true; shift 1 ;;
        --build-type)
            case "$2" in
                Debug|debug) BUILD_TYPE="Debug" ;;
                Release|release) BUILD_TYPE="Release" ;;
                *) echo "--build-type must be Debug or Release" >&2; exit 1 ;;
            esac
            shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

BUILD_TYPE_LOWER="${BUILD_TYPE,,}"
APK_PATH="$PROJECT_DIR/app/build/outputs/apk/$BUILD_TYPE_LOWER/app-$BUILD_TYPE_LOWER.apk"

cyan()   { printf '\033[36m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
warn()   { printf '\033[33mWARNING: %s\033[0m\n' "$*" >&2; }
die()    { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }

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

    local adb_note=""
    if command -v adb >/dev/null 2>&1; then
        adb_note=$'\n'"ADB was found at '$(command -v adb)', but its standalone Platform-Tools folder is not a complete Android SDK."
    fi
    die "A complete Android SDK was not found.$adb_note
Install Android Studio (no emulator required), then use SDK Manager to install:
  - Android SDK Platform 37
  - Android SDK Build-Tools
  - Android SDK Platform-Tools
  - NDK (Side by side) 27.0.12077973
  - CMake 3.22.1
  - Android SDK Command-line Tools (latest)
Accept the SDK licenses, then rerun with:
  ./build-and-run.sh --sdk-root \"\$HOME/Android/Sdk\""
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

# Prints the binary of an already-running adb server, if any. A client whose
# version differs from the server kills and restarts it, which drops every
# Wireless Debugging connection, so reuse whichever adb owns the server.
find_running_adb_server() {
    local pid exe
    for pid in $(pgrep -x adb 2>/dev/null); do
        tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q 'fork-server server' || continue
        exe="$(readlink "/proc/$pid/exe" 2>/dev/null)" || continue
        [[ -x "$exe" ]] && { echo "$exe"; return 0; }
    done
    return 1
}

find_adb() {
    local android_sdk="$1"
    find_running_adb_server && return
    if [[ -n "$android_sdk" && -x "$android_sdk/platform-tools/adb" ]]; then
        echo "$android_sdk/platform-tools/adb"
        return
    fi
    command -v adb 2>/dev/null && return
    # Linux SDK installs rarely put platform-tools on PATH, so --skip-build
    # also checks the usual SDK locations.
    local candidate
    for candidate in "$SDK_ROOT" "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "$HOME/Android/Sdk"; do
        if [[ -n "$candidate" && -x "$candidate/platform-tools/adb" ]]; then
            echo "$candidate/platform-tools/adb"
            return
        fi
    done
    die 'adb was not found. Install Android SDK Platform-Tools or set ANDROID_SDK_ROOT.'
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

assert_android_sdk_packages() {
    local android_sdk="$1"
    local -a missing=()
    [[ -e "$android_sdk/platforms/android-37.0/android.jar" ]] || missing+=('Android SDK Platform 37')
    [[ -e "$android_sdk/ndk/27.0.12077973" ]] || missing+=('NDK 27.0.12077973')
    [[ -e "$android_sdk/cmake/3.22.1" ]] || missing+=('CMake 3.22.1')
    [[ -e "$android_sdk/platform-tools/adb" ]] || missing+=('Android SDK Platform-Tools')
    if [[ ${#missing[@]} -gt 0 ]]; then
        local joined
        joined="$(printf '%s, ' "${missing[@]}")"
        die "Missing Android SDK packages: ${joined%, }. Install them with Android Studio SDK Manager, then rerun."
    fi
}

get_authorized_devices() {
    "$1" devices | tail -n +2 | sed -nE 's/^([^[:space:]]+)[[:space:]]+device([[:space:]]|$).*/\1/p'
}

get_wireless_debugging_endpoints() {
    local output
    output="$("$1" mdns services 2>/dev/null)" || return 0
    # Typical output is: <service name> _adb-tls-connect._tcp. <ip>:<port>
    sed -nE 's/.*_adb-tls-connect\._tcp\.?[[:space:]]+([^[:space:]]+:[0-9]+)[[:space:]]*$/\1/p' <<< "$output" | sort -u
}

connect_wireless_devices() {
    local adb_path="$1"
    local -a endpoints
    mapfile -t endpoints < <(get_wireless_debugging_endpoints "$adb_path")
    [[ ${#endpoints[@]} -eq 0 ]] && return

    yellow 'No authorized Android device found. Trying paired Wireless Debugging devices...'
    local endpoint
    for endpoint in "${endpoints[@]}"; do
        echo "  Connecting to $endpoint"
        "$adb_path" connect "$endpoint" || true
    done
}

contains() {
    local needle="$1"; shift
    local item
    for item in "$@"; do [[ "$item" == "$needle" ]] && return 0; done
    return 1
}

# Fills RESOLVED_DEVICE_SERIALS.
resolve_device_serials() {
    local adb_path="$1" requested_serial="$2"
    local -a devices
    if [[ -n "$requested_serial" ]]; then
        mapfile -t devices < <(get_authorized_devices "$adb_path")
        if ! contains "$requested_serial" "${devices[@]}" && [[ "$requested_serial" =~ :[0-9]+$ ]]; then
            "$adb_path" connect "$requested_serial" || true
            mapfile -t devices < <(get_authorized_devices "$adb_path")
        fi
        if ! contains "$requested_serial" "${devices[@]}"; then
            die "Device '$requested_serial' is not connected and authorized."
        fi
        RESOLVED_DEVICE_SERIALS=("$requested_serial")
        return
    fi

    mapfile -t devices < <(get_authorized_devices "$adb_path")
    if [[ ${#devices[@]} -eq 0 ]]; then
        connect_wireless_devices "$adb_path"
        mapfile -t devices < <(get_authorized_devices "$adb_path")
    fi

    mapfile -t devices < <(printf '%s\n' "${devices[@]}" | sed '/^$/d' | sort -u)
    if [[ ${#devices[@]} -gt 0 ]]; then
        RESOLVED_DEVICE_SERIALS=("${devices[@]}")
        return
    fi

    die "No authorized Android device was found over USB or Wireless Debugging.
On the phone, enable Developer options > Wireless debugging and keep the phone and PC on the same network.
For first-time setup, choose 'Pair device with pairing code' and run:
  adb pair <phone-ip>:<pairing-port>
Then rerun this script. If mDNS discovery is unavailable, pass the phone's Wireless debugging IP and port:
  ./build-and-run.sh --device-serial '<phone-ip>:<connect-port>'"
}

RESOLVED_SDK=""
if ! $SKIP_BUILD; then
    RESOLVED_SDK="$(find_android_sdk)"
    export ANDROID_SDK_ROOT="$RESOLVED_SDK"
    export ANDROID_HOME="$RESOLVED_SDK"
    assert_android_sdk_licenses "$RESOLVED_SDK"
    assert_android_sdk_packages "$RESOLVED_SDK"
    assert_java
fi
ADB="$(find_adb "$RESOLVED_SDK")"

"$ADB" start-server >/dev/null
RESOLVED_DEVICE_SERIALS=()
resolve_device_serials "$ADB" "$DEVICE_SERIAL"
if [[ ${#RESOLVED_DEVICE_SERIALS[@]} -eq 1 ]]; then
    echo "Using Android device ${RESOLVED_DEVICE_SERIALS[0]}"
else
    echo "Using ${#RESOLVED_DEVICE_SERIALS[@]} Android devices: $(IFS=,; echo "${RESOLVED_DEVICE_SERIALS[*]}" | sed 's/,/, /g')"
fi

if ! $SKIP_BUILD; then
    GRADLE="$(find_gradle)"
    GRADLE_ARGS=(--no-daemon -p "$PROJECT_DIR" ":app:assemble$BUILD_TYPE")
    if [[ "$BUILD_TYPE" == "Release" ]]; then
        # Local release builds never use the release key. Opt in to the standard Android
        # debug key for this local build-and-run workflow only.
        GRADLE_ARGS+=(-PtnrLocalReleaseSigning)
    fi
    # Emulator images are x86_64; include that ABI only when one is targeted.
    for serial in "${RESOLVED_DEVICE_SERIALS[@]}"; do
        if [[ "$("$ADB" -s "$serial" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')" == "x86_64" ]]; then
            GRADLE_ARGS+=(-PtnrX86)
            break
        fi
    done

    cyan "Building the native Jetpack Compose Android $BUILD_TYPE_LOWER APK..."
    "$GRADLE" "${GRADLE_ARGS[@]}" || die "Gradle build failed with exit code $?."
fi

[[ -f "$APK_PATH" ]] || die "APK not found at $APK_PATH. Run without --skip-build first."

DEVICE_FAILURES=()
for serial in "${RESOLVED_DEVICE_SERIALS[@]}"; do
    ADB_ARGS=(-s "$serial")
    cyan "Installing Track N Race on $serial..."
    if ! "$ADB" "${ADB_ARGS[@]}" install -r "$APK_PATH"; then
        DEVICE_FAILURES+=("$serial (install failed)")
        warn "Install failed on $serial; continuing with the remaining devices."
        continue
    fi

    if [[ "$BUILD_TYPE" == "Release" ]]; then
        # A sideloaded APK runs interpreted and JIT-compiled until Android's
        # idle-and-charging dexopt job compiles it, which can be a day later.
        # On a low-end phone that is a 20-30% slower UI; compile it now.
        cyan "Compiling Track N Race ahead of time on $serial..."
        if ! "$ADB" "${ADB_ARGS[@]}" shell cmd package compile -m speed -f com.nogoat.tracknrace >/dev/null; then
            warn "Ahead-of-time compilation failed on $serial; the app will still run, but slower until Android compiles it."
        fi
    fi

    "$ADB" "${ADB_ARGS[@]}" shell am force-stop com.nogoat.tracknrace >/dev/null || true
    if ! "$ADB" "${ADB_ARGS[@]}" shell am start -n com.nogoat.tracknrace/.MainActivity; then
        DEVICE_FAILURES+=("$serial (launch failed)")
        warn "Launch failed on $serial; continuing with the remaining devices."
        continue
    fi

    green "Track N Race installed and launched on $serial."
done

if [[ ${#DEVICE_FAILURES[@]} -gt 0 ]]; then
    joined="$(printf '%s, ' "${DEVICE_FAILURES[@]}")"
    die "Install or launch failed for: ${joined%, }."
fi

green "Track N Race installed and launched on ${#RESOLVED_DEVICE_SERIALS[@]} device(s)."
echo 'In the F1 game telemetry settings, send UDP to this phone on port 20777.'
