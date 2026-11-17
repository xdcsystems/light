# `misc/` — Untracked Local Artifacts

This directory holds local binary artifacts (RKNN models, Python wheels, reference clips, and ad-hoc CSV logs) that are excluded from Git by [`.gitignore`](../.gitignore). Only this `README.md` is tracked.

For model conversion/licensing details, see [`THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md).

## Contents

- `rknn_toolkit_lite2-2.5.0-cp39-cp39-manylinux_2_17_aarch64.manylinux2014_aarch64.whl`  
  Offline Python wheel for `rknn-toolkit-lite2==2.5.0` (`aarch64`, `cp39`), downloaded and SHA-256 verified by [`scripts/install_rknn.sh`](../scripts/install_rknn.sh).
- `ref_evening.mjpeg`  
  Reference 1,063-frame MJPEG test clip used by Layer A regression checks ([`reference/README.md`](../reference/README.md)).

## Models (Located in `misc/`)

All `.rknn` model files live in `misc/` (never in the `light_ai/` root):

- `yolov5s_relu.rknn` — FP16 baseline model (M2.1 accuracy reference for A/B checks).
- `yolov5s_relu_int8.rknn` — Initial KLD INT8 build (2026-10-24; superseded by `v2`).
- `yolov5s_relu_int8_v2.rknn` — Validated INT8 build (2026-10-26, `dataset_mix`).
- `yolov5s_relu_int8_mmse.rknn` — Early MMSE INT8 build (`dataset_mix`; superseded by `v5`).
- `yolov5s_relu_int8_v3.rknn` — MMSE INT8 calibrated on 60 daytime frames (`1/255` canonical input scale, M2.4).
- `yolov5s_relu_int8_v4.rknn` — MMSE INT8 calibrated on 960p daytime frames (`dataset_day3`).
- `yolov5s_relu_int8_mmse_v5.rknn` — **Active default model** since 2026-11-01 (MMSE INT8 calibrated on 30 night + 20 day frames). Configured as the default `--model` value in [`include/config/config.hpp`](../include/config/config.hpp); see [Run & CLI Reference](../README.md#run--cli-reference).
