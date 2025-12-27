# ROMFS Manager GUI

This directory contains the Qt-based ROMFS Manager desktop application. It lets you browse, upload, and download files on the N64cart over USB or the remote proxy.

## Prerequisites

- Qt 6 (Widgets + Network + LinguistTools modules)
- CMake >= 3.16
- A C++17 compiler toolchain
- `libusb-1.0`
  - macOS: automatically built from the checked-in tarball unless `ENABLE_SYSTEM_LIBUSB=ON`
  - Linux: install the system package (e.g. `libusb-1.0-0-dev`)
  - Windows: build via vcpkg (see below)

## macOS Build

```bash
cd utils/romfs-gui
cmake -B build-macos -S .
cmake --build build-macos
```

Notes:
- Pass `-DENABLE_SYSTEM_LIBUSB=ON` to use Homebrew/MacPorts libusb instead of the bundled source.
- The build copies app + Qt translations into the `.app` bundle automatically.

## Linux Build

1. Install dependencies, e.g. on Ubuntu:
   ```bash
   sudo apt install qt6-base-dev qt6-tools-dev qt6-tools-dev-tools libusb-1.0-0-dev cmake build-essential
   ```
2. Configure & build:
   ```bash
   cd utils/romfs-gui
   cmake -B build-linux -S . -DENABLE_SYSTEM_LIBUSB=ON
   cmake --build build-linux
   ```

## Windows Build

### Install dependencies

1. Install Visual Studio with Desktop development (or MSVC Build Tools).
2. Install Qt 6 and ensure the "MinGW 64-bit" kit is available.
3. Install vcpkg and build libusb:
   ```powershell
   cd utils\romfs-gui
   git clone https://github.com/microsoft/vcpkg.git
   vcpkg\bootstrap-vcpkg.bat
   vcpkg\vcpkg.exe install libusb:x64-windows
   ```
   - This mirrors the default include/library paths referenced in `CMakeLists.txt`.

### Build steps

```powershell
cd utils\romfs-gui
cmake -B build-win -S . -G "Ninja"
cmake --build build-win
```

Use `cmake --build build-win --target romfs-gui_lupdate` if you change strings; `romfs-gui_lrelease` regenerates QM files.

## Running

- macOS: launch `build-macos/ROMFS Manager.app`
- Linux: run `./build-linux/romfs-gui`
- Windows: run `build-win/Release/romfs-gui.exe`

Use `ENABLE_SYSTEM_LIBUSB=ON` when targeting platforms with a preinstalled libusb.
