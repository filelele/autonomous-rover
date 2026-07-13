#just install android studio to get the ndk (libs and compilers)
export ANDROID_HOME=${HOME}/Android/Sdk
export ANDROID_NDK_HOME=${ANDROID_HOME}/ndk/30.0.14904198 

# Do the 2 lines below for webrtc to know IP and establish connection when build, 
# IPs is hardcoded

# export TAILSCALE_PHONE_IP = "100.x.x.x"
# export PI_IP = "100.x.x.x" # raspi local IP in the Phone hotspot network

# Build for Phone side
# create the apk for Phone
cd ./src/Phone
bash ./create_apk.sh


# Build for Server side
# install opencv for Server
sudo apt update
sudo apt install libopencv-dev

# install openssl for Server
sudo apt install openssl

# create Server ground control station executable
cd ../Server
bash ./create_executable.sh






