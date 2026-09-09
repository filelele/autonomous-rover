#!/bin/bash

# export TAILSCALE_PHONE_IP = "100.x.x.x"

sudo apt update

# install opencv
sudo apt install libopencv-dev

# install openssl
sudo apt install -y libssl-dev

# install sdl2
sudo apt install -y libsdl2-dev

# download and install lingbot-map
git clone https://github.com/filelele/lingbot-map.git
cd lingbot-map
ENV_NAME="lingbot-map"

if conda env list | awk '{print $1}' | grep -Fxq "$ENV_NAME"; then
    echo "Environment '$ENV_NAME' already exists. Skipping..."
    conda activate lingbot-map
else
    conda create -n lingbot-map python=3.10 -y
    conda activate lingbot-map
    pip install torch==2.8.0 torchvision==0.23.0 --index-url https://download.pytorch.org/whl/cu128
    pip install -e .
    pip install --index-url https://pypi.org/simple flashinfer-python
    pip install -e ".[vis]"
fi
## sky-masking model
wget -c "https://huggingface.co/robbyant/lingbot-map/resolve/main/skyseg_batch.onnx"
pip install onnxruntime-gpu

## main model
wget -c "https://huggingface.co/robbyant/lingbot-map/resolve/main/lingbot-map.pt"
mv lingbot-map.pt ./lingbot_map/models/checkpoints/lingbot-map.pt
cd ..

#build the executable
mkdir -p ./build
cd ./build
sudo rm -rf ./*
cmake ..
cmake --build . -j$(nproc)

# the executable is main.
