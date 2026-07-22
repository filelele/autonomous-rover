#!/bin/bash

# export TAILSCALE_PHONE_IP = "100.x.x.x"

sudo apt update

# install opencv
sudo apt install libopencv-dev

# install openssl
sudo apt install -y libssl-dev

# install sdl2
sudo apt install -y libsdl2-dev

mkdir -p ./build
cd ./build
sudo rm -rf ./*
cmake ..
cmake --build
cmake --build . -j$(nproc)

# the executable is ground_control_station