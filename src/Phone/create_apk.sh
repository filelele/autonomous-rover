#!/bin/bash
export ANDROID_HOME=${HOME}/Android/Sdk
export ANDROID_NDK_HOME=${ANDROID_HOME}/ndk/30.0.14904198 

mkdir -p build
cd ./build
sudo rm -rf ./*

# ensure CMake installed
cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake   -DANDROID_ABI=arm64-v8a   -DANDROID_PLATFORM=android-30   -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
cp ./libphone_app.so ../android_wrapper/app/src/main/jniLibs/arm64-v8a/

cd ../android_wrapper
sudo rm -rf ./app/build/*
./gradlew assembleRelease

# the apk will be in autonomous-rover/src/Phone/android_wrapper/app/build/outputs/apk/release/app-release.apk
# connect phone, turn on USB debugging then adb install path-to-apk/app-release.apk

