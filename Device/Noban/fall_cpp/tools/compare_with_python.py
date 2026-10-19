#!/usr/bin/env python3
"""Compare the C++ results (infer mode) with the Python framework's infer results.

  list     write the video paths of a split from the Python manifest (input of the C++ infer mode)
             python tools/compare_with_python.py list --manifest workspace/manifests/splits.json \
                    --splits test --out videos.txt [--replace /old/prefix /new/prefix]
  compare  diff the result files of both implementations, video by video
             python tools/compare_with_python.py compare --python-dir workspace/results --cpp-dir results_cpp

Videos are matched by "<group folder>/<file name>" (the folder above `Videos`), so the data layout on
the Pi only has to keep the group folder and file names.
"""
import argparse
import json
import sys
from pathlib import Path, PurePosixPath


def video_key(path: str) -> str:
    parts = PurePosixPath(path.replace("\\", "/")).parts
    parent = parts[-2] if len(parts) >= 2 else ""
    group = parts[-3] if parent.lower() == "videos" and len(parts) >= 3 else parent
    return f"{group}/{parts[-1]}"


def cmd_list(args) -> int:
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    wanted = set(args.splits)
    paths = []
    for entry in manifest["videos"]:
        if entry["split"] in wanted:
            p = entry["path"]
            if args.replace and p.startswith(args.replace[0]):
                p = args.replace[1] + p[len(args.replace[0]):]
            paths.append(p)
    Path(args.out).write_text("\n".join(paths) + "\n", encoding="utf-8")
    print(f"wrote {len(paths)} paths to {args.out}")
    return 0


def iou(a, b) -> float:
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def compare_video(py: dict, cpp: dict, event_tol: int) -> dict:
    out = {"n_frames": (py["n_frames"], cpp["n_frames"])}
    py_ev = [e["event_frame"] for e in py["events"]]
    cpp_ev = [e["event_frame"] for e in cpp["events"]]
    out["events"] = (py_ev, cpp_ev)
    out["events_match"] = len(py_ev) == len(cpp_ev) and all(abs(a - b) <= event_tol for a, b in zip(py_ev, cpp_ev))
    py_al = [(b["batch"], b["fall"]) for b in py["batches"] if b["valid"]]
    cpp_al = [(b["batch"], b["fall"]) for b in cpp["batches"] if b["valid"]]
    out["aligned_match"] = py_al == cpp_al
    pr, cr = {r["index"]: r for r in py["records"]}, {r["index"]: r for r in cpp["records"]}
    common = sorted(set(pr) & set(cr))
    for flag in ("s1", "s2", "s3"):
        both = [(pr[i][flag], cr[i][flag]) for i in common if pr[i][flag] is not None and cr[i][flag] is not None]
        out[flag] = sum(a == b for a, b in both) / len(both) if both else float("nan")
    ious, presence_diff = [], 0
    for i in common:
        a, b = pr[i]["box_norm"], cr[i]["box_norm"]
        if (a is None) != (b is None):
            presence_diff += 1
        elif a is not None:
            ious.append(iou(a, b))
    out["box_presence_diff_frames"] = presence_diff
    out["box_iou_mean"] = sum(ious) / len(ious) if ious else float("nan")
    out["box_iou_min"] = min(ious) if ious else float("nan")
    for name in ("svm_prob", "cnn_prob"):
        diffs = [abs(pr[i][name] - cr[i][name]) for i in common if pr[i][name] is not None and cr[i][name] is not None]
        out[name + "_max_diff"] = max(diffs) if diffs else float("nan")
        out[name + "_only_one_side"] = sum((pr[i][name] is None) != (cr[i][name] is None) for i in common)
    return out


def cmd_compare(args) -> int:
    py = {}
    for f in Path(args.python_dir).glob("*.json"):
        d = json.loads(f.read_text(encoding="utf-8"))
        if "records" in d and "video_id" in d:
            py[d["video_id"]] = d
    cpp = {}
    for f in Path(args.cpp_dir).glob("*.json"):
        d = json.loads(f.read_text(encoding="utf-8"))
        if "records" in d:
            cpp[video_key(d["path"])] = d
    keys = sorted(set(py) & set(cpp))
    for k in sorted(set(cpp) - set(py)):
        print(f"[no python result] {k}")
    if not keys:
        print("no video present in both result folders")
        return 2
    bad = 0
    for k in keys:
        r = compare_video(py[k], cpp[k], args.event_tol)
        ok = r["events_match"] and r["aligned_match"] and r["s3"] >= args.min_flag_agreement
        bad += not ok
        print(f"{'OK  ' if ok else 'DIFF'} {k}: events py={r['events'][0]} cpp={r['events'][1]} | aligned fall_label "
              f"{'equal' if r['aligned_match'] else 'DIFFERENT'} | s1/s2/s3 agreement {r['s1']:.3f}/{r['s2']:.3f}/{r['s3']:.3f} | "
              f"box IoU mean {r['box_iou_mean']:.3f} min {r['box_iou_min']:.3f}, presence differs on {r['box_presence_diff_frames']} frames | "
              f"max |svm diff| {r['svm_prob_max_diff']:.2e}, max |cnn diff| {r['cnn_prob_max_diff']:.2e}")
    print(f"\n{len(keys) - bad}/{len(keys)} videos match (events within {args.event_tol} frames, identical aligned fall_label "
          f"sequence, s3 agreement >= {args.min_flag_agreement})")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("list")
    l.add_argument("--manifest", required=True)
    l.add_argument("--splits", nargs="+", default=["test"])
    l.add_argument("--out", required=True)
    l.add_argument("--replace", nargs=2, metavar=("OLD_PREFIX", "NEW_PREFIX"))
    c = sub.add_parser("compare")
    c.add_argument("--python-dir", required=True)
    c.add_argument("--cpp-dir", required=True)
    c.add_argument("--event-tol", type=int, default=0, help="allowed difference of event frames (default 0)")
    c.add_argument("--min-flag-agreement", type=float, default=0.98)
    args = ap.parse_args()
    return cmd_list(args) if args.cmd == "list" else cmd_compare(args)


if __name__ == "__main__":
    sys.exit(main())
