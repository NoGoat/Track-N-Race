# Track N Race Android frontend

The Android app uses a React/Vite frontend hosted by Capacitor and keeps the
existing native telemetry backend. The direct source still runs the shared C++
`protocol_parser_library` through JNI; paired mode still consumes the desktop
WebSocket stream and uses the existing Android discovery, pairing, and storage
implementations.

Hot telemetry does not use Capacitor's JSON plugin bridge. Native code batches
the existing `BinaryRows.h` wire records and posts them directly to the WebView
as an `ArrayBuffer`. The TypeScript side decodes every row with the Electron
binary decoder into a mutable dashboard state. There is no ingest timer,
30 FPS gate, or latest-value replacement. Canvas rendering follows the display
refresh rate while ingestion continues independently; an explicit overrun
status is reported if the bounded 8 MiB transport buffer ever fills.

Recording is disabled by default and configured from the dashboard's full-page
Settings screen, matching the Electron app's persisted opt-in behavior. When
enabled, the setting calls the shared engine's TNRD V5 writer through JNI. The
writer stages sessions in the app's external Documents directory under
`Track N Race/`; the Android host creates and verifies that folder before it
enables the native writer. Users can optionally select another folder with
Android's system folder picker, and finalized recordings are moved there.
Recording errors from the native disk thread are shown in the dashboard.

The React UI includes the live dashboard, telemetry and recording settings,
desktop discovery/QR/manual-code pairing, and open-source notices.

## Build and run on a physical phone

Prerequisites on the PC:

- Node.js 22.12+ and npm
- JDK 21
- Android SDK Platform 36 and Build-Tools
- Android SDK Platform-Tools and Command-line Tools (latest)
- NDK (Side by side) 27.0.12077973 and CMake 3.22.1
- Gradle 8.13 or newer on `PATH` (or generate a Gradle wrapper here)
- USB debugging, or Android 11+ Wireless debugging enabled and paired

From PowerShell:

```powershell
.\android_frontend\build-and-run.ps1
```

If the SDK is not in the default `%LOCALAPPDATA%\Android\Sdk` location:

```powershell
.\android_frontend\build-and-run.ps1 -SdkRoot 'D:\Android\Sdk'
```

If more than one ADB device is connected:

```powershell
.\android_frontend\build-and-run.ps1 -DeviceSerial '<adb serial>'
```

The script prefers an authorized USB device. If none is available, it
discovers paired Wireless Debugging devices and connects automatically. The
first wireless connection must be paired from **Developer options > Wireless
debugging > Pair device with pairing code**:

```powershell
adb pair '<phone-ip>:<pairing-port>'
```

Unless `-SkipBuild` is supplied, the script runs `npm install`, type-checks and
builds the Vite frontend, syncs the output into the Capacitor Android assets,
and then builds the native APK. The first build downloads both npm and native
dependencies, so it requires internet access and takes longer than subsequent
builds.

## Phone and game network setup

Keep the phone and the PC/console running F1 on the same local network. In the
game's Telemetry settings, enable UDP telemetry, set the destination IP to the
phone's Wi-Fi IPv4 address, and use port `20777`. Keep the dashboard in the
foreground while receiving; Android may suspend its listener in the
background.

No broad storage permission prompt is expected. The app declares Android's
normal `INTERNET` permission, which also permits local UDP sockets. The default
recording folder uses scoped app storage; selecting another destination grants
access only to the folder chosen through Android's system picker.
