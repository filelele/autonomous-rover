#!/bin/bash
set -e

# export TAILSCALE_PHONE_IP = "100.x.x.x"
# export PI_IP="192.168.x.x"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Shared dependencies
if [ ! -d "$ROOT_DIR/src/Shared/libdatachannel" ]; then
    git clone --recursive --depth 1 https://github.com/paullouisageneau/libdatachannel.git -b v0.23.3 "$ROOT_DIR/src/Shared/libdatachannel"
fi

# Build for Phone side (creates apk and ndk symlink)
(cd "$ROOT_DIR/src/Phone" && bash ./create_apk.sh)

# Build for Server side (creates Server ground control station executable)
(cd "$ROOT_DIR/src/Server" && bash ./create_executable.sh)