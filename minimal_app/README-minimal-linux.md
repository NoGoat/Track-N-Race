# Track N Race Minimal Recorder - Linux Executable

This is a system-linked Linux executable, not an AppImage. Run it with:

```bash
./track-n-race-minimal
```

## Runtime dependencies

- Linux with a glibc version compatible with the build machine
- Qt 6 Core, GUI, and Widgets libraries, or the corresponding Qt 5 libraries
  when the executable was built using the Qt 5 fallback
- A Qt platform plugin. The XCB plugin is normally installed with Qt; install
  the Qt Wayland package separately when native Wayland support is required.
- The system C and C++ runtimes: glibc, libstdc++, and libgcc

Glaze, zlib, Zstandard, libxlsxwriter, and minizip are linked statically and do
not need separate runtime packages. To see the exact shared libraries required
by a particular build, run:

```bash
ldd ./track-n-race-minimal
```

Typical runtime packages:

| Distribution | Qt 6 | Qt 5 fallback | Optional Wayland |
| --- | --- | --- | --- |
| Debian/Ubuntu | `libqt6widgets6` | `libqt5widgets5` | `qt6-wayland` or `qtwayland5` |
| Fedora | `qt6-qtbase-gui` | `qt5-qtbase-gui` | `qt6-qtwayland` or `qt5-qtwayland` |
| Arch Linux | `qt6-base` | `qt5-base` | `qt6-wayland` or `qt5-wayland` |

## Build dependencies

- CMake 3.20 or newer
- GCC or Clang with C++20 support
- Git
- GNU Make or Ninja
- Qt 6 Widgets development files, or Qt 5 Widgets development files as a
  fallback
- Linux libc development headers
- Network access on the first configure so CMake can download pinned sources

Install the build dependencies with one of these commands:

```bash
# Debian/Ubuntu, Qt 6
sudo apt install build-essential cmake git qt6-base-dev

# Fedora, Qt 6
sudo dnf install cmake gcc-c++ git make qt6-qtbase-devel

# Arch Linux, Qt 6
sudo pacman -S base-devel cmake git qt6-base
```

The first configure downloads and statically builds these pinned dependencies:

- Glaze 7.8.3
- zlib 1.3.2
- Zstandard 1.5.7
- libxlsxwriter 1.2.4, including its bundled minizip

Build from the repository root:

```bash
bash minimal_app/build-minimal-linux.sh
```

The script writes only the executable and this README to
`minimal_app/dist/minimal-linux`. Set `BUILD_DIR`, `OUTPUT_DIR`, or `JOBS` to
override the defaults.
