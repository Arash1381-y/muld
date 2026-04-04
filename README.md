# MULD

## Installation

### 1. Prerequisites (Dependencies)

To build `muld_cli`, you need a C++20 compatible compiler, CMake (3.16+), Boost, and OpenSSL.

**Ubuntu / Debian:**
```bash
sudo apt update
sudo apt install build-essential cmake libboost-system-dev libssl-dev
```

**macOS (using Homebrew):**
```bash
brew install cmake boost openssl
```

**Fedora / RHEL:**
```bash
sudo dnf install gcc-c++ cmake boost-devel openssl-devel
```

### 2. Build Locally

First, clone the repository and navigate into it. Then, generate the build files and compile the project using CMake:

```bash
# 1. Create a build directory
mkdir build
cd build

# 2. Configure the project for a Release build
cmake -DCMAKE_BUILD_TYPE=Release ..

# 3. Compile the executable (uses multiple cores to speed up building)
cmake --build . --parallel
```
*After this step finishes, the `muld_cli` executable will be available inside your `build` directory, and you can run it directly using `./muld_cli`.*

### 3. Install System-Wide

To install the executable globally so you can run `muld_cli` from anywhere in your terminal, run the following command from inside your `build` directory:

```bash
sudo cmake --install .
```
*(Note: `sudo` is required on Linux/macOS to copy the binary to `/usr/local/bin`. On Windows, run the terminal as Administrator instead).*

**Uninstalling:**
If you ever need to remove the application, you can simply delete the binary from your system path:
```bash
sudo rm /usr/local/bin/muld_cli
```