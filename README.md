# MULD

MULD (**MUL**ti-thread **D**ownloader) is a C++20 download engine and CLI for reliable, resumable HTTP/HTTPS file downloads.
It is designed as a reusable backend library (`muld`) with a simple command-line app (`muld`).

If you want a desktop frontend, see **muld-gui**:

- https://github.com/Arash1381-y/muld-gui

## What MULD does well

- Fast downloads using multiple parallel connections.
- Pause/resume support with persisted job image (`.muld`) for interrupted sessions.
- Resumable workflow designed for long downloads and unstable networks.
- Works as both:
  - a C++ library for integration in other apps
  - a standalone CLI tool

## Feature summary

- HTTP and HTTPS downloads (Boost.Asio + OpenSSL)
- Multi-connection chunked downloading
- Pause / resume / terminate / load-state workflow
- Download manager for lifecycle control
- Progress and chunk-progress callbacks
- Connection control logic (adaptive behavior tested)

## Platform support

| Platform       | Status                  | Notes                                        |
| -------------- | ----------------------- | -------------------------------------------- |
| Linux          | Supported               | Primary development target                   |
| macOS          | Supported               | Build via Clang/Homebrew dependencies        |
| Windows (MSVC) | Supported (build-level) | CMake + MSVC flags added; use vcpkg for deps |

> Note: Windows portability is actively improving. Core build support is present; always validate with latest dependencies/toolchain.

## Install

### 1) Install dependencies

- **Ubuntu / Debian**

  ```bash
  sudo apt update && sudo apt install -y build-essential cmake libboost-system-dev libssl-dev
  ```

- **Fedora / RHEL**

  ```bash
  sudo dnf install -y gcc-c++ cmake boost-devel openssl-devel
  ```

- **macOS (Homebrew)**

  ```bash
  brew install cmake boost openssl
  ```

- **Windows (MSVC + vcpkg)**
  ```powershell
  # In a Visual Studio Developer PowerShell
  git clone https://github.com/microsoft/vcpkg "$env:USERPROFILE\vcpkg"
  & "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
  & "$env:USERPROFILE\vcpkg\vcpkg.exe" install boost-system openssl:x64-windows
  ```

### 2) Configure and build

- **Linux / macOS**

  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --parallel
  ```

- **Windows (with vcpkg toolchain)**
  ```powershell
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake"
  cmake --build build --config Release
  ```

Binary output:

- Linux/macOS: `build/muld`
- Windows (multi-config generators): `build/Release/muld.exe`

### 3) Optional install

- Linux/macOS:
  ```bash
  sudo cmake --install build
  ```
- Windows (Administrator terminal):
  ```powershell
  cmake --install build --config Release
  ```

## CLI usage

```bash
muld [OPTIONS] <URL>
```

Common options:

- `-o, --output <path>` output file path
- `-c, --threads <N>` worker thread pool size (default: 8)
- `-s, --speed <value>` speed cap (e.g. `500K`, `2M`, `0` for unlimited)
- `-h, --help` show help

Example:

```bash
./build/muld -o ubuntu.iso -c 12 -s 8M https://example.com/ubuntu.iso
```

## Library usage (high level)

MULD exposes public headers under `include/muld/` and internal implementation under `src/`.
Typical integration flow:

1. Create a download via manager/engine APIs.
2. Attach callbacks for progress/chunk updates.
3. Start, pause, resume, or terminate as needed.
4. Persist/load state for resume-after-restart workflows.

For working examples, check test files in `tests/`.

## Project structure

- `include/muld/` public API headers
- `src/` core engine + CLI
- `tests/` integration and behavior tests
- `CMakeLists.txt` build definition

## Related project

- **muld-gui** (GUI frontend powered by MULD backend):
  https://github.com/Arash1381-y/muld-gui
