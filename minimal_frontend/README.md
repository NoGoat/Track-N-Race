# Track N Race Minimal Recorder

A deliberately small automatic TNRD recorder. The Windows front end is plain
Win32, the macOS front end uses SwiftUI, and the Linux front end uses Qt
Widgets. Linux prefers Qt 6 and falls back to Qt 5 when Qt 6 is unavailable.
All three use libtnrp's raw socket backend and record TNRD V2/Zstandard files
whenever a valid folder is selected.

## Windows

Prerequisites: Windows 10 or newer, Visual Studio with the Desktop development
with C++ workload, a recent Windows SDK, CMake, and network access on the first
build.

```powershell
powershell -ExecutionPolicy Bypass -File .\minimal_frontend\build.ps1
```

The script builds the Win32 frontend, libtnrp, and its native dependencies as
one statically linked executable through CMake. It clears the old Windows
staging directory and installs `Track N Race Minimal Recorder.exe` under
`minimal_frontend\dist\windows`. The executable launches directly
with no framework runtime, package restore, DLL bundle, self-extractor,
PowerShell launcher, or terminal window. The embedded standard Win32 manifest
enables Common Controls v6 and Per-Monitor V2 DPI behavior. The application
follows the Windows light/dark app mode, requests matching native control and
title-bar themes, and paints the classic Win32 surfaces that do not follow that
setting themselves. It does not customize corners or backdrop materials.
The Attribution button opens the complete project and third-party license texts;
those texts are compiled into the application on every platform, so a
standalone binary does not need a neighboring license directory.

## macOS

Prerequisites: macOS 13 or newer, Apple Command Line Tools (or full Xcode),
CMake, and network access for libtnrp's pinned FetchContent dependencies on the
first configure. No Qt installation, Ninja, or full Xcode installation is
required.

```bash
chmod +x minimal_frontend/build-macos.sh minimal_frontend/build-macos-dmg.sh
./minimal_frontend/build-macos.sh
```

The script builds the native SwiftUI frontend and its Objective-C++ bridge to
the shared C++ recorder. It stages a self-contained, ad-hoc-signed
`Track N Race Minimal Recorder.app` under `minimal_frontend/dist/macos`.
Settings are stored in the application's macOS `UserDefaults` domain. The app
uses the system folder picker, appearance, accessibility behavior, and local
network privacy prompt.

The default build targets the current machine architecture. Override it with
`ARCHITECTURE=arm64` or `ARCHITECTURE=x86_64`; a single build tree intentionally
accepts one architecture at a time. For Developer ID distribution, provide a
signing identity:

```bash
CODESIGN_IDENTITY="Developer ID Application: Example (TEAMID)" \
    ./minimal_frontend/build-macos.sh
```

To create a compressed disk image after building and signing the app:

```bash
./minimal_frontend/build-macos-dmg.sh
```

The DMG is written to `minimal_frontend/dist/dmg`. Notarization is intentionally
left to the release environment because it requires Apple developer credentials.

## Linux system build

Prerequisites: CMake, a C++20 compiler, Qt 6 Widgets development files (or Qt 5
Widgets as a fallback), and network access for libtnrp's pinned FetchContent
dependencies on the first configure.

```bash
chmod +x minimal_frontend/build.sh minimal_frontend/build-appimage.sh
./minimal_frontend/build.sh
```

The system-linked tree is written to `minimal_frontend/dist/linux`. Install that tree
under a normal prefix, or run its binary directly. Settings are stored through
Qt's platform-native `QSettings` backend.

## Linux AppImage

The AppImage script additionally requires `curl`. It downloads the pinned
linuxdeploy release and its Qt plugin, bundles Qt and the application files,
and writes the result under `minimal_frontend/dist/appimage`.

```bash
./minimal_frontend/build-appimage.sh
```

The macOS application and DMG scripts were built and smoke-tested on Apple
silicon with Apple Command Line Tools.
