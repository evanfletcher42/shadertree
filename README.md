# shadertree

## Building the reconstruction solver

`reconstruct/` builds `ledreconstruct` with CMake (3.22+) and a C++17 compiler. Dependencies: Ceres 2.2+, Eigen 3.4+, OpenCV 4.6+ (5.x works; FFmpeg is needed for video decode), jsoncpp and cxxopts 3.1+. If Ceres was built with LAPACK, the solver uses it; back it with an optimized BLAS such as OpenBLAS, since reference BLAS can be slower than Ceres' built-in Eigen path.

### Windows

Install Visual Studio 2022 or 2026 with the "Desktop development with C++" workload, which includes CMake and vcpkg. Then, from a Developer PowerShell:

```
cd reconstruct
cmake --preset windows
cmake --build --preset windows-release
```

vcpkg builds the dependencies from `vcpkg.json` on the first configure (slow; cached afterwards). The preset builds Ceres and the solver with AVX2, so the result needs an AVX2 CPU (Intel Haswell / AMD Excavator or newer). The executable and its DLLs end up in `build\windows\Release\`. You can also open `reconstruct\` as a folder in Visual Studio.

### Ubuntu 24.04

```
sudo apt install cmake g++ libceres-dev libopencv-dev libeigen3-dev libjsoncpp-dev libcxxopts-dev libopenblas-dev
cd reconstruct
cmake --preset linux
cmake --build --preset linux -j
```

### Arch

```
sudo pacman -S --needed base-devel cmake ceres-solver opencv qt6-base vtk hdf5 eigen jsoncpp cxxopts blas-openblas
```

Then build as on Ubuntu.

### SteamOS

Build inside an Arch container: `distrobox create --name arch --image archlinux:latest`, then `distrobox enter arch` and follow the Arch steps.

### Other platforms

Use vcpkg for the dependencies: `cmake --preset linux -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.
