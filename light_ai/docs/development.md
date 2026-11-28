# `light_ai` — Prerequisites, Build, Tests & Tooling

Setup, static-analysis, testing, and tooling guide for [`light_ai`](../README.md). For coding rules and PR review policy, see [`CONTRIBUTING.md`](../CONTRIBUTING.md).

## Prerequisites

### Hardware

- **SoC:** Rockchip RK3588 or RK3588S (`aarch64`: 4× Cortex-A76 + 4× Cortex-A55, 3-core RKNPU2, RGA3/RGA2, MPP, Mali G610).
- **Memory:** `>= 4 GB` RAM (on `>= 8 GB` boards, DMA allocations use `/dev/dma_heap/system-dma32`; see [`docs/architecture.md`](architecture.md#verified-hardware--sdk-constraints)).
- **Storage:** External NVMe or USB SSD for repository checkout, builds, and `clang-tidy` cache (avoids wearing internal eMMC flash). All build outputs, test binaries, tidy caches, logs, and calibration dumps stay inside `build/` or `dataset*/` (both git-ignored); [`scripts/storage_guard.sh`](../scripts/storage_guard.sh) emits a non-fatal warning if build outputs land on `/dev/mmcblk*` or `/`.
- **Video Inputs:** USB V4L2 MJPEG camera and/or HTTP MJPEG streams (up to 4 concurrent sources).
- **Cooling:** Active heatsink/fan cooling recommended for sustained 24/7 multi-core NPU + GPU + H.264 operation.

### OS, Kernel & Soft-RTOS Runtime

- **OS & Init:** Linux `aarch64` (verified on Debian 11 / Linaro ALIP with `5.10.226-preempt`) and `systemd`.
- **Kernel Config (24/7 Soft-RTOS):** `CONFIG_PREEMPT=y`, `CONFIG_NO_HZ_FULL=y`, `CONFIG_RCU_NOCB_CPU=y`, `CONFIG_NVME_HWMON=y`.
- **Kernel `bootargs`:** Clean isolation flags `isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 irqaffinity=0-3` (without `rcupdate.rcu_expedited=1` or `rcu_nocbs=all`).
- **Permissions & Device Nodes:** Passwordless `sudo -n` (for RT thread pinning, `mlockall`, CPU/NPU/DDR governors, and `isolate_rt.sh`) and read/write access to `/dev/video*`, `/dev/dri/renderD*`, `/dev/dma_heap/system-dma32`, `/dev/rknpu`, and `/dev/cpu_dma_latency`.

### Build & Test Dependencies

- **Host-only tests (`./scripts/test.sh`):** `cmake >= 3.10`, GCC 10+ or Clang 11+ (C++17), `pthread`, `libanl` (no Rockchip/FFmpeg/EGL SDKs required).
- **Full RK3588 build (`./scripts/build.sh`):**
  - `pkg-config`, `rockchip_mpp`, `librga`, `egl`, `glesv2`, `libgbm`.
  - Self-built shared `ffmpeg-rockchip` `8.1` installed in `/usr/local/lib/pkgconfig` (`FFMPEG_PKGCONF_DIR` defaults to `/usr/local/lib/pkgconfig` so distro FFmpeg 4.x `so.58` headers never leak in; see [LGPL vs. GPL note](../THIRD_PARTY_LICENSES.md#ffmpeg-lgpl-vs-gpl-important-for-binary-redistribution)):
    ```bash
    cd ~/work/ffmpeg-rockchip
    ./configure --prefix=/usr/local --enable-gpl --enable-version3 --enable-rkmpp --enable-rkrga --enable-libdrm --enable-libx264 --enable-libx265 --enable-libvpx --enable-pic --enable-shared --disable-static
    make -j$(nproc) && sudo make install && sudo ldconfig
    ```
  - RKNPU2 `2.5.0` (`rknn_api.h` and `librknnrt.so`, or override via `-DRKNN_ROOT=/path/to/rknpu2`).
- **Optional (Gate, Benchmarks & Python Tools):**
  - **2-pass `clang-tidy` gate & editor:** system `clang-tidy-11` + `light_ai/venv` (`clang-tidy==23.1.4`, `clangd==22.1.1`). One-time venv setup from `light_ai/`:
    ```bash
    python3 -m venv venv && venv/bin/pip install --no-cache-dir clang-tidy==23.1.4 clangd==22.1.1
    ```
  - **Benchmarks & viewer checks:** `rt-tests` (`cyclictest`), `ffmpeg` / `ffplay`.
  - **Python diagnostics (`scripts/`, `tools/`):** install `rknn-toolkit-lite2==2.5.0` from GitHub Releases with SHA-256 verification via `./scripts/install_rknn.sh`.

## Build & Static Analysis

```bash
./scripts/build.sh
```

> **Note (`isolcpus=4-7`):** Because boot-time `isolcpus=4-7` removes Cortex-A76 cores `4-7` from CFS load balancing until reboot, a plain `./scripts/build.sh` runs on housekeeping Cortex-A55 cores `0-3`. A bare `taskset -c 0-7 ./scripts/build.sh` is still forbidden: it only raises `nproc` without migrating compiler/tidy jobs onto `4-7`. To build on the A76 cores use `BUILD_ON_A76=1` (below). If `isolate_rt.sh on` was run manually earlier, verify `./scripts/isolate_rt.sh status` (`user.slice`) before long builds.

#### Building on the A76 cores (`BUILD_ON_A76=1`)

```bash
BUILD_ON_A76=1 ./scripts/build.sh
```

- **When:** on the RK3588 board, with `light_ai` stopped and boot-time `isolcpus=4-7` active.
- **How:** [`scripts/a76_slot.sh`](../scripts/a76_slot.sh) gives every compile and `clang-tidy` job one of 4 `flock` slots (`build/.a76_slot_{4..7}`) and pins it to that core: one core per job, because the scheduler does not balance isolated cores. Builds run with `-j4` (a 4-job ceiling for `3.8 GB` RAM); tidy defaults to `min(4, MemAvailable / 400 MB)` jobs, and the wait for a slot is not part of the per-TU timeout. `scripts/test.sh` and `scripts/tidy_run.sh` accept the same variable. CMake itself, the link steps and the test binaries are not pinned (one core would weaken ThreadSanitizer).
- **Compile database:** the launcher is passed as `-DCMAKE_RULE_LAUNCH_COMPILE=...`. CMake reads no variable of that name, so both `CMakeLists.txt` files forward it to the global property `RULE_LAUNCH_COMPILE`. The property stays out of `compile_commands.json`, so `clang-tidy` and the tidy cache see the same commands in both modes. Do not use `CMAKE_CXX_COMPILER_LAUNCHER`, which does enter the database. Without `BUILD_ON_A76=1` the scripts pass an empty value, which clears the cached one.
- **Preflight (`a76_slot.sh --check`):** fails if a `light_ai` process or a `light_ai*.service` unit is active, if that cannot be determined, or if the cpuset blocks a core of `4-7` (`sudo -n ./scripts/isolate_rt.sh off`). It runs when a script starts, not per job: do not start `light_ai` during such a build.
- **Cached launcher:** the CMake cache keeps the launcher until the next run of the scripts. Without `BUILD_ON_A76=1` in the environment `a76_slot.sh` runs the command unpinned, so a manual `cmake --build`, `make` or IDE build does not reach cores `4-7` without a preflight.

`scripts/build.sh` runs three gates in sequence and fails on the first error:
1. **Release CMake build** (`build/light_ai`, C++17, `-O3 -Wall -Wextra`).
2. **Host unit tests + ThreadSanitizer** (`scripts/test.sh`; `light_ai_tests` builds at `-O2`, the TSan binary at `-O1 -g`; skip tests with `SKIP_TESTS=1`, skip only TSan with `SKIP_TSAN=1`).
3. **Two-pass `clang-tidy` gate** (`scripts/tidy_run.sh`; skip with `SKIP_TIDY=1`).

### CMake Options & CPU Flags

- **CPU tuning (`TARGET_CPU_FLAGS` in [`CMakeLists.txt`](../CMakeLists.txt)):**
  - GCC default: `-mcpu=cortex-a76+dotprod+crypto -mtune=cortex-a76.cortex-a55`
  - Clang default: `-mcpu=cortex-a76+crypto+dotprod` (Clang rejects GCC's dotted `-mtune` syntax)
  - Override via `cmake -DTARGET_CPU_FLAGS="..." ..`.
- **FFmpeg (`FFMPEG_PKGCONF_DIR`):** Defaults to `/usr/local/lib/pkgconfig` (`ffmpeg-rockchip` 8.1, `libavcodec.so.62`).
- **RKNN SDK (`RKNN_ROOT`):** Locate `rknn_api.h` and `librknnrt.so` via `-DRKNN_ROOT=/path/to/rknpu2` or standard `/usr/local`, `/usr` paths.

### `clang-tidy` Gate Operations (`scripts/tidy_run.sh`)

Rule definitions, function-size limits, `rules_lint.sh` checks, `tests/tidy_probe/` self-tests, and NOLINT allowlist policy (`scripts/rules_allow.txt`) are documented canonically in [`CONTRIBUTING.md`](../CONTRIBUTING.md#pre-pr-verification-gate). Operational details:

- **Logs & hangs:** `build/tidy/{src,tests}/<path>.log`, `build/tidy/rules/{src,tests}/<path>.log`, `build/tidy/selftest/` (self-test and lint), `build/tidy/HANGS.txt`.
- **Cache & parallelism:** Result cache in `build/tidy-cache` (`scripts/tidy_cache.py`; disable with `TIDY_NO_CACHE=1`), parallel jobs via `TIDY_JOBS=N` (defaults to `min(nproc, MemAvailable / 400 MB)`; `4` instead of `nproc` with `BUILD_ON_A76=1`).
- **Per-run memo (`build/tidy/pp-cache`):** emptied at the start of every run, under the run lock. The `main` pass stores each TU's preprocessor digest and the `rules` pass reuses it instead of preprocessing again; the digest comes with the `stat` stamps of the files it read, so a file edited between the passes is preprocessed anew (a header created or removed between the passes, changing how an include resolves, is not seen). The `--version` outputs are taken once per run (compiler) and once per pass (`clang-tidy`).
- **Subset run (`TIDY_FILES`):** `TIDY_FILES="src/a.cpp tests/b.cpp" bash scripts/tidy_run.sh` runs the self-test, lint, and both passes on only those translation units (`src/**/*.cpp`, `tests/*.cpp`; a header or missing file exits with `2`), updating only their logs. Quick check while editing only — `scripts/build.sh` always runs every TU.
- **Rules pass & config verification:** The `rules` pass invokes `venv/bin/clang-tidy` (`23.1.4`) with `--experimental-custom-checks` on `scripts/tidy_rules.yaml`, and `clang-tidy --verify-config` runs on `scripts/tidy_rules.yaml` (verified against [`tests/tidy_probe/verify_typo.yaml`](../tests/tidy_probe/verify_typo.yaml)) so mistyped option keys cannot silently pass.

### Editor Diagnostics (`clangd`)

[`.clangd`](../.clangd) configures `clangd` 22.1.1 from `light_ai/venv`: it reads both compile DBs (`build/`, `build/tests/`), strips `-mtune`/`-mcpu`, disables the background index (RAM), enables `readability-function-cognitive-complexity` (`Threshold: 20`) and `misc-const-correctness` (`FastCheckFilter: None`), and disables LLVM 12+ checks absent from `clang-tidy-11` (`custom-*` rules do not run in `clangd`; use `TIDY_FILES`). Quick CLI check on one file: `venv/bin/clangd --check=src/a.cpp`. Editor diagnostics are hints only (e.g. LLVM 22 counts constructor initializers as statements, so `Pipeline::Pipeline` reports `67 > 60` in the editor only).

## Tests

```bash
./scripts/test.sh
```

[`tests/CMakeLists.txt`](../tests/CMakeLists.txt) is an independent host CMake project requiring only a C++17 compiler and `pthread` (zero Rockchip/FFmpeg/EGL dependencies — runs identically on the board and on x86_64/arm64 hosts).

- **Unit binary (`build/tests/light_ai_tests`):** **72 suites, 10,378 checks**.
- **ThreadSanitizer binary (`build/tests/light_ai_tests_tsan`, `-fsanitize=thread`):** **11 threaded suites, 6,209 checks** (`source_threads`, `source_prober_threads`, `preview_ring_threads`, `packet_queue_threads`, `session_gate_threads`, `reid_worker_tsan`, `pi_mutex`, `async_log_sink_threads`, `infer_slot_threads`, `stream_output_threads`, `tcp_pull_threads`).
- **Custom runner options ([`tests/suite_runner.hpp`](../tests/suite_runner.hpp)):**
  ```bash
  ./build/tests/light_ai_tests [substr...] [--repeat N]
  ./build/tests/light_ai_tests_tsan --repeat 200 packet_queue_threads
  ```
  Exits `0` on pass, `1` on check failure, `2` on invalid runner argument.

### Covered Test Suites (72 Total in `tests/test_main.cpp`)

- **Core & Containers:** `check_harness`, `pod_containers`, `geometry`, `fp16`, `box_math`, `rga_limits`, `fixed_fmt`, `status`, `stop_flag`, `joining_thread`, `non_copyable`, `log`, `async_log_sink_threads` *(TSan)*, `timings`, `perf_clock`, `perf_reporter`
- **Detection, Tracking, Labels, Re-ID & Reference:** `yolo_decoder`, `yolo_head_shape`, `box_filter`, `box_smoother`, `kalman_axis`, `associator`, `auto_orient`, `trail_window`, `tracker`, `glyph_font`, `callout_layout`, `overlay_raster`, `box_mapper`, `global_track`, `reid_worker_tsan` *(TSan)*, `reference_chain` (Layer B golden check; see [`tests/data/README`](../tests/data/README))
- **Config, CLI, Signals, Affinity & Platform:** `config`, `signals`, `cpu_affinity`, `pi_mutex` *(TSan)*, `sd_watchdog`, `console_reporter`, `cal_dumper`
- **Inference Pool & Output:** `inference_pool`, `infer_slot_threads` *(TSan)*, `stream_output_threads` *(TSan)*
- **Sources & HTTP MJPEG:** `jpeg_probe`, `byte_window`, `http_url`, `http_head`, `multipart_reader`, `line_scan`, `buffered_socket`, `mjpeg_slot_pool`, `net_mjpeg_source`, `usb_node_locator`, `source_channel`, `source_threads` *(TSan)*, `source_set`, `source_prober`, `source_prober_threads` *(TSan)*
- **Encoder, Session & Transport:** `tcp_retry`, `tcp_send`, `tcp_pull_threads` *(TSan)*, `encoder_config`, `param_set_cache`, `pts_clock`, `encode_pacer`, `session_warmup`, `idle_plate`, `preview_ring`, `preview_ring_threads` *(TSan)*, `packet_queue`, `packet_queue_threads` *(TSan)*, `session_gate`, `session_gate_threads` *(TSan)*

**Not unit-tested (verified by live hardware runs & Layer A replay):** V4L2 capture, MPP MJPEG decode, RGA3 resize/compose, RKNN NPU execution, Mali EGL/GLES2 drawing, and `h264_rkmpp` encoding.

## Scripts & Tools

- **Build, test & system (`scripts/`):**
  - [`scripts/build.sh`](../scripts/build.sh) — full release build + unit/TSan tests + 2-pass clang-tidy gate.
  - [`scripts/test.sh`](../scripts/test.sh) — host-side `light_ai_tests` + `light_ai_tests_tsan`.
  - [`scripts/a76_slot.sh`](../scripts/a76_slot.sh) — one build job per Cortex-A76 core for `BUILD_ON_A76=1` (preflight + 4 `flock` slots).
  - [`scripts/tidy_run.sh`](../scripts/tidy_run.sh), [`scripts/tidy_cache.py`](../scripts/tidy_cache.py), [`scripts/tidy_rules.yaml`](../scripts/tidy_rules.yaml), [`scripts/rules_lint.sh`](../scripts/rules_lint.sh), [`scripts/rules_allow.txt`](../scripts/rules_allow.txt) — 2-pass `clang-tidy` runner & cache, project-rule checks, lint, and allowlist (self-test fixtures: `tests/tidy_probe/`).
  - [`scripts/storage_guard.sh`](../scripts/storage_guard.sh) — warns (non-fatally) if build output targets `/` or internal eMMC (`/dev/mmcblk*`) to help protect flash endurance on eMMC boards.
  - [`scripts/install_rknn.sh`](../scripts/install_rknn.sh) — downloads & verifies `rknn_toolkit_lite2==2.5.0` wheel.
  - [`scripts/run.sh`](../scripts/run.sh), [`scripts/light_ai.ini.example`](../scripts/light_ai.ini.example), [`scripts/light_ai.service.example`](../scripts/light_ai.service.example) — wrapper launcher (loads `build/light_ai.ini` by default), INI profile template, and hardened `systemd` unit template.
  - [`scripts/isolate_rt.sh`](../scripts/isolate_rt.sh), [`scripts/run_bench.sh`](../scripts/run_bench.sh) — IRQ affinity isolation (`CPU 0–3` A55) and isolated `cyclictest` + loopback latency benchmark.
  - [`scripts/set_governors.sh`](../scripts/set_governors.sh) — pins CPU/NPU/DDR governors to `performance` (`sudo`).
  - [`scripts/usb_replug.sh`](../scripts/usb_replug.sh) — sysfs USB camera power-cycle helper (installed at `/usr/local/sbin/light-usb-replug`).
  - [`scripts/collect_state.sh`](../scripts/collect_state.sh), [`scripts/collect_project.sh`](../scripts/collect_project.sh) — collects board state / source snapshots under `build/`.
  - [`scripts/probe_yolo.py`](../scripts/probe_yolo.py), [`scripts/test_inference.py`](../scripts/test_inference.py), [`scripts/diag_zero_out.py`](../scripts/diag_zero_out.py), [`scripts/compare_night.py`](../scripts/compare_night.py), [`scripts/split_mjpeg.py`](../scripts/split_mjpeg.py) — RKNNLite Python diagnostics and offline MJPEG comparison helpers (run with `venv/bin/python`).
- **Replay, verification & benchmarks (`tools/`):**
  - [`tools/ref_replay.sh`](../tools/ref_replay.sh) + [`tools/mjpeg_replay.py`](../tools/mjpeg_replay.py) — Layer A drop-free MJPEG regression replay (`reference/ref_evening.csv`).
  - [`tools/oracle_check.py`](../tools/oracle_check.py) — C++ `--box-log` vs. Python RKNNLite parity verifier over `--cal` PPM dumps.
  - [`tools/ab_run.py`](../tools/ab_run.py), [`tools/compare.py`](../tools/compare.py), [`tools/list_missed.py`](../tools/list_missed.py) — offline model A/B comparison scripts.
  - [`tools/bench_npu.c`](../tools/bench_npu.c), [`tools/bench_npu_pool.cpp`](../tools/bench_npu_pool.cpp) — pure NPU throughput microbenchmarks.
