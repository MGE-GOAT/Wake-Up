"""Generate golden.txt: inputs and outputs of the PYTHON framework for the C++ unit tests.

    python tests/golden/make_golden.py <path-to-python-framework> <final default.yaml> [out.txt]

Each line is `tag count v1 v2 ...`; the C++ test replays the inputs and compares with the outputs.
"""
import json
import sys
from dataclasses import asdict

framework, config_path = sys.argv[1], sys.argv[2]
out_path = sys.argv[3] if len(sys.argv) > 3 else "golden.txt"
sys.path.insert(0, framework)
sys.path.insert(0, framework + "/tests")

import numpy as np

from falldet.bbox.person_box_module import BBoxModule, FrameBox, _Obs
from falldet.config import load_config
from falldet.data.video_io import SampledFrame, canonical_timeline
from falldet.fall import representation as R
from falldet.fall.classic_stage import ClassicStage
from falldet.fall.deep_stage import DeepStage
from falldet.fall.detector import FallDetector
from falldet.vision.rtmpose_onnx import PoseResult, RTMPoseEstimator
from falldet.vision.yolo_onnx import YoloPersonDetector
from falldet.vision.geometry import letterbox

cfg = load_config(config_path)
lines = []


def emit32(tag, values):
    values = np.asarray(values, dtype=np.float32).reshape(-1)
    lines.append(f"{tag} {len(values)} " + " ".join("%.9g" % float(v) for v in values))


def emit(tag, values):
    values = np.asarray(values, dtype=np.float64).reshape(-1)
    lines.append(f"{tag} {len(values)} " + " ".join(repr(float(v)) for v in values))


# ------------------------------------------------------------------ signatures
specs = [R.FeatureSpec(point_order=o, channels=c) for o in ("legacy", "chain") for c in (2, 3)]
specs.append(R.FeatureSpec(batch_bbox_margin=1.5, smooth_sigma=1.0, smooth_half_window=2))
for i, s in enumerate(specs):
    emit(f"sig{i}.spec", [0 if s.point_order == "legacy" else 1, s.channels, s.batch_bbox_margin, s.smooth_half_window, s.smooth_sigma])
    lines.append(f"sig{i}.value {s.signature()}")

# ------------------------------------------------------------------ representation windows
for i, spec in enumerate(specs):
    rng = np.random.default_rng(100 + i)
    t = np.arange(16)
    x1 = 100 + 2 * t + rng.uniform(0, 3, 16)
    y1 = 50 + t + rng.uniform(0, 3, 16)
    boxes = np.stack([x1, y1, x1 + rng.uniform(30, 60, 16), y1 + rng.uniform(80, 120, 16)], axis=1)
    kp = rng.uniform([100, 50], [160, 170], size=(16, 17, 2))
    sc = rng.uniform(0.0, 1.0, size=(16, 17))
    sc[3, :5] = 0.0005                       # exercise the 1e-3 floor of the head weights
    sc[10, :] = 0.0                          # a frame the pose model does not trust at all
    w = R.build_window(boxes, kp, sc, spec)
    emit(f"rep{i}.boxes", boxes); emit(f"rep{i}.kp", kp); emit(f"rep{i}.sc", sc)
    emit(f"rep{i}.f80", w.features80); emit(f"rep{i}.tensor", w.cnn_tensor)
    emit(f"rep{i}.conf", [w.mean_confidence, w.min_frame_confidence])

# ------------------------------------------------------------------ classic stage
N = 160
idx = np.arange(N)
cx = 60 + 0.3 * idx
cy = 100 + 35 * np.sin(0.18 * idx) ** 2
bw = 30 + 25 * np.sin(0.07 * idx) ** 2
bh = 90 - 45 * np.sin(0.05 * idx) ** 2
box_arr = np.stack([cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2], axis=1)
missing = np.zeros(N, bool); missing[30:38] = True; missing[90] = True
emit("classic.boxes", box_arr); emit("classic.missing", missing.astype(float))


class SigmoidSvm:
    threshold = None
    def predict_proba(self, x):
        return 1.0 / (1.0 + np.exp(-10.0 * (x[:, 40] - 0.5)))


spec = R.FeatureSpec.from_cfg(cfg)
stage = ClassicStage(cfg.classic, spec, SigmoidSvm())
prob = np.full(N, -1.0); s1 = np.full(N, -1.0); s2 = np.full(N, -1.0); s3 = np.full(N, -1.0)
for c in range(0, N, 16):
    fbs = [FrameBox(i, None if missing[i] else box_arr[i], "yolo", 0.9) for i in range(c, c + 16)]
    u = stage.advance(fbs)
    for name, arr in (("svm_prob", prob), ("s1", s1), ("s2", s2), ("s3", s3)):
        for k, v in getattr(u, name).items():
            arr[k] = v
emit("classic.svm_prob", prob); emit("classic.s1", s1); emit("classic.s2", s2); emit("classic.s3", s3)

# ------------------------------------------------------------------ detector scenarios
class ScriptedBBox:
    multi_person_confirmed = False
    def __init__(self, boxes, missing, lookahead=18):
        self.boxes, self.missing, self.lookahead_frames, self.n, self.next = boxes, missing, lookahead, 0, 0
    def process_batch(self, frames):
        self.n = frames[-1].index + 1
        out = []
        while self.next <= self.n - 1 - self.lookahead_frames:
            i = self.next
            out.append(FrameBox(i, None if (i < len(self.missing) and self.missing[i]) else self.boxes[min(i, len(self.boxes) - 1)], "yolo", 0.9))
            self.next += 1
        return out


class OneSvm:
    threshold = None
    def predict_proba(self, x):
        return np.ones(len(x))


class SlopeCnn:
    threshold = None
    def predict_proba(self, x):
        return np.where(x[:, -1, 0, 1] > x[:, 0, 0, 1], 0.99, 0.1)


class AlwaysCnn:
    threshold = None
    def predict_proba(self, x):
        return np.full(len(x), 0.99)


class LinPose:
    def estimate(self, images, boxes):
        out = []
        for b in boxes:
            out.append(PoseResult(np.stack([np.linspace(b[0], b[2], 17), np.linspace(b[1], b[3], 17)], axis=1), np.full(17, 0.8)))
        return out


def run_scenario(name, boxes, missing, svm, cnn, n_batches):
    bb = ScriptedBBox(boxes, missing)
    det = FallDetector(cfg, bb, ClassicStage(cfg.classic, spec, svm), DeepStage(cfg.deep, spec, LinPose(), cnn), True)
    img = np.zeros((48, 64, 3), np.uint8)
    results = []
    for b in range(n_batches):
        frames = [SampledFrame(16 * b + i, (16 * b + i) / 16.0, 16 * b + i, 0.0, img) for i in range(16)]
        results.append(det.push_batch(frames))
    results += det.flush()
    emit(f"det.{name}.boxes", boxes); emit(f"det.{name}.missing", np.asarray(missing, float)); emit(f"det.{name}.n_batches", [n_batches])
    emit(f"det.{name}.lookahead", [det.lookahead_frames, det.latency_batches])
    emit(f"det.{name}.aligned", [[int(r.valid), -1 if r.target_batch is None else r.target_batch, int(r.fall)] for r in results])
    emit(f"det.{name}.events", [[e.event_id, e.window_start, e.window_end, e.event_frame, e.probability, e.decided_at_frame,
                                  e.latency_frames, int(e.involves_padding)] for e in det.events] or [0])
    status = {"ok": 0, "low_pose_confidence": 1, "missing_data": 2}
    emit(f"det.{name}.windows", [[w.start, w.end, int(w.positive), status[w.status], -1 if w.probability is None else w.probability]
                                  for w in det.window_log] or [0])
    emit(f"det.{name}.s3", [-1 if det.history.get(i) is None or det.history[i].s3 is None else det.history[i].s3 for i in range(16 * n_batches)])
    if name == "motion":                                   # a Python-schema result file for the writer/compare-tool test
        n_real = 16 * n_batches
        result = {"video_id": "scenario/scenario_motion.avi", "path": "/data/scenario/Videos/scenario_motion.avi",
                  "n_frames": n_real, "target_fps": 16, "lookahead_frames": det.lookahead_frames,
                  "latency_batches": det.latency_batches, "gt": {"has_fall": False}, "src_index": list(range(n_real)),
                  "records": [asdict(det.history[k]) for k in range(n_real) if k in det.history],
                  "events": [asdict(e) for e in det.events],
                  "batches": [{"batch": r.batch_index, "valid": r.valid, "target_batch": r.target_batch, "fall": r.fall,
                               "suspended": r.suspended} for r in results],
                  "deep_windows": [asdict(w) for w in det.window_log]}
        import os
        with open(os.path.join(os.path.dirname(os.path.abspath(out_path)), "python_result_motion.json"), "w") as handle:
            json.dump(result, handle)
    emit(f"det.{name}.cnn_prob", [-1 if det.history.get(i) is None or det.history[i].cnn_prob is None else det.history[i].cnn_prob for i in range(16 * n_batches)])


const_box = np.tile([20.0, 10.0, 40.0, 50.0], (400, 1))
miss1 = np.zeros(400, bool); miss1[50:58] = True; miss1[200] = True
run_scenario("steady", const_box, miss1, OneSvm(), AlwaysCnn(), 24)
motion_miss = np.zeros(400, bool); motion_miss[120:126] = True
run_scenario("motion", np.concatenate([box_arr, box_arr, box_arr[:80]])[:400], motion_miss, SigmoidSvm(), SlopeCnn(), 24)

# ------------------------------------------------------------------ track fusion
fz = BBoxModule(cfg.bbox, None)
rng = np.random.default_rng(7)
obs_all = {}
for i in range(100):
    base = np.array([50 + 0.4 * i, 40 + 0.2 * i, 90 + 0.4 * i, 140 + 0.2 * i])
    if i % 8 == 7 and not (40 <= i < 70):
        obs_all[i] = _Obs(base + rng.normal(0, 0.5, 4), "yolo", float(rng.uniform(0.3, 0.9)))
    elif not (45 <= i < 50 or 52 <= i < 76):
        obs_all[i] = _Obs(base + rng.normal(0, 2.0, 4), "bg", cfg.bbox.fusion.bg_score)
code = {None: 0, "yolo": 1, "bg": 2, "interp": 3}
emit("fusion.obs", [[i, o.box[0], o.box[1], o.box[2], o.box[3], code[o.source], o.score] for i, o in sorted(obs_all.items())])
first = {i: o for i, o in obs_all.items() if i < 60}
fz.obs = dict(first); fz.n_received = 60
out = fz._finalize(60 - 1 - fz.lookahead_frames)
fz.obs.update({i: o for i, o in obs_all.items() if i >= 60}); fz.n_received = 100
out += fz._finalize(99)
emit("fusion.out", [[f.index, 0 if f.box is None else 1] + ([0] * 4 if f.box is None else list(f.box)) + [code[f.source], f.score] for f in out])
emit("fusion.lookahead", [fz.lookahead_frames])

# ------------------------------------------------------------------ YOLO decode + letterbox
det = YoloPersonDetector.__new__(YoloPersonDetector)
det.size, det.conf, det.iou_nms, det.person_class = 320, cfg.bbox.yolo.conf, cfg.bbox.yolo.iou_nms, 0
cases = [(320, 240), (184, 184), (128, 128), (640, 360), (300, 500)]
for ci, (w, h) in enumerate(cases):
    img = np.zeros((h, w, 3), np.uint8)
    _, scale, (px, py) = letterbox(img, 320)
    rng = np.random.default_rng(300 + ci)
    pred = np.zeros((84, 2100), np.float32)
    pred[4:, :] = rng.uniform(0, 0.15, (80, 2100))
    pred[4, :] = rng.uniform(0, 0.15, 2100)
    cand = []
    for k in range(6):
        a = int(rng.integers(0, 2100)); cxx, cyy = rng.uniform(80, 240, 2); ww, hh = rng.uniform(40, 150, 2)
        pred[0:4, a] = [cxx, cyy, ww, hh]; pred[4, a] = rng.uniform(0.15, 0.95)
        b = (a + 1) % 2100                                   # near-duplicate to exercise NMS
        pred[0:4, b] = [cxx + 2, cyy - 1, ww + 3, hh + 2]; pred[4, b] = pred[4, a] * 0.9
    res = det._decode(pred, (scale, (px, py), w, h))
    emit(f"yolo{ci}.meta", [w, h, scale, px, py])
    keep = np.flatnonzero(pred[4] >= 0.1)
    emit32(f"yolo{ci}.cand", np.concatenate([keep[:, None].astype(np.float32), pred[:5, keep].T], axis=1))
    emit(f"yolo{ci}.out", [[d.box[0], d.box[1], d.box[2], d.box[3], d.score] for d in res] or [0])

# ------------------------------------------------------------------ SimCC decode (through the real estimator code)
class FakeSession:
    def __init__(self, sx, sy): self.sx, self.sy = sx, sy
    def run(self, names, feed): return [self.sx, self.sy]


for ci, box in enumerate([[100, 40, 180, 230], [10, 10, 300, 100], [0, 0, 5, 5]]):
    rng = np.random.default_rng(500 + ci)
    sx = rng.normal(0, 1, (1, 17, 384)).astype(np.float32); sy = rng.normal(0, 1, (1, 17, 512)).astype(np.float32)
    sx[0, 3] = -np.abs(sx[0, 3]) - 0.1                          # a keypoint whose score is <= 0
    est = RTMPoseEstimator.__new__(RTMPoseEstimator)
    est.session, est.input_name, est.in_w, est.in_h = FakeSession(sx, sy), "input", 192, 256
    est.padding, est.split, est.rgb_input, est.max_batch = 1.25, 2.0, True, 32
    r = est.estimate([np.zeros((240, 320, 3), np.uint8)], [box])[0]
    emit(f"simcc{ci}.box", box); emit32(f"simcc{ci}.x", sx); emit32(f"simcc{ci}.y", sy)
    emit(f"simcc{ci}.out", np.concatenate([r.keypoints.reshape(-1), r.scores]))

# ------------------------------------------------------------------ resampler
for ci, (fps, n) in enumerate([(25, 100), (30, 64), (32, 64), (16, 32), (24, 50), (29.97, 100), (60, 200), (12, 40)]):
    tl = canonical_timeline(n, fps, 16)
    emit(f"resample{ci}.meta", [fps, n]); emit(f"resample{ci}.out", [[k, s] for k, s, _ in tl])

with open(out_path, "w") as handle:
    handle.write("\n".join(lines) + "\n")
print("wrote", out_path, len(lines), "records")
