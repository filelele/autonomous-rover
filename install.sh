#just install android studio to get the ndk (libs and compilers)
export ANDROID_HOME=${HOME}/Android/Sdk
export ANDROID_NDK_HOME=${ANDROID_HOME}/ndk/30.0.14904198 

# export TAILSCALE_PHONE_IP = "100.x.x.x"
# export PI_IP = "100.x.x.x"

#install encrypt lib openssl for android ndk
cd ./src/Shared 
if [ ! -d "openssl" ]; then
    git clone https://github.com/openssl/openssl.git
fi
cd openssl
git checkout openssl-3.5.2

# OpenSSL needs ANDROID_NDK_ROOT and toolchain in PATH for Android builds
export ANDROID_NDK_ROOT=${ANDROID_NDK_HOME}
export PATH=${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin:${PATH}

./Configure android-arm64 -D__ANDROID_API__=30 --prefix=${PWD}/android-arm64 no-tests no-shared
make -j$(nproc)
make install_sw

#create the apk for phone
cd ../../Phone
bash ./create_apk.sh

# install json reader lib
sudo apt install nlohmann-json3-dev

# install opencv for server
sudo apt update
sudo apt install libopencv-dev

# install openssl for server
sudo apt install openssl

# create server ground control station executable
cd ../Server
bash ./create_executable.sh






