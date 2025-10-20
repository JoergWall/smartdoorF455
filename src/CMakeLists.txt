##
# @file CMakeLists.txt
#
# @brief Main CMake configuration file for project smartdoorF455
# @details
# This CMakeLists.txt sets up the build environment for the smartdoorF455 application.
# It specifies the minimum required CMake version, the C++ standard to be used,
# and includes several external dependencies using FetchContent and ExternalProject_Add.
# The external dependencies included are:
# - OpenCV for computer vision tasks
# - OpenSSL for encryption and TLS support
# - mosquitto for MQTT communication
# via FetchContent:
# - tomlplusplus for TOML parsing from https://github.com/marzer/tomlplusplus
# - WiringPi for GPIO access from https://github.com/WiringPi/WiringPi
# - RealSenseID for Intel RealSense ID SDK from https://github.com/IntelRealSense/RealSenseID.git
# - tgbot-cpp for Telegram Bot API from https://github.com/DoclerLabs/tgbot-cpp
# - PeriodicExecutor for periodic task execution from https://github.com/joergwall/PeriodicExecutor.git
# via ExternalProject_Add, because no CMakeLists.txt is provided and has to be built via Makefile:
# - rpi-rgb-led-matrix for LED matrix control from https://github.com/hzeller/rpi-rgb-led-matrix
#
# @author Joerg Wallmersperger
#
##
cmake_minimum_required(VERSION 3.10.2)

# C++17 and above
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
set(EXE_NAME smartdoorF455)
add_executable(${EXE_NAME} smartdoorF455.cpp)


if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
	# set the timestamps of extracted contents to the time of extraction when using FetchContent()
	# in order to avoid DOWNLOAD_EXTRACT_TIMESTAMP warning
    cmake_policy(SET CMP0135 NEW)
endif()

# Fix the CMP0167 warning from tgbot-cpp's old FindBoost usage
cmake_policy(SET CMP0167 NEW)

# --- Find System Dependencies First ---

# Find OpenCV and define targets for core, videoio, and imgcodecs
find_package(OpenCV REQUIRED COMPONENTS core videoio imgcodecs)

# Find OpenSSL for encryption/TLS
find_package(OpenSSL REQUIRED)

# Find mosquitto (typically a system library on Linux/RPi)
# We use find_library as mosquitto may not provide a config file for find_package
find_library(MOSQUITTO_LIB NAMES mosquitto REQUIRED)



# --- FetchContent Dependencies ---
# FetchContent to manage external dependencies
# see https://cmake.org/cmake/help/latest/module/FetchContent.html
# put them in external/ subfolder
include(FetchContent)
set(FETCHCONTENT_BASE_DIR ${CMAKE_SOURCE_DIR}/external)
# include PeriodicExecutor via FetchContent
# git ls-remote --tags https://github.com/joergwall/PeriodicExecutor.git
FetchContent_Declare(
  PeriodicExecutor
  GIT_REPOSITORY https://github.com/joergwall/PeriodicExecutor.git
  GIT_TAG main
)

# include RealSenseID via FetchContent
# git ls-remote --tags https://github.com/IntelRealSense/RealSenseID.git
FetchContent_Declare(
  RealSenseID
  GIT_REPOSITORY https://github.com/IntelRealSense/RealSenseID
  GIT_TAG f038ef9e9e7c274748e503ebafc6fde21df8ec0f #v1.3.1, May 22nd 2025
)
# include tgbot-cpp via FetchContent
# git ls-remote --tags https://github.com/reo7sp/tgbot-cpp.git
FetchContent_Declare(
  tgbot-cpp
  GIT_REPOSITORY https://github.com/reo7sp/tgbot-cpp
  GIT_TAG f52c0b841550a74045e37c9d17a2011c355a08df # v1.9.1,
)
# include tomlplusplus via FetchContent
# git ls-remote --tags https://github.com/marzer/tomlplusplus.git
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de # v3.4.0
)

# include WiringPi via FetchContent
# git ls-remote --tags https://github.com/WiringPi/WiringPi.git
FetchContent_Declare(
  WiringPi
  GIT_REPOSITORY https://github.com/WiringPi/WiringPi.git
  GIT_TAG master
)

# Populate dependencies that provide a CMakeLists.txt
FetchContent_MakeAvailable(PeriodicExecutor RealSenseID tgbot-cpp tomlplusplus WiringPi)

# --- rpi-rgb-led-matrix via ExternalProject_Add ---
# Use ExternalProject_Add because it has no CMakeLists.txt and must be built via Makefile.
include(ExternalProject)

# Define variables for rpi-rgb-led-matrix source and binary directories
set(RPI_RGB_LED_MATRIX_REPO "https://github.com/hzeller/rpi-rgb-led-matrix.git")
set(RPI_RGB_LED_MATRIX_TAG "master")
set(RPI_RGB_LED_MATRIX_SOURCE_DIR "${CMAKE_BINARY_DIR}/external/rpi-rgb-led-matrix-src")
set(RPI_RGB_LED_MATRIX_BINARY_DIR "${CMAKE_BINARY_DIR}/external/rpi-rgb-led-matrix-build")

message(STATUS "Building rpi-rgb-led-matrix from ${RPI_RGB_LED_MATRIX_SOURCE_DIR} via ExternalProject_Add")

ExternalProject_Add(rpi_rgbmatrix_ep
    # Clone the repository
    GIT_REPOSITORY ${RPI_RGB_LED_MATRIX_REPO}
    GIT_TAG ${RPI_RGB_LED_MATRIX_TAG}
    # Specify directories
    SOURCE_DIR  ${RPI_RGB_LED_MATRIX_SOURCE_DIR}
    BINARY_DIR  ${RPI_RGB_LED_MATRIX_BINARY_DIR}
    # Disable steps not needed
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND ""            # upstream project uses Makefile
    INSTALL_COMMAND ""              # leave install empty; we will link directly against produced lib
    # Build using its Makefile
    BUILD_COMMAND make -C ${RPI_RGB_LED_MATRIX_SOURCE_DIR} # Use -C to ensure make is run in the source dir
    # Required to generate the lib/ directory within the source directory
    BUILD_BYPRODUCTS
      "${RPI_RGB_LED_MATRIX_SOURCE_DIR}/lib/librgbmatrix.a"
      "${RPI_RGB_LED_MATRIX_SOURCE_DIR}/lib/librgbmatrix.so"
)

# --- IMPORTED Library Target for linking ---

# Path to the expected static library produced by the Makefile (as it builds in the source directory)
set(RGBMATRIX_EXPECTED_LIB "${RPI_RGB_LED_MATRIX_SOURCE_DIR}/lib/librgbmatrix.a")

# Create an IMPORTED library target that will be available for linking.
# We remove INTERFACE_INCLUDE_DIRECTORIES here to fix the "non-existent path" error.
add_library(rgbmatrix_external UNKNOWN IMPORTED GLOBAL)
set_target_properties(rgbmatrix_external PROPERTIES
    IMPORTED_LOCATION "${RGBMATRIX_EXPECTED_LIB}"
)

# Ensure main executable is built after the external project
add_dependencies(${EXE_NAME} rpi_rgbmatrix_ep)

set(RGBMATRIX_FINAL rgbmatrix_external) # Define the link target name

# ---

# Add necessary include directories. We explicitly add rpi-rgb-led-matrix/include
# using its source directory path, as this is the only way to propagate its include path
# since we can't use INTERFACE_INCLUDE_DIRECTORIES on the IMPORTED target.
target_include_directories(${EXE_NAME} PRIVATE
    "/usr/include/opencv4/" # OpenCV system header path
    "${PeriodicExecutor_SOURCE_DIR}/include"
    "${tomlplusplus_SOURCE_DIR}/include"
    "${tomlplusplus_SOURCE_DIR}/single_include"
    "${tgbot-cpp_SOURCE_DIR}/include"
    "${RealSenseID_SOURCE_DIR}/include"
    # Include the rpi-rgb-led-matrix headers using the source directory path
    "${RPI_RGB_LED_MATRIX_SOURCE_DIR}/include"
)

# ignore BOOST deprecated headers warnings introduced with TgBot and boost 1.76
add_definitions("-DBOOST_ALLOW_DEPRECATED_HEADERS")
add_definitions("-DBOOST_BIND_GLOBAL_PLACEHOLDERS")
# link tomlplusplus via its imported target provided by FetchContent and then other libraries;
# link to the imported rgbmatrix_external target.
target_link_libraries(${EXE_NAME} PRIVATE
    tomlplusplus::tomlplusplus
    rsid
    wiringPi
    ${RGBMATRIX_FINAL} # Link to the IMPORTED target created by ExternalProject_Add
    TgBot
    mosquitto
    ssl
    crypto
    opencv_core
    opencv_videoio
    opencv_imgcodecs
)

install(TARGETS ${EXE_NAME} DESTINATION /usr/local/bin)