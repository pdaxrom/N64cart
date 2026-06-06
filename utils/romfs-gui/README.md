# ROMFS Manager GUI

This directory contains the Qt-based ROMFS Manager desktop application. It lets you browse, upload, and download files on the N64cart over USB or the remote proxy.

## Prerequisites

- Qt 6 (Widgets + Network + LinguistTools modules)
- CMake >= 3.16
- A C++17 compiler toolchain
- `libusb-1.0`
- Optional but recommended: [vcpkg](https://github.com/microsoft/vcpkg) for `libusb` on macOS, Linux, and Windows

`utils/romfs-gui/vcpkg.json` declares:
- `libusb`
- host-side `pkgconf` for `pkg-config` based discovery

If your local vcpkg clone lives at `/Users/sash/Work/vcpkg`, set:

```bash
export VCPKG_ROOT=/Users/sash/Work/vcpkg
```

You can still build against a system `libusb` if you skip the vcpkg toolchain and provide `libusb-1.0` through your package manager.

## macOS Build

```bash
cd utils/romfs-gui
cmake -B build-macos -S . \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-macos
```

Notes:
- The app links `libusb` through `pkg-config`; with the vcpkg toolchain that resolves to the vcpkg package.
- Run `cmake --build build-macos --target deploy` to execute `macdeployqt`.

## Linux Build

1. Install Qt and base toolchain, e.g. on Ubuntu:
   ```bash
   sudo apt install qt6-base-dev qt6-tools-dev qt6-tools-dev-tools cmake build-essential
   ```
2. Configure & build:
   ```bash
   cd utils/romfs-gui
   cmake -B build-linux -S . \
     -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
   cmake --build build-linux
   ```
3. Build a portable bundle:
   ```bash
   cmake --build build-linux --target deploy
   ```

The Linux `deploy` target creates:
- `build-linux/deploy/romfs_gui-linux-portable`
- `build-linux/deploy/romfs_gui-linux-portable.tar.gz`

## Windows Build

### Install dependencies

1. Install Visual Studio with Desktop development (or MSVC Build Tools).
2. Install Qt 6.
3. Ensure vcpkg is bootstrapped:
   ```powershell
   $env:VCPKG_ROOT = "C:\path\to\vcpkg"
   cd $env:VCPKG_ROOT
   .\bootstrap-vcpkg.bat
   ```

### Build steps

```powershell
cd utils\romfs-gui
cmake -B build-win -S . -G "Ninja" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build-win
```

Run `cmake --build build-win --target deploy` to execute `windeployqt` and copy `libusb` next to the `.exe`.

Use `cmake --build <build-dir> --target romfs-gui_lupdate` if you change strings; `romfs-gui_lrelease` regenerates QM files.

## Running

- macOS: launch `build-macos/ROMFS Manager.app`
- Linux: run `./build-linux/ROMFS\ Manager`
- Windows: run `build-win/ROMFS Manager.exe` or the deployed copy after `--target deploy`
