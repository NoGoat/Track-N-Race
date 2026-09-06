# Track N Race Android frontend

The Android app is a native Kotlin/Jetpack Compose Material 3 application. It
uses the shared C++ `protocol_parser_library` through a Kotlin/JNI
host, supports direct UDP telemetry and paired desktop telemetry, and records
TNRD V5 sessions through the shared engine.

There is no WebView, JavaScript runtime, Capacitor bridge, React bundle, or
per-packet Compose state update. Native and paired binary batches are decoded
on their worker threads. Only the newest telemetry sample is atomically
published, and the visible dashboard samples it once per display frame. Cold
rows update Compose snapshot state on the main thread at their native cadence.

Material 3 provides the UI components and dynamic Material You color is used
on Android 12 and later, with light/dark fallback schemes on older devices.
Landscape mode keeps the existing immersive steering-wheel presentation.

Recording is disabled by default. Sessions are staged in the app's external
Documents directory under `Track N Race/`; users may select another directory
with Android's system folder picker. Desktop discovery, pairing, QR scanning,
saved preferences, and recording storage remain native Android code.

## Build and run on a physical phone

Prerequisites:

- JDK 21
- Gradle 8.13 or newer on `PATH` (or a Gradle wrapper in this directory)
- Android SDK Platform 36 and Build-Tools
- Android SDK Platform-Tools and Command-line Tools
- NDK 27.0.12077973 and CMake 3.22.1
- USB debugging, or Android 11+ Wireless debugging

From PowerShell:

```powershell
.\android_frontend\build-and-run.ps1
```

For a non-default SDK location:

```powershell
.\android_frontend\build-and-run.ps1 -SdkRoot 'D:\Android\Sdk'
```

For a specific connected device:

```powershell
.\android_frontend\build-and-run.ps1 -DeviceSerial '<adb serial>'
```

The script builds the Kotlin/Compose app and native C++ library, assembles the
debug APK, installs it on every selected device, and launches the activity.

## Phone and game network setup

Keep the phone and the PC/console running F1 on the same local network. Enable
UDP telemetry in the game, set the destination to the phone's Wi-Fi IPv4
address, and use port `20777`. Keep the dashboard in the foreground while
receiving; Android may suspend its listener in the background.

The app uses scoped storage. It declares normal network and camera permissions;
selecting a recording destination grants access only to the chosen folder.
