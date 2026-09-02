# Track N Race Android frontend

The Android app is a focused, full-screen steering-wheel dashboard backed by
the same C++ `protocol_parser_library` as the desktop apps. It listens for F1
24/25/26 UDP telemetry on port 20777 and displays shift lights, gear,
speed/RPM, position and lap timing, ERS and fuel, tyre state, DRS/SLM, and
brake/throttle inputs. The layout adapts to both portrait and landscape.

The **REC** control calls the shared engine's TNRD V5 writer through JNI.
Recording is enabled by default, persists across launches, and writes sessions
to the app's external Documents directory under `Track N Race/`. Turning REC
off finalizes the active file. Recording errors from the native disk thread are
shown in the dashboard.

This is the first of three planned screens. The old placeholder-heavy
navigation shell has intentionally been removed; the remaining screens will be
designed separately.

## Build and run on a physical phone

Prerequisites on the PC:

- JDK 17
- Android SDK Platform 35 and Build-Tools
- Android SDK Platform-Tools and Command-line Tools (latest)
- NDK (Side by side) 27.0.12077973 and CMake 3.22.1
- Gradle 8.9 or newer on `PATH` (or generate a Gradle wrapper here)
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

The first native build downloads the dependencies pinned by
`protocol_parser_library`, so it requires internet access and takes longer than
subsequent builds.

## Phone and game network setup

Keep the phone and the PC/console running F1 on the same local network. In the
game's Telemetry settings, enable UDP telemetry, set the destination IP to the
phone's Wi-Fi IPv4 address, and use port `20777`. Keep the dashboard in the
foreground while receiving; Android may suspend its listener in the
background.

No runtime permission prompt is expected. The app declares Android's normal
`INTERNET` permission, which also permits local UDP sockets. Recordings use the
app's scoped external Documents directory and therefore need no storage
permission.
