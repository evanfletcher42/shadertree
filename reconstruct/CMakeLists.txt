cmake_minimum_required( VERSION 3.22 )
project( ledreconstruct LANGUAGES CXX )

find_package( Ceres 2.2 REQUIRED )
find_package( Eigen3 REQUIRED NO_MODULE )
find_package( OpenCV REQUIRED )
find_package( jsoncpp CONFIG REQUIRED )
find_package( cxxopts 3 CONFIG REQUIRED )
find_package( Threads REQUIRED )

# OpenCV's version file rejects a different major version, so check the floor by hand.
if ( OpenCV_VERSION VERSION_LESS 4.6 )
	message( FATAL_ERROR "OpenCV 4.6 or newer required (found ${OpenCV_VERSION})" )
endif()

# OpenCV 5 split calib3d; opencv2/calib3d.hpp remains as a header in calib, the functions we use are in geometry.
if ( OpenCV_VERSION_MAJOR GREATER_EQUAL 5 )
	set( OPENCV_GEOMETRY_LIBS opencv_geometry opencv_calib )
else()
	set( OPENCV_GEOMETRY_LIBS opencv_calib3d )
endif()

# jsoncpp's target name depends on version and packaging.
if ( TARGET JsonCpp::JsonCpp )
	set( JSONCPP_LIB JsonCpp::JsonCpp )
elseif ( TARGET jsoncpp_lib )
	set( JSONCPP_LIB jsoncpp_lib )
else()
	set( JSONCPP_LIB jsoncpp_static )
endif()

add_executable( ledreconstruct
	ledreconstruct/bundleadjustment.cpp
	ledreconstruct/imagedecoder.cpp
	ledreconstruct/ledreconstruct.cpp
	ledreconstruct/observationset.cpp
	ledreconstruct/pointsvisualizer.cpp
)

target_compile_features( ledreconstruct PRIVATE cxx_std_17 )
target_compile_definitions( ledreconstruct PRIVATE _USE_MATH_DEFINES OPENCV_DISABLE_EIGEN_TENSOR_SUPPORT )

if ( MSVC )
	target_compile_options( ledreconstruct PRIVATE /permissive- /sdl )
	set_property( TARGET ledreconstruct PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON )
endif()

target_link_libraries( ledreconstruct PRIVATE
	Ceres::ceres
	Eigen3::Eigen
	opencv_core opencv_imgproc opencv_imgcodecs opencv_videoio opencv_highgui ${OPENCV_GEOMETRY_LIBS}
	${JSONCPP_LIB}
	cxxopts::cxxopts
	Threads::Threads
)
