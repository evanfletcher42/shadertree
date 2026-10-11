# shadertree

## Building the reconstruction solver

`reconstruct/` builds `ledreconstruct` with CMake (3.22+) and a C++17 compiler. Dependencies: Ceres 2.2+, Eigen 3.4+, OpenCV 4.6+ (5.x works; FFmpeg is needed for video decode), jsoncpp and cxxopts 3.1+.

### Windows

Run `powershell -ExecutionPolicy Bypass -File setup-dev.ps1` once. It checks for Visual Studio 2022 or newer with the C++ tools and vcpkg (listed in `reconstruct\.vsconfig`; if anything is missing it prints the install command), sets `VCPKG_ROOT` to Visual Studio's bundled vcpkg unless you already have one, and creates `.venv` (Python 3.12) for the web side and the reconstruction dev tools. Then, from a Developer PowerShell:

```
cd reconstruct
cmake --preset windows
cmake --build --preset windows-release
```

vcpkg builds the dependencies from `vcpkg.json` on the first configure (slow; cached afterwards). The executable and its DLLs end up in `build\windows\Release\`. You can also open `reconstruct\` as a folder in Visual Studio.

For a faster debuggable build use `cmake --build --preset windows-relwithdebinfo` (Debug works but the solve is very slow).

### Ubuntu 24.04 and Arch

Run `./setup-dev.sh` once. It installs the dependencies with apt or pacman and creates `.venv`. Then:

```
cd reconstruct
cmake --preset linux
cmake --build --preset linux -j
```

### SteamOS

Build inside an Arch container: `distrobox create --name arch --image archlinux:latest`, then `distrobox enter arch` and follow the Arch steps.

### VS Code

Open the repository root and install the recommended extensions (CMake Tools, C/C++, Python). Pick the `windows` or `linux` configure preset and a build preset (`windows-relwithdebinfo` for debugging). Ctrl+Shift+B lists build tasks for each configuration; the `ledreconstruct` launch configurations build the selected preset, then solve from a directory of cached observation JSONs. Python completion for `stweb/` uses the `.venv` from the setup script. On Windows, restart VS Code after the first `setup-dev.ps1` run so it sees `VCPKG_ROOT`.

### Other platforms

Use vcpkg for the dependencies: `cmake --preset linux -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.
