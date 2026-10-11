# shadertree

## Building the reconstruction solver

`reconstruct/` builds `ledreconstruct` with CMake (3.22+) and a C++17 compiler. Dependencies: Ceres 2.2+, Eigen 3.4+, OpenCV 4.6+ (5.x works; FFmpeg is needed for video decode), jsoncpp and cxxopts 3.1+.

### Windows

Install Visual Studio 2022 or 2026 with the "Desktop development with C++" workload, which includes CMake and vcpkg. Then, from a Developer PowerShell:

```
cd reconstruct
cmake --preset windows
cmake --build --preset windows-release
```

vcpkg builds the dependencies from `vcpkg.json` on the first configure (slow; cached afterwards). The executable and its DLLs end up in `build\windows\Release\`. You can also open `reconstruct\` as a folder in Visual Studio.

For a faster debuggable build use `cmake --build --preset windows-relwithdebinfo` (Debug works but the solve is very slow).

### VS Code

Open the repository root and install the recommended extensions (CMake Tools, C/C++, Python). Pick the `windows` or `linux` configure preset and a build preset (`windows-relwithdebinfo` for debugging), then use the `ledreconstruct` launch configurations, which solve from a directory of cached observation JSONs. On Windows, `VCPKG_ROOT` must be set: start VS Code from a Developer PowerShell (`code .`), or set `VCPKG_ROOT` as a user environment variable pointing at Visual Studio's `VC\vcpkg` directory.

For Python completion on the web side (`stweb/`), create a virtual environment at the repository root, which VS Code picks up automatically: `python -m venv .venv`, then install `stweb/requirements.txt` into it.

### Ubuntu 24.04

```
sudo apt install cmake g++ libceres-dev libopencv-dev libeigen3-dev libjsoncpp-dev libcxxopts-dev
cd reconstruct
cmake --preset linux
cmake --build --preset linux -j
```

### Arch

```
sudo pacman -S --needed base-devel cmake ceres-solver opencv qt6-base vtk hdf5 eigen jsoncpp cxxopts
```

Then build as on Ubuntu.

### SteamOS

Build inside an Arch container: `distrobox create --name arch --image archlinux:latest`, then `distrobox enter arch` and follow the Arch steps.

### Other platforms

Use vcpkg for the dependencies: `cmake --preset linux -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.
