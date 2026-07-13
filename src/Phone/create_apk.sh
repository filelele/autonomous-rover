#!/bin/bash
export ANDROID_HOME=${HOME}/Android/Sdk
export ANDROID_NDK_HOME=${ANDROID_HOME}/ndk/30.0.14904198 


# Install openssl and build for android
# OpenSSL needs ANDROID_NDK_ROOT and toolchain in PATH for Android builds
export ANDROID_NDK_ROOT=${ANDROID_NDK_HOME}
export PATH=${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin:${PATH}
if [ ! -d "openssl" ]; then
    git clone https://github.com/openssl/openssl.git
    cd openssl
    git checkout openssl-3.5.2
    ./Configure android-arm64 -D__ANDROID_API__=30 --prefix=${PWD}/android-arm64 no-tests no-shared
    make -j$(nproc)
    make install_sw
    cd ..
fi

# Build the native_main as jni lib .so
mkdir -p build
cd ./build
sudo rm -rf ./*

cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake   -DANDROID_ABI=arm64-v8a   -DANDROID_PLATFORM=android-30   -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
mkdir -p ../android_wrapper/app/src/main/jniLibs/arm64-v8a
cp ./libphone_app.so ../android_wrapper/app/src/main/jniLibs/arm64-v8a/

cd ../android_wrapper
sudo rm -rf ./app/build/*
./gradlew assembleRelease

# the apk will be in autonomous-rover/src/Phone/android_wrapper/app/build/outputs/apk/release/app-release.apk
# connect phone, turn on USB debugging then adb install path-to-apk/app-release.apk