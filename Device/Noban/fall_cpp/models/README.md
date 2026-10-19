Put these files here (the names are set in config.yaml):

| file | source |
|---|---|
| `yolo11n-320.ncnn.param`, `yolo11n-320.ncnn.bin` | `yolo export model=yolo11n.pt format=ncnn imgsz=320` |
| `RTMPose-t.onnx` | the same file the Python framework uses |
| `svm.onnx`, `svm.meta.json` | produced by the Python `train_svm` mode |
| `cnn.onnx`, `cnn.meta.json` | produced by the Python `train_cnn` mode |

The `.meta.json` files carry the feature signature (checked against config.yaml), the SVM threshold and the
class order; keep each one next to its `.onnx` file.
