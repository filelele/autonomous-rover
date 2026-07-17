cmake_minimum_required(VERSION 3.22.1)
project(GroundControlStation)
set(CMAKE_CXX_STANDARD 20)

find_package(OpenSSL REQUIRED)
find_package(SDL2 REQUIRED)
find_package(OpenCV REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswscale)

# libdatachannel setup
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(USE_LIBCRYPTO OFF CACHE BOOL "")
set(ENABLE_OPENSSL ON CACHE BOOL "")
set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
        datachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG        v0.23.3
)
FetchContent_MakeAvailable(datachannel)

add_library(dashboard STATIC
        src/ServerPhoneCommunication.cpp
        src/DashboardUI.cpp
        src/H265Decoder.cpp
)

target_include_directories(dashboard PUBLIC
        include
        ../Shared/include
        ${FFMPEG_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(dashboard PUBLIC
        datachannel
        OpenSSL::SSL
        OpenSSL::Crypto
        ${FFMPEG_LIBRARIES}
        ${SDL2_LIBRARIES}
        ${OpenCV_LIBS}
)

# Final executable
add_executable(ground_control_station src/GroundControlStation.cpp)
target_link_libraries(ground_control_station PRIVATE dashboard)