# Fall detector - C++ for Raspberry Pi 4

Camera frames (16 FPS) in, one Boolean `fall_label` per 16-frame batch out. It is a C++ port of the main
pipeline of the Python fall-detection framework and uses the models you trained there:

```
frames -> person box (yolo11n ncnn every 8 frames on a motion ROI + background subtraction + smoothing)
       -> 80-d vector -> SVM -> high-sensitivity rule -> post-processing 1/2 -> runs of suspicious frames
       -> RTMPose-t (ONNX) -> 16x15x3 tensor -> CNN (ONNX) -> fall_label
```

## Layout
```
config.yaml            all settings (values copied from your final Python config) + threads, camera, infer
CMakeLists.txt, build.sh
models/                yolo11n-320.ncnn.{param,bin}, RTMPose-t.onnx, svm.onnx/.meta.json, cnn.onnx/.meta.json
src/
  main.cpp             camera mode and infer mode
  pipeline.*           loads the models once, wires one detector per stream/video
  detector.*           the module: push_batch(16 frames) -> BatchResult{fall_label, ...}, the streaming contract
  classic_stage.*      SVM + post-processing           deep_stage.*   RTMPose + CNN on runs of suspicious frames
  representation.*     feature definitions (80-d vector, 16x15xC tensor, signature)
  person_box_module.*, background.*, track_fusion.*      person boxes
  yolo_ncnn.*, onnx_models.*, vision_utils.*             model wrappers and decoding
  video_source.*, resampler.h                            video file / camera -> canonical 16 FPS frames
  config.*, json_util.*, result_writer.*                 config, *.meta.json, result files
tests/                 golden tests against the Python implementation (no external libraries needed)
tools/compare_with_python.py
```

## Build on the Pi
Requirements as in your sample projects: OpenCV (system), ncnn (`~/ncnn/build/install`), ONNX Runtime in
`deps/onnxruntime/{include,lib}` (or `-DORT_ROOT=...`).
```
chmod +x build.sh

./build.sh                                     # or: ./build.sh -DNCNN_DIR=/path/to/ncnn/install -DORT_ROOT=/path/to/onnxruntime
```
`build.sh` also runs the golden tests (`ctest`); they need no models and no libraries.

## Run
```
./build/fall_detector config.yaml camera       # live: one line per batch, fall_label=0/1
./build/fall_detector config.yaml infer        # videos listed in config.yaml -> results_cpp/<video>.json
```
Camera mode prints, per batch: `batch k valid=.. about_batch=.. fall_label=..` (and `FALL event` lines the moment an
event is decided, which is earlier than the aligned `fall_label`). Ctrl-C stops it. A capture thread reads the camera
independently; if processing falls behind real time, the oldest frames are dropped with a warning.

### Threads (config.yaml, `runtime:`)
`yolo_threads` (ncnn), `pose_threads` (RTMPose-t), `classifier_threads` (SVM and CNN). The models run one after the other
in the processing thread, so giving each of them up to 4 threads does not make them compete.

## `fall_label` contract
Identical to the Python module: a decision needs future frames (box smoothing 18 frames + cascade 21 frames), so the
answer for call `k` is about batch `k-3` (`latency_batches = ceil(39/16) = 3`, computed from the config). The first 3 calls
have `valid=false`. `fall_label` is true iff a CNN window ended in that batch and fired; after an event the cascade
is silent for `cooldown_batches` (6) batches. In code: `detector.push_batch(frames)` returns `BatchResult` with
`valid`, `target_batch`, `fall_label`, `new_events` and per-stage timings.

## Check the C++ port against the Python results
1. On the Python side you already have `workspace/results/*.json` from `--mode infer`.
2. Make the video list of a split and copy the videos (same `<group>/Videos/<file>` layout) to the Pi:
   `python tools/compare_with_python.py list --manifest workspace/manifests/splits.json --splits test --out videos.txt --replace /old/data/root /pi/data/root`
3. In `config.yaml` set `mode: infer` and `infer.video_list_file: videos.txt`, then run `./build/fall_detector config.yaml infer`.
4. Copy `results_cpp/` back (or copy the Python results to the Pi) and run
   `python tools/compare_with_python.py compare --python-dir workspace/results --cpp-dir results_cpp`.

It reports per video: event frames, whether the aligned `fall_label` sequences are identical, agreement of the SVM/post-processing
flags (s1/s2/s3), box IoU, and the largest SVM/CNN probability differences. Exit code 0 only if every video matches.
Small differences are expected where the two implementations use different numerics: YOLO runs through ncnn (Python used
ONNX Runtime), so a person box can differ by a pixel or two, and a borderline SVM/CNN score can then flip a flag. Use
`--event-tol N` to allow event frames to differ by N frames. Larger or systematic differences point to a porting error.

## What the tests prove, and what they do not
`tests/test_core.cpp` replays inputs recorded from the Python implementation (`tests/golden/make_golden.py`, run with your final
config) through the C++ code and compares outputs: feature signatures (SHA-1 identical to the Python value in the `.meta.json`
files), the 80-d vectors and CNN tensors (both point orders, 2 and 3 channels), the classic stage (SVM flags, post-processing 1 and 2),
two complete detector scenarios (aligned `fall_label` sequence, events with decision frames, deep-window log, per-frame flags),
box fusion/smoothing, YOLO decoding with NMS, SimCC pose decoding, the 16 FPS resampler for 8 frame rates, and config parsing of your
real `default.yaml`. All 78 checks passed, and they fail when a setting is changed (checked by mutation).

Not covered by any test here: the parts that call OpenCV, ncnn and ONNX Runtime (`person_box_module`, `background`,
`yolo_ncnn`, `onnx_models`, `video_source`, `pipeline`, `main`) and the CMake files. They were ported line by line from the
Python code and only syntax/type-checked against stub headers; they have **not** been compiled against the real libraries or run.
The compare tool exists to close exactly that gap on your Pi.

## Real-time budget (your measurements, not a benchmark of this code)
Per 16-frame batch (1 s of video): YOLO runs twice, 2 x 145 ms = about 290 ms with 4 threads (490 ms with 1 thread).
RTMPose-t is needed for every new frame while the cascade is suspicious: up to 16 x 70-120 ms = 1.1-1.9 s per batch, which is
more than the 1 s available. So during a long suspicious stretch the program falls behind real time (latency grows and, after
`camera.max_backlog_batches`, frames are dropped with a warning); normal scenes with few suspicious frames stay within budget.
Set `camera.log_timing: true` to see the milliseconds per stage and batch on your Pi before deciding on changes.

## Differences from your two sample projects
* Pose input follows the Python framework (RGB, `pose.rgb_input: true`; keypoint score = min of the two SimCC maxima), because the
  svm/cnn were trained on features computed that way. `rtmpose_pi_cpp` uses BGR and the mean of the maxima.
* YOLO pre-processing is the Python letterbox (cv::resize + padding 114), not `from_pixels_resize`, so ncnn sees the same pixels
  as the ONNX model did.
* No `-ffast-math` (it is on in the ncnn sample): results should stay comparable with Python.
