#!/bin/bash
set -e

ENV_NAME="lingbot-map"

USE_SDPA=true
# set to fall to enable FlashInfer for faster inference with more vram usage, not required here

# Enable per-keyframe KV cache & VRAM growth tracking
export LINGBOT_DEBUG_KV=1

KV_CACHE_SLIDING_WINDOW=77 #max on rtx4060ti is 78
# depends on number of scene images available and vram available. 
# More vram = more scene images = better kv cache

KEYFRAME_INTERVAL=1
# Manually tune this and KV_CACHE_SLIDING_WINDOW based on Vram available and scene images needed to cover whole scene.

MODE="streaming" 
MODEL_PATH="lingbot-map/lingbot_map/models/checkpoints/lingbot-map.pt"
IMAGE_FOLDER="$HOME/Pictures/floor1_normal/denoised_frames"

NUM_SCALE_FRAMES=4 
# more = better, but this will spike on vram usage initially.

POINTCLOUD_DOWNSAMPLE_FACTOR=10
DEPTH_CONF_THRESHOLD=1.5

# Check if conda is initialized; if not, initialize it
if ! command -v conda &> /dev/null; then
    if [ -f "$HOME/miniconda3/etc/profile.d/conda.sh" ]; then
        source "$HOME/miniconda3/etc/profile.d/conda.sh"
    elif [ -f "$HOME/anaconda3/etc/profile.d/conda.sh" ]; then
        source "$HOME/anaconda3/etc/profile.d/conda.sh"
    elif [ -f "/opt/conda/etc/profile.d/conda.sh" ]; then
        source "/opt/conda/etc/profile.d/conda.sh"
    fi
fi

if ! type conda 2>/dev/null | grep -q 'function'; then
    eval "$(conda shell.bash hook)"
fi

# Check if currently in lingbot-map environment; if not, activate it
if [ "$CONDA_DEFAULT_ENV" != "$ENV_NAME" ]; then
    echo "Activating conda environment '$ENV_NAME' (current: '${CONDA_DEFAULT_ENV:-none}')..."
    conda activate "$ENV_NAME"
fi


ARGS=()
if [ "$USE_SDPA" = true ]; then
    ARGS+=(--use_sdpa)
fi

ARGS+=(
    --kv_cache_sliding_window "$KV_CACHE_SLIDING_WINDOW"
    --mode "$MODE"
    --keyframe_interval "$KEYFRAME_INTERVAL"
    --model_path "$MODEL_PATH"
    --image_folder "$IMAGE_FOLDER"
    --num_scale_frames "$NUM_SCALE_FRAMES"
    --downsample_factor "$POINTCLOUD_DOWNSAMPLE_FACTOR"
    --conf_threshold "$DEPTH_CONF_THRESHOLD"
)

python lingbot-map/demo.py "${ARGS[@]}"
