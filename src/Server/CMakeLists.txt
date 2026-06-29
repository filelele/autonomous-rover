cmake_minimum_required(VERSION 3.22.1)
project(GroundControlStation)
set(CMAKE_CXX_STANDARD 20)

# OpenSSL - Force find system version
find_package(OpenSSL REQUIRED)
message(STATUS "Using system OpenSSL: ${OPENSSL_LIBRARIES}")

# SDL2
find_package(SDL2 REQUIRED)

# OpenCV
find_package(OpenCV REQUIRED)

# datachannel
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

# Submodules as static libs
add_library(dashboard STATIC 
    src/DashboardListener.cpp 
    src/DashboardUI.cpp
)
target_include_directories(dashboard PUBLIC 
    include 
    ../Shared/include
    ${SWSCALE_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)
target_link_libraries(dashboard PUBLIC 
    datachannel 
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SWSCALE_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${OpenCV_LIBS}
)

# final executable
add_executable(ground_control_station src/GroundControlStation.cpp)
target_link_libraries(ground_control_station PRIVATE dashboard)
