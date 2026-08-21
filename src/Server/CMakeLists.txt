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

set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../Shared/libdatachannel libdatachannel)

add_library(h265decoder STATIC
        src/H265Decoder.cpp
)

target_include_directories(h265decoder PUBLIC
        include
        ../Shared/include
        ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(h265decoder PUBLIC
        ${FFMPEG_LIBRARIES}
        ${OpenCV_LIBS}
)

add_library(server_phone_communication STATIC
        src/ServerPhoneCommunication.cpp
)

target_include_directories(server_phone_communication PUBLIC
        include
        ../Shared/include
        ${FFMPEG_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(server_phone_communication PUBLIC
        h265decoder
        datachannel
        OpenSSL::SSL
        OpenSSL::Crypto
        ${OpenCV_LIBS}
)

add_library(dashboard_ui STATIC
        src/DashboardUI.cpp
)

target_include_directories(dashboard_ui PUBLIC
        include
        ../Shared/include
        ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(dashboard_ui PUBLIC
        server_phone_communication
        ${SDL2_LIBRARIES}
        ${OpenCV_LIBS}
)

add_library(lingbot_map_localizer STATIC
        src/LingbotMapLocalizer.cpp
)

target_include_directories(lingbot_map_localizer PUBLIC
        include
        ../Shared/include
)

target_link_libraries(lingbot_map_localizer PUBLIC
        server_phone_communication
)

# Final executable
add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE
        dashboard_ui
        lingbot_map_localizer
        server_phone_communication
)