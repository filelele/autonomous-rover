import queue
import sys
import os

# Cap CPU thread pools BEFORE importing torch/numpy so the localizer doesn't
# starve the main process's UI/decode threads.
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("NUMEXPR_NUM_THREADS", "1")

import cv2
import numpy as np
import torch
from typing import List, Union, Optional, Dict, Any

# Also cap torch's own intra/inter-op pools
torch.set_num_threads(1)
torch.set_num_interop_threads(1)

LINGBOT_MAP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "lingbot-map"))
if LINGBOT_MAP_DIR not in sys.path:
    sys.path.insert(0, LINGBOT_MAP_DIR)

from lingbot_map.models.gct_stream import GCTStream
from lingbot_map.utils.load_fn import load_and_preprocess_images
from lingbot_map.utils.pose_enc import pose_encoding_to_extri_intri
from lingbot_map.utils.geometry import closed_form_inverse_se3_general


class LingbotMapLocalizer:
    """
    Wrapper for LingBot-MAP GCTStream model focusing solely on real-time camera localization.
    Builds fixed KV cache over initial anchor frames and performs online pose estimation
    for streaming query frames without growing KV cache memory.
    """

    def __init__(
        self,
        model_path: str,
        num_scale_frames: int = 4,
        kv_cache_sliding_window: int = 20,
        map_keyframe_interval: int = 2,
        query_keyframe_interval: int = 10,
        image_size: int = 518,
        patch_size: int = 14,
        camera_num_iterations: int = 4,
        enable_3d_rope: bool = True,
        use_sdpa: bool = False,
        device: Optional[str] = None,
    ):
        self.model_path = model_path
        self.num_scale_frames = num_scale_frames
        self.kv_cache_sliding_window = kv_cache_sliding_window
        self.map_keyframe_interval = map_keyframe_interval
        self.query_keyframe_interval = query_keyframe_interval
        self.image_size = image_size
        self.patch_size = patch_size
        self.camera_num_iterations = camera_num_iterations
        self.enable_3d_rope = enable_3d_rope
        self.use_sdpa = use_sdpa

        # Queues for I/O thread synchronization
        self.input_queue = queue.Queue()
        self.output_queue = queue.Queue()

        self.fixed_kv_cache_ready = False
        self.live_frame_counter = 0
        self.result = None

        # Determine target device and compute precision
        if device is None:
            self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        else:
            self.device = torch.device(device)

        if self.device.type == "cuda":
            self.dtype = (
                torch.bfloat16
                if torch.cuda.get_device_capability()[0] >= 8
                else torch.float16
            )
        else:
            self.dtype = torch.float32

        self.model = self._init_model()

    def _init_model(self) -> GCTStream:
        """Instantiate GCTStream optimized for localization-only inference."""
        model = GCTStream(
            img_size=self.image_size,
            patch_size=self.patch_size,
            enable_camera=True,
            enable_point=False,        # Disable unused point cloud head
            enable_local_point=False,
            enable_depth=False,        # Disable unused depth head
            enable_3d_rope=self.enable_3d_rope,
            kv_cache_sliding_window=self.kv_cache_sliding_window,
            kv_cache_scale_frames=self.num_scale_frames,
            kv_cache_cross_frame_special=True,
            kv_cache_include_scale_frames=True,
            use_sdpa=self.use_sdpa,
            camera_num_iterations=self.camera_num_iterations,
        )

        if self.model_path:
            checkpoint = torch.load(self.model_path, map_location=self.device, weights_only=False)
            state_dict = checkpoint.get("model", checkpoint)
            model.load_state_dict(state_dict, strict=False)

        model = model.to(self.device).eval()

        # Cast feature aggregator to half-precision to save VRAM
        if self.dtype != torch.float32 and hasattr(model, "aggregator") and model.aggregator is not None:
            model.aggregator = model.aggregator.to(dtype=self.dtype)

        return model

    def _preprocess_numpy_bgr_frame(self, frame_bgr: np.ndarray) -> torch.Tensor:
        """
        Resize/crop a single OpenCV BGR image into normalized tensor [1, 1, 3, H, W]
        matching load_and_preprocess_images(mode="crop") exactly.
        """
        frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        h, w, _ = frame_rgb.shape

        target_size = self.image_size
        new_width = target_size
        new_height = int(np.round(h * (new_width / w) / self.patch_size) * self.patch_size)

        resized_img = cv2.resize(frame_rgb, (new_width, new_height), interpolation=cv2.INTER_CUBIC)

        if new_height > target_size:
            start_y = (new_height - target_size) // 2
            resized_img = resized_img[start_y : start_y + target_size, :]

        # Convert to float tensor [3, H, W] in [0, 1]
        tensor_img = torch.from_numpy(resized_img).permute(2, 0, 1).float() / 255.0

        # Reshape to [B=1, S=1, C=3, H, W]
        return tensor_img.unsqueeze(0).unsqueeze(0).to(self.device)

    def buildKVbyFixedSceneFrames(self, frames_paths: List[str]):
        """
        Builds the initial KV cache from fixed reference scene frames.
        - The first `num_scale_frames` are processed as a scale batch.
        - Subsequent frames are processed sequentially with KV caching enabled.
        """
        if not frames_paths:
            raise ValueError("frames_paths cannot be empty.")

        # Load and preprocess all initial scene images
        # Output shape: [S, 3, H, W]
        images_tensor = load_and_preprocess_images(
            frames_paths,
            mode="crop",
            image_size=self.image_size,
            patch_size=self.patch_size,
        ).to(self.device)

        total_frames = images_tensor.shape[0]
        scale_frames = min(self.num_scale_frames, total_frames)

        self.model.clean_kv_cache()
        self.model._set_skip_append(False)

        with torch.no_grad(), torch.amp.autocast("cuda", dtype=self.dtype, enabled=(self.device.type == "cuda")):
            # Note: output_device must be None so inference_streaming does not call clean_kv_cache() at the end
            self.model.inference_streaming(
                images_tensor,
                num_scale_frames=scale_frames,
                keyframe_interval=self.map_keyframe_interval,
                output_device=None,
            )

        info = self.model.get_kv_cache_info()
        print(f"[wrapper] KV cache after build: {info}", file=sys.stderr)
        if info.get("num_cached_blocks", 0) == 0 or info.get("cache_memory_mb", 0.0) == 0.0:
            raise RuntimeError(f"KV cache is empty or unpopulated after build: {info}")

        self.fixed_kv_cache_ready = True
        self.live_frame_counter = 0
        self._log_kv_cache_tracker(label="Map build complete")

    def _log_kv_cache_tracker(self, label: str = ""):
        """Logs active keyframes, evicted keyframes, payload memory, growth rate, and GPU VRAM."""
        try:
            agg = getattr(self.model, "aggregator", None)
            if agg is None:
                return
            mgr = getattr(agg, "kv_cache_manager", None)
            if mgr is not None and hasattr(mgr, "get_cache_stats"):
                s = mgr.get_cache_stats(block_idx=0)
                frame_count = s['frame_count']
                active_kfs = s['scale_pages'] + s['live_pages']
                evicted_kfs = max(0, frame_count - active_kfs)

                bytes_per_token = 2 * mgr.num_heads * mgr.head_dim * 2 * mgr.num_blocks
                active_mb = (active_kfs * mgr.page_size * bytes_per_token) / (1024 * 1024)
                special_mb = (s['special_tokens'] * bytes_per_token) / (1024 * 1024)
                total_kv_mb = active_mb + special_mb

                d_frames = frame_count - getattr(self, "_last_tracked_frames", 0)
                d_mb = total_kv_mb - getattr(self, "_last_tracked_mb", 0.0)
                rate_str = f"+{d_mb / d_frames:.2f} MB/KF" if d_frames > 0 else "0.00 MB/KF"
                self._last_tracked_frames = frame_count
                self._last_tracked_mb = total_kv_mb

                vram_res = torch.cuda.memory_reserved() / (1024 ** 3) if torch.cuda.is_available() else 0.0
                print(
                    f"[KV Tracker] {label} | Active KFs: {active_kfs} ({active_mb:.1f} MB) | "
                    f"Evicted KFs: {evicted_kfs} ({special_mb:.2f} MB) | "
                    f"KV: {total_kv_mb:.1f} MB ({rate_str}) | VRAM: {vram_res:.2f}G res",
                    file=sys.stderr,
                    flush=True,
                )
        except Exception as e:
            print(f"[KV Tracker] Error logging KV stats: {e}", file=sys.stderr, flush=True)

    def processFrame(self, frame_input: Any) -> Optional[Dict[str, Any]]:
        """
        Processes a single incoming frame from the LingbotMapLocalizer C++ object.
        Applies continuous keyframe streaming so the sliding window advances as the rover drives.

        Returns:
            Dictionary with parsed 6DoF camera poses:
                - "c2w": [4, 4] numpy array (Camera-to-World transform matrix)
                - "extrinsic": [3, 4] numpy array (World-to-Camera or c2w per convention)
                - "intrinsic": [3, 3] numpy array
                - "pose_enc": [9] raw pose encoding tensor
        """
        if not self.fixed_kv_cache_ready:
            raise RuntimeError("Cannot process query frame before buildKVbyFixedSceneFrames is completed.")

        # Extract NumPy array if wrapped in DecodedFrame / struct
        if hasattr(frame_input, "bgr"):
            frame_np = frame_input.bgr
        elif isinstance(frame_input, np.ndarray):
            frame_np = frame_input
        else:
            raise TypeError(f"Unsupported frame type: {type(frame_input)}")

        # Rescale / preprocess frame to tensor [1, 1, 3, H, W]
        query_tensor = self._preprocess_numpy_bgr_frame(frame_np)
        h, w = query_tensor.shape[-2:]

        # Streaming mode: 1 keyframe every query_keyframe_interval (5) live frames enters the KV cache
        is_keyframe = (self.query_keyframe_interval <= 1) or (
            self.live_frame_counter > 0 and self.live_frame_counter % self.query_keyframe_interval == 0
        )
        self.live_frame_counter += 1

        # Skip appending for non-keyframes; append and evict oldest keyframe in the 10-frame window for keyframes
        self.model._set_skip_append(not is_keyframe)

        with torch.no_grad(), torch.amp.autocast("cuda", dtype=self.dtype, enabled=(self.device.type == "cuda")):
            predictions = self.model.forward(
                query_tensor,
                num_frame_for_scale=self.num_scale_frames,
                num_frame_per_block=1,
                causal_inference=True,
            )
        self.model._set_skip_append(False)

        if is_keyframe:
            self._log_kv_cache_tracker(label=f"Query frame {self.live_frame_counter} (KEYFRAME)")

        pose_enc = predictions.get("pose_enc")  # Shape: [1, 1, 9]
        if pose_enc is None:
            return None

        # Convert 9D pose encoding to extrinsics and intrinsics
        extrinsic, intrinsic = pose_encoding_to_extri_intri(pose_enc, (h, w))

        # Convert w2c -> c2w (Camera to World pose)
        extrinsic_4x4 = torch.zeros((*extrinsic.shape[:-2], 4, 4), device=extrinsic.device, dtype=extrinsic.dtype)
        extrinsic_4x4[..., :3, :4] = extrinsic
        extrinsic_4x4[..., 3, 3] = 1.0
        c2w_4x4 = closed_form_inverse_se3_general(extrinsic_4x4)

        result = {
            "c2w": c2w_4x4.squeeze(0).squeeze(0).cpu().numpy(),
            "extrinsic": extrinsic.squeeze(0).squeeze(0).cpu().numpy(),
            "intrinsic": intrinsic.squeeze(0).squeeze(0).cpu().numpy(),
            "pose_enc": pose_enc.squeeze(0).squeeze(0).cpu().numpy(),
        }

        self.result = result
        return result

    def pushFrameAndGetResult(self, frame_input: Any) -> Optional[Dict[str, Any]]:
        """
        Receives a frame from C++, puts it to queue, runs localization, and yields result.
        """
        self.input_queue.put(frame_input)

        if self.fixed_kv_cache_ready:
            localization_result = self.processFrame(frame_input)
            self.output_queue.put(localization_result)

        if not self.output_queue.empty():
            return self.output_queue.get()
        return None


def _run_stdio_server():
    """
    Stdio command server used by the C++ LingbotMapLocalizer.
    Protocol (binary-safe, all reads/writes on raw buffers):
      C -> PY: "BUILD <images_folder>\n"
      PY -> C: b"OK\n" or b"ERR <msg>\n"
      C -> PY: "FRAME <width> <height>\n" followed by exactly width*height*3 raw BGR bytes
      PY -> C: int32 status (-1 failure, 0 success) then, on success,
               16 float64 values (row-major 4x4 camera-to-world matrix)
      C -> PY: "QUIT\n"  ->  process exits
    """
    import sys
    import glob
    import os
    import struct

    # stdout is reserved for the binary protocol with C++.
    # Redirect any library prints / logging to stderr so they don't corrupt it.
    real_stdout = sys.stdout.buffer
    sys.stdout = sys.stderr

    model_path = sys.argv[1] if len(sys.argv) > 1 else ""
    localizer = LingbotMapLocalizer(model_path=model_path)

    stdin = sys.stdin.buffer
    stdout = real_stdout

    while True:
        line = stdin.readline()
        if not line:
            break
        cmd = line.decode().strip()

        if cmd.startswith("BUILD"):
            folder = cmd.split(" ", 1)[1]
            try:
                paths = []
                for ext in ("jpg", "jpeg", "png", "bmp"):
                    paths.extend(sorted(glob.glob(os.path.join(folder, f"*.{ext}"))))
                    paths.extend(sorted(glob.glob(os.path.join(folder, f"*.{ext.upper()}"))))
                if not paths:
                    raise ValueError(f"No images found in {folder}")
                localizer.buildKVbyFixedSceneFrames(paths)
                stdout.write(b"OK\n")
            except Exception as e:
                stdout.write(f"ERR {e}\n".encode())
            stdout.flush()

        elif cmd.startswith("FRAME"):
            try:
                _, w_str, h_str = cmd.split()
                w, h = int(w_str), int(h_str)
                data = stdin.read(w * h * 3)
                if len(data) != w * h * 3:
                    raise ValueError(f"Short frame read: {len(data)} of {w*h*3} bytes")
                frame_np = np.frombuffer(data, np.uint8).reshape(h, w, 3)
                import time as _time
                _t0 = _time.time()
                result = localizer.processFrame(frame_np)
                print(f"[wrapper] FRAME {w}x{h} processed in {_time.time()-_t0:.2f}s",
                      file=sys.stderr)
                if result is None:
                    stdout.write(struct.pack("<i", -1))
                else:
                    stdout.write(struct.pack("<i", 0))
                    stdout.write(result["c2w"].astype(np.float64).tobytes())
            except Exception as e:
                import traceback
                traceback.print_exc()
                stdout.write(struct.pack("<i", -1))
            stdout.flush()

        elif cmd == "QUIT":
            break


if __name__ == "__main__":
    _run_stdio_server()