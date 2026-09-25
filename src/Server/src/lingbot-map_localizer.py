import os
import struct
import sys
import time
from typing import Any, Dict, Optional

# Cap CPU thread pools BEFORE importing torch/numpy so the localizer doesn't
# starve the main process's UI/decode threads.
os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("NUMEXPR_NUM_THREADS", "1")

import cv2
import numpy as np
import torch
import yaml

torch.set_num_threads(1)
torch.set_num_interop_threads(1)

LINGBOT_MAP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "lingbot-map"))
if LINGBOT_MAP_DIR not in sys.path:
    sys.path.insert(0, LINGBOT_MAP_DIR)

from lingbot_map.models.gct_stream import GCTStream


class LingbotMapLocalizer:
    """
    Real-time Camera Localizer using Precomputed Map Descriptors and Poses.

    Workflow:
    - Loads precomputed reference descriptors and poses from occupancy_grid/map_reference.npz.
    - Extracts DINOv2 mean descriptor for live query frames via backbone patch_embed.
    - Performs cosine similarity search against all reference map frames.
    - Selects Top-3 closest frames with spatial outlier filtering.
    - Computes a softmax-weighted average of position (tx, tz) and circular average of heading (yaw).
    """

    def __init__(
        self,
        model_path: str,
        image_size: int = 518,
        patch_size: int = 14,
        device: Optional[str] = None,
        **kwargs,
    ):
        self.model_path = model_path
        self.image_size = image_size
        self.patch_size = patch_size

        self.map_ready = False
        self.live_frame_counter = 0

        # Reference map data loaded from map_reference.npz
        self.map_descriptors: Optional[torch.Tensor] = None   # [N, 1024]
        self.map_patch_tokens: Optional[torch.Tensor] = None  # [N, 1036, 1024]
        self.map_c2w: Optional[np.ndarray] = None             # [N, 4, 4]
        self.map_positions: Optional[np.ndarray] = None       # [N, 3]
        self.map_yaws: Optional[np.ndarray] = None            # [N]

        # Device & precision
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

        # Precompute spatial window mask for Fine Stage (1/5 of 518 width = 7 patches)
        n_rows, n_cols = 28, 37
        rows = torch.arange(n_rows).repeat_interleave(n_cols)
        cols = torch.arange(n_cols).repeat(n_rows)
        dr = (rows[:, None] - rows[None, :]).abs()
        dc = (cols[:, None] - cols[None, :]).abs()
        self.window_mask = ((dc <= 7) & (dr <= 5)).to(self.device)

        # Denoising filter parameters
        self.ksize_chroma = (11, 11)
        self.sigma_chroma = 2.5
        self.ksize_luma = (3, 3)
        self.sigma_luma = 1.2

        self.spatial_threshold: float = 1.0  # Computed dynamically in loadReferenceMap

        self.model = self._init_model()

    def _init_model(self) -> GCTStream:
        """Instantiate GCTStream backbone."""
        model = GCTStream(
            img_size=self.image_size,
            patch_size=self.patch_size,
            enable_camera=False,
            enable_point=False,
            enable_local_point=False,
            enable_depth=False,
            enable_3d_rope=False,
            use_sdpa=True,
        )

        if self.model_path and os.path.exists(self.model_path):
            checkpoint = torch.load(self.model_path, map_location=self.device, weights_only=False)
            state_dict = checkpoint.get("model", checkpoint)
            model.load_state_dict(state_dict, strict=False)

        model = model.to(self.device).eval()

        if self.dtype != torch.float32 and hasattr(model, "aggregator") and model.aggregator is not None:
            model.aggregator = model.aggregator.to(dtype=self.dtype)

        return model

    def _denoise_frame(self, frame_bgr: np.ndarray) -> np.ndarray:
        """Apply Gaussian blur to luma (Y) and chroma (Cr, Cb) channels to reduce sensor noise."""
        ycrcb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2YCrCb)
        y, cr, cb = cv2.split(ycrcb)

        y_blur = cv2.GaussianBlur(y, self.ksize_luma, self.sigma_luma)
        cr_blur = cv2.GaussianBlur(cr, self.ksize_chroma, self.sigma_chroma)
        cb_blur = cv2.GaussianBlur(cb, self.ksize_chroma, self.sigma_chroma)

        ycrcb_filtered = cv2.merge([y_blur, cr_blur, cb_blur])
        return cv2.cvtColor(ycrcb_filtered, cv2.COLOR_YCrCb2RGB)

    def _preprocess_numpy_bgr_frame(self, frame_bgr: np.ndarray) -> torch.Tensor:
        """Denoise and resize/crop a single OpenCV BGR image into normalized tensor [1, 3, H, W] in [0, 1]."""
        frame_rgb = self._denoise_frame(frame_bgr)
        h, w, _ = frame_rgb.shape

        target_size = self.image_size
        new_width = target_size
        new_height = int(np.round(h * (new_width / w) / self.patch_size) * self.patch_size)

        resized_img = cv2.resize(frame_rgb, (new_width, new_height), interpolation=cv2.INTER_CUBIC)
        if new_height > target_size:
            start_y = (new_height - target_size) // 2
            resized_img = resized_img[start_y : start_y + target_size, :]

        tensor_img = torch.from_numpy(resized_img).permute(2, 0, 1).float() / 255.0
        return tensor_img.unsqueeze(0).to(self.device)

    @torch.no_grad()
    def _extract_descriptors(self, frame_tensor: torch.Tensor):
        """Extracts normalized patch tokens [1036, 1024] and global mean descriptor [1024]."""
        agg = self.model.aggregator
        norm_img = (frame_tensor - agg._resnet_mean.squeeze(0)) / agg._resnet_std.squeeze(0)
        with torch.amp.autocast("cuda", dtype=self.dtype, enabled=(self.device.type == "cuda")):
            out = agg.patch_embed(norm_img)
            tok = out["x_norm_patchtokens"] if isinstance(out, dict) else out
            norm_patches = torch.nn.functional.normalize(tok.squeeze(0), p=2, dim=-1)
            global_desc = torch.nn.functional.normalize(norm_patches.mean(dim=0), p=2, dim=-1)
            return norm_patches, global_desc

    def loadReferenceMap(self, folder: str):
        """Loads precomputed map reference and occupancy grid metadata."""
        grid_dir = os.path.join(folder, "occupancy_grid")
        npz_path = os.path.join(grid_dir, "map_reference.npz")
        yaml_path = os.path.join(grid_dir, "occupancy_grid.yaml")

        print(f"[Localizer] Loading map reference: {npz_path}", file=sys.stderr)
        data = np.load(npz_path)
        self.map_descriptors = torch.from_numpy(data["descriptors"]).to(self.device).float()
        self.map_patch_tokens = torch.from_numpy(data["patch_tokens"]).to(self.device)
        self.map_c2w = data["c2w"].astype(np.float64)
        self.map_positions = data["positions"].astype(np.float64)
        self.map_yaws = data["yaws"].astype(np.float64)

        with open(yaml_path, "r") as f:
            map_cfg = yaml.safe_load(f)

        resolution = float(map_cfg["resolution"])
        grid_size = map_cfg["grid_size"]
        map_w_m = float(grid_size[0]) * resolution
        map_h_m = float(grid_size[1]) * resolution
        self.spatial_threshold = min(map_w_m, map_h_m) / 10.0

        self.map_ready = True
        print(
            f"[Localizer] Loaded {len(self.map_positions)} reference frames. "
            f"Map: {map_w_m:.2f}m x {map_h_m:.2f}m -> Spatial clustering dist <= {self.spatial_threshold:.2f}m",
            file=sys.stderr,
            flush=True,
        )

    def processFrame(self, frame_input: Any) -> Optional[Dict[str, Any]]:
        """
        Two-stage localization:
        1. Fast coarse global search (Top-5 candidates via mean descriptors).
        2. Fine Windowed MaxSim re-ranking (within 1/5 width window) to verify spatial details.
        3. Top-3 spatial-consistent similarity-weighted position and circular heading averaging.
        """
        if not self.map_ready or self.map_descriptors is None:
            raise RuntimeError("Cannot process query frame before reference map is loaded.")

        if hasattr(frame_input, "bgr"):
            frame_np = frame_input.bgr
        elif isinstance(frame_input, np.ndarray):
            frame_np = frame_input
        else:
            raise TypeError(f"Unsupported frame type: {type(frame_input)}")

        # Preprocess query image and extract patch tokens + global descriptor
        query_tensor = self._preprocess_numpy_bgr_frame(frame_np)
        query_patches, query_global = self._extract_descriptors(query_tensor)

        # Stage 1: Coarse Global Similarity Search (retrieve Top-30 candidates)
        global_sims = torch.mv(self.map_descriptors, query_global.float())
        coarse_k = min(20, len(global_sims))
        _, cand_indices = torch.topk(global_sims, k=coarse_k)

        # Stage 2: Fine Windowed MaxSim Re-Ranking on candidates
        cand_patches = self.map_patch_tokens[cand_indices].to(query_patches.dtype)
        patch_sims = torch.matmul(cand_patches, query_patches.T).transpose(1, 2)
        patch_sims = torch.where(self.window_mask, patch_sims, torch.tensor(-1.0, device=self.device, dtype=query_patches.dtype))
        fine_scores = patch_sims.max(dim=2).values.mean(dim=1)

        # Select Top-3 from the fine re-ranked scores
        fine_k = min(3, coarse_k)
        top_fine_vals, top_fine_order = torch.topk(fine_scores, k=fine_k)
        indices = cand_indices[top_fine_order].cpu().numpy()
        vals = top_fine_vals.cpu().numpy()

        # Spatial outlier filter: discard candidates farther than 1/10 of smaller map side from top-1
        top1_pos = self.map_positions[indices[0]]
        valid_indices = []
        valid_vals = []
        for idx, val in zip(indices, vals):
            dist = np.hypot(
                self.map_positions[idx][0] - top1_pos[0],
                self.map_positions[idx][2] - top1_pos[2],
            )
            if dist <= self.spatial_threshold:
                valid_indices.append(idx)
                valid_vals.append(val)

        valid_indices = np.array(valid_indices, dtype=int)
        valid_vals = np.array(valid_vals, dtype=float)

        # Softmax similarity weighting (temperature = 0.1)
        exp_w = np.exp((valid_vals - valid_vals[0]) / 0.1)
        weights = exp_w / np.sum(exp_w)

        # Weighted average position (tx, ty, tz)
        pred_pos = np.sum(weights[:, None] * self.map_positions[valid_indices], axis=0)

        # Circular weighted average for heading (yaw)
        candidate_yaws = self.map_yaws[valid_indices]
        sin_yaw = np.sum(weights * np.sin(candidate_yaws))
        cos_yaw = np.sum(weights * np.cos(candidate_yaws))
        pred_yaw = np.arctan2(sin_yaw, cos_yaw)

        # Construct 4x4 Camera-to-World (c2w) matrix
        c2w = np.eye(4, dtype=np.float64)
        c2w[0, 0] = np.cos(pred_yaw)
        c2w[0, 2] = np.sin(pred_yaw)
        c2w[2, 0] = -np.sin(pred_yaw)
        c2w[2, 2] = np.cos(pred_yaw)
        c2w[0, 3] = pred_pos[0]
        c2w[1, 3] = pred_pos[1]
        c2w[2, 3] = pred_pos[2]

        self.live_frame_counter += 1
        print(
            f"[Localizer] #{self.live_frame_counter}: Top={indices[0]} (fine={vals[0]:.4f}) | "
            f"matches={valid_indices.tolist()} weights={weights.round(2).tolist()} -> "
            f"Pos=({pred_pos[0]:.2f}, {pred_pos[2]:.2f}), Yaw={np.degrees(pred_yaw):.1f}°",
            file=sys.stderr,
            flush=True,
        )

        return {
            "c2w": c2w,
            "matched_raw_idx": int(indices[0]),
            "similarity": float(vals[0]),
        }


def _run_stdio_server():
    """Stdio command server matching the C++ LingbotMapLocalizer protocol."""
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
                localizer.loadReferenceMap(folder)
                stdout.write(b"OK\n")
            except Exception as e:
                import traceback
                traceback.print_exc()
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
                t0 = time.time()
                result = localizer.processFrame(frame_np)
                dt = time.time() - t0
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