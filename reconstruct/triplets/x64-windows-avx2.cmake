# x64-windows, plus AVX2 for Ceres. Ceres shares Eigen types with ledreconstruct and Eigen's alignment depends on
# the SIMD flags, so ledreconstruct is built to match (LEDRECONSTRUCT_AVX2, set by the windows preset).
set( VCPKG_TARGET_ARCHITECTURE x64 )
set( VCPKG_CRT_LINKAGE dynamic )
set( VCPKG_LIBRARY_LINKAGE dynamic )

if ( PORT STREQUAL "ceres" )
	set( VCPKG_C_FLAGS "/arch:AVX2" )
	set( VCPKG_CXX_FLAGS "/arch:AVX2" )
endif()
