# MULD

## Install (Quick)

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

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

**Windows (with vcpkg toolchain):**
```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
```

Binary output:
- Linux/macOS: `build/muld`
- Windows: `build/Release/muld.exe` (multi-config generators)

### 3) (Optional) Install globally

- Linux/macOS:
  ```bash
  sudo cmake --install build
  ```
- Windows (Administrator terminal):
  ```powershell
  cmake --install build --config Release
  ```

### Uninstall

- Linux/macOS: remove installed binary from your install prefix (commonly `/usr/local/bin/muld`).
- Windows: remove `muld.exe` from the installation directory used by `cmake --install`.
