# Track N Race Minimal Recorder

A deliberately small automatic TNRD recorder. The Windows front end is plain
Win32; the Linux front end uses GTK 4. Both use libtnrp's raw socket backend
and record TNRD V2/Zstandard files whenever a valid folder is selected.

## Windows

Prerequisites: Windows 10 or newer, Visual Studio with the Desktop development
with C++ workload, a recent Windows SDK, CMake, and network access on the first
build.

```powershell
powershell -ExecutionPolicy Bypass -File .\minimal_app\build.ps1
```

The script builds the Win32 frontend, libtnrp, and its native dependencies as
one statically linked executable through CMake. It clears the old Windows
staging directory and installs `Track N Race Minimal Recorder.exe` under
`minimal_app\dist\windows`. The executable launches directly
with no framework runtime, package restore, DLL bundle, self-extractor,
PowerShell launcher, or terminal window. The embedded standard Win32 manifest
enables Common Controls v6 and Per-Monitor V2 DPI behavior. The application
follows the Windows light/dark app mode, requests matching native control and
title-bar themes, and paints the classic Win32 surfaces that do not follow that
setting themselves. It does not customize corners or backdrop materials.
The Attribution button opens the complete project and third-party license texts;
those texts are compiled into the executable on both Windows and Linux, so a
standalone binary does not need a neighboring license directory.

## Linux system build

Prerequisites: CMake, a C++20 compiler, `pkg-config`, GTK 4.6 development files,
`glib-compile-schemas`, and network access for libtnrp's pinned FetchContent
dependencies on the first configure.

```bash
chmod +x minimal_app/build.sh minimal_app/build-appimage.sh
./minimal_app/build.sh
```

The system-linked tree is written to `minimal_app/dist/linux`. Install that tree
under a normal prefix, or run its binary directly; the executable also discovers
the staged GSettings schema beside the build/install tree.

## Linux AppImage

The AppImage script additionally requires `curl`. It downloads the pinned
linuxdeploy release `1-alpha-20250213-2`, stages GTK and the application files,
and writes the result under `minimal_app/dist/appimage`.

```bash
./minimal_app/build-appimage.sh
```

These commands are provided for handoff. The implementing agent did not invoke
the configure, compile, package, or launch commands.
