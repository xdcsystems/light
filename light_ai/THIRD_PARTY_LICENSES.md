# Third-Party Licenses and Compliance Levels

`light_ai` is licensed under the [MIT License](LICENSE). Because the pipeline integrates vendored headers, open-source system libraries, Rockchip proprietary NPU runtime components, and external neural-network models, each layer carries distinct licensing obligations.

## 1. License Hierarchy Summary

| Layer | Component | License | Distributed in Git | Key Obligations / Notes |
|---|---|---|---|---|
| **Project source** | `light_ai` (`include/`, `src/`, `tests/`, `scripts/`, `tools/`) | **MIT** | Yes | Retain copyright and MIT permission notice ([LICENSE](LICENSE)) |
| **Vendored C/C++** | [`externals/inih`](externals/inih/LICENSE.txt) (`ini.c`, `ini.h`) | **BSD-3-Clause** (New BSD) | Yes | Retain Ben Hoyt copyright notice and BSD-3 disclaimer |
| **Vendored fonts** | [`externals/fonts/stb_easy_font.h`](externals/fonts/stb_easy_font.h) | **MIT or Unlicense** (dual) | Yes | Either MIT notice or public domain terms apply |
| **System SDK (OS)** | `rockchip_mpp` (`rk_mpi.h`) | **Apache-2.0** | No (system lib) | Apache-2.0 notice and patent grant |
| **System SDK (OS)** | `librga` (`im2d.h`, `rga.h`) | **Apache-2.0** | No (system lib) | Apache-2.0 notice and patent grant |
| **System Media** | `FFmpeg` (`libavformat`, `libavcodec`, `libavutil`) | **LGPL-2.1+** *(or GPL-2.0+/3.0+)* | No (system lib) | **See FFmpeg Compliance Note below** |
| **System GPU** | `EGL`, `GLESv2`, `libgbm` (Mali / Mesa) | **Vendor / Khronos / MIT** | No (system lib) | Dynamically linked / `dlopen`ed at runtime |
| **NPU Runtime** | Rockchip `rknnrt` (`librknnrt.so`, `rknn_api.h`) | **Proprietary** (Rockchip) | No (SDK runtime) | Rockchip Electronics Co., Ltd. All rights reserved |
| **Models (`.rknn`)** | YOLOv5s detector & OSNet Re-ID models (`misc/*.rknn`) | **AGPL-3.0** *(Ultralytics YOLOv5)* / **Apache-2.0** *(rknn_model_zoo)* / **MIT** *(OSNet / torchreid)* | No (`.gitignore`d) | Not tracked in Git; user-supplied at runtime (`--model`, `--reid-model`) |
| **Class labels** | [`include/coco_names.h`](include/coco_names.h) | **CC BY 4.0** (MS COCO taxonomy) | Yes | Standard 80 COCO class name strings |

---

## 2. Critical Compliance Notes

### FFmpeg LGPL vs. GPL (Important for Binary Redistribution)
`light_ai` dynamically links against `libavformat`, `libavcodec`, and `libavutil` (pinned to `/usr/local/lib/pkgconfig` in [`CMakeLists.txt`](CMakeLists.txt)). At runtime, `light_ai` uses only the `h264_rkmpp` hardware encoder, `rawvideo` NUT muxer, and MPEG-TS muxer, none of which require GPL-only FFmpeg components.

- **LGPL build (recommended for redistribution):** Configure `ffmpeg-rockchip` **without** `--enable-gpl`, `--enable-version3`, `--enable-libx264`, or `--enable-libx265`. In that configuration FFmpeg is **LGPL-2.1-or-later**, which is compatible with dynamic linking alongside proprietary `librknnrt.so`.
- **GPL build (dev board default for CLI tools):** If FFmpeg in `/usr/local/lib` was configured with `--enable-gpl --enable-version3` (for example, to use `libx264`/`libx265` in standalone `ffmpeg` CLI commands), those shared libraries carry **GPLv3** terms. **Do not distribute compiled `light_ai` binaries linked against a GPL-configured FFmpeg together with proprietary `librknnrt.so`**, as GPL copyleft conflicts with closed-source NPU runtime terms.

### Rockchip NPU Runtime (`rknnrt` / `rknn-toolkit-lite2`)
- `rknn_api.h` and `librknnrt.so` are proprietary Rockchip SDK deliverables (`Copyright (c) 2026 by Rockchip Electronics Co., Ltd. All rights reserved.`) and are **not** stored in this repository.
- `scripts/install_rknn.sh` downloads the Python `rknn_toolkit_lite2` wheel (for offline diagnostic scripts in `scripts/` and `tools/` only) from GitHub Releases and verifies its SHA-256 checksum; the C++ binary does not depend on Python.

### Neural Network Models (`.rknn`)
- Pre-converted `.rknn` models and calibration image datasets are excluded from Git via [`.gitignore`](.gitignore).
- If you export or redistribute YOLOv5 weights derived from Ultralytics YOLOv5, note that Ultralytics YOLOv5 is dual-licensed under **AGPL-3.0** and a commercial Ultralytics license. Rockchip `rknn_model_zoo` conversion scripts are **Apache-2.0**.
- Pretrained OSNet Re-ID weights (`osnet_x1_0_imagenet`, `osnet_x0_25_market` from `deep-person-reid` / `torchreid`) are licensed under **MIT**.

### Unit Test Target (`light_ai_tests`)
- The host unit-test project ([`tests/CMakeLists.txt`](tests/CMakeLists.txt)) links only against the C++17 standard library and POSIX threads (`pthread`), with zero SDK, FFmpeg, or proprietary dependencies.
