#!/bin/bash

# export TAILSCALE_PHONE_IP = "100.x.x.x"
# export PI_IP="192.168.x.x"

# Build for Phone side
# create the apk for Phone
cd ./src/Phone
bash ./create_apk.sh

# Build for Server side
# create Server ground control station executable
cd ../Server
bash ./create_executable.sh




