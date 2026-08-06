# Track N Race Android frontend

The first Android screen is the live Standings/Timing Tower. It hosts the
repository's existing C++ `protocol_parser_library` through JNI, listens for F1
UDP telemetry on port 20777, and renders timing, participants, tyre compounds,
fastest lap and penalty state as Material 2 driver cards designed for a phone.
Persistent bottom navigation provides homes for Overview, Standings, Session,
Strategy and the remaining dashboard pages under More.

## Build and run on a physical phone

Prerequisites on the PC:

- JDK 17
- Android SDK Platform 35 and Build-Tools
- Android SDK Platform-Tools and Command-line Tools (latest)
- NDK (Side by side) 27.0.12077973 and CMake 3.22.1
- Gradle 8.9 or newer on `PATH` (or generate a Gradle wrapper here)
- USB debugging or wireless debugging connected and authorized

From PowerShell:

```powershell
.\android_frontend\build-and-run.ps1
```

If the SDK is not in the default `%LOCALAPPDATA%\Android\Sdk` location:

```powershell
.\android_frontend\build-and-run.ps1 -SdkRoot 'D:\Android\Sdk'
```

The standalone Platform-Tools download is not a full Android SDK. Put Google's
Command-line Tools under `Sdk\cmdline-tools\latest`, install the components
listed above with `sdkmanager`, and accept their licences from PowerShell:

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\cmdline-tools\latest\bin\sdkmanager.bat" --licenses
```

If more than one ADB device is connected:

```powershell
.\android_frontend\build-and-run.ps1 -DeviceSerial '<adb serial>'
```

The first native build downloads the dependencies already pinned by
`protocol_parser_library` (glaze, zlib, Zstandard and libxlsxwriter), so it
requires internet access and takes longer than subsequent builds.

## Phone and game network setup

Keep the phone and the PC/console running F1 on the same local network. In the
game's Telemetry settings, enable UDP telemetry, set the destination IP to the
phone's Wi-Fi IPv4 address and the port to `20777`. Keep the app in the
foreground while receiving; Android may suspend its listener in the background.

No runtime permission prompt is expected. The app only declares Android's
normal `INTERNET` permission, which also permits local UDP sockets.
