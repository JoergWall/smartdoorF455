cmake_minimum_required(VERSION 3.10.2)
include(FetchContent)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)
set(EXE_NAME smartdoorF455)
add_executable(${EXE_NAME} main.cpp)
target_include_directories(${EXE_NAME} PRIVATE ~/RealSenseID/include/ ~/RealSenseID/include/RealSenseID ~/rpi-rgb-led-matrix/include/ /usr/include/opencv4/)
target_link_directories(${EXE_NAME} PRIVATE ~/RealSenseID/build/lib/ ~/rpi-rgb-led-matrix/lib /usr/lib/aarch64-linux-gnu)
# ignore BOOST deprecated headers warnings introduced with TgBot and boost 1.76
add_definitions("-DBOOST_ALLOW_DEPRECATED_HEADERS")
add_definitions("-DBOOST_BIND_GLOBAL_PLACEHOLDERS")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread ") 
target_link_libraries(${EXE_NAME} PRIVATE rsid pigpio rgbmatrix mosquitto  TgBot ssl crypto opencv_core opencv_videoio opencv_imgcodecs) 
install(TARGETS ${EXE_NAME} DESTINATION /usr/local/bin)