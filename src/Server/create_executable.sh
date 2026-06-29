#!/bin/bash

# export TAILSCALE_PHONE_IP = "100.x.x.x"

mkdir -p ./build
cd ./build
sudo rm -rf ./*
cmake ..
cmake --build
cmake --build . -j$(nproc)

# the executable is ground_control_station
