cmake_minimum_required(VERSION 3.22.1)
project(PhoneApp)

set(CMAKE_CXX_STANDARD 20)

if(DEFINED ENV{PI_IP})
    set(PI_IP $ENV{PI_IP})
endif()
add_definitions(-DPI_IP="${PI_IP}")
message(STATUS "Baking PI_IP: ${PI_IP}")

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
set(OPENSSL_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/openssl/android-arm64")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libssl.a")
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libcrypto.a")
FetchContent_MakeAvailable(datachannel)

# for native app glue lib
add_library(native_app_glue STATIC ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${CMAKE_ANDROID_NDK}/sources/android)

# opencv as static lib
set(OpenCV_STATIC ON)
set(OpenCV_DIR "opencv-android/sdk/native/jni")
find_package(OpenCV REQUIRED)

# main app as a .so lib
add_library(phone_app SHARED
    src/main.cpp
    ../Shared/src/FrameBuffer.cpp
    src/Camera.cpp
    src/PhoneServerCommunication.cpp
    src/H265Encoder.cpp
)
target_link_options(phone_app PRIVATE "-Wl,-u,ANativeActivity_onCreate")

target_include_directories(phone_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
find_library(log-lib log)
find_library(android-lib android) #native hardware access
find_library(camera-lib camera2ndk)
find_library(mediandk-lib mediandk)

target_link_libraries(phone_app PRIVATE 
    ${android-lib}
    ${log-lib}
    ${camera-lib}
    native_app_glue 
    datachannel
    ${OpenCV_LIBS}
    ${mediandk-lib}
)
