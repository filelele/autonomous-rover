#!/bin/bash

# ANDROID NATIVE SETUP
sudo apt update && sudo apt install -y openjdk-17-jdk

if [ ! -d "$HOME/Android/Sdk/ndk" ]; then
    mkdir -p $HOME/Android/Sdk/ndk
    cd $HOME/Android/Sdk/ndk
    wget https://dl.google.com/android/repository/android-ndk-r26b-linux.zip
    unzip android-ndk-r26b-linux.zip
    rm android-ndk-r26b-linux.zip
    mv android-ndk-r26b r26b
fi

if [ ! -d "$HOME/Android/Sdk/cmdline-tools" ]; then
    mkdir -p $HOME/Android/Sdk/cmdline-tools
    cd $HOME/Android/Sdk/cmdline-tools
    wget https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
    unzip commandlinetools-linux-11076708_latest.zip
    rm commandlinetools-linux-11076708_latest.zip
    mv cmdline-tools latest
fi

export PATH="$PATH:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools"
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/r26b"
if [ ! -d "$HOME/Android/Sdk/platforms" ]; then
    yes | $HOME/Android/Sdk/cmdline-tools/latest/bin/sdkmanager --licenses 
    $HOME/Android/Sdk/cmdline-tools/latest/bin/sdkmanager "platforms;android-31" "build-tools;31.0.0" "platform-tools"
fi

export PATH="$PATH:$HOME/Android/Sdk/cmdline-tools/latest/bin:$HOME/Android/Sdk/platform-tools"


# Download opencv android
if [ ! -d "opencv-android" ]; then
    curl -L -O https://github.com/opencv/opencv/releases/download/4.12.0/opencv-4.12.0-android-sdk.zip
    unzip opencv-4.12.0-android-sdk.zip
    mkdir -p opencv-android
    mv OpenCV-android-sdk/* opencv-android/
    rm -rf opencv-4.12.0-android-sdk.zip OpenCV-android-sdk
fi

# Install openssl and build for android
# OpenSSL needs ANDROID_NDK_ROOT and toolchain in PATH for Android builds
export ANDROID_NDK_ROOT=${ANDROID_NDK_HOME}
export PATH=${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin:${PATH}
if [ ! -d "openssl" ]; then
    git clone https://github.com/openssl/openssl.git
    cd openssl
    git checkout openssl-3.5.2
    ./Configure android-arm64 -D__ANDROID_API__=31 --prefix=${PWD}/android-arm64 no-tests no-shared
    make -j$(nproc)
    make install_sw
    cd ..
fi

# Build the native_main as jni lib .so
mkdir -p build
cd ./build
sudo rm -rf ./*

cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake   -DANDROID_ABI=arm64-v8a   -DANDROID_PLATFORM=android-31   -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
mkdir -p ../android_wrapper/app/src/main/jniLibs/arm64-v8a
cp ./libphone_app.so ../android_wrapper/app/src/main/jniLibs/arm64-v8a/

cd ../android_wrapper
sudo rm -rf ./app/build/*
./gradlew assembleRelease

# the apk will be in autonomous-rover/src/Phone/android_wrapper/app/build/outputs/apk/release/app-release.apk
# connect phone, turn on USB debugging then adb install path-to-apk/app-release.apk