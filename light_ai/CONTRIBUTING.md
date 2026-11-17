# Contributing to `light_ai`

This guide documents the project's coding rules, error model, comment style, and verification gates. Keep changes strictly scoped to `light_ai/` and target the `fix/audit-p0` branch via pull requests.

## Coding Rules

- **No GStreamer, no OpenCV.**
- **Zero-copy frame path:** Frames exist only as `dma-buf` descriptors; CPU pixel copies are forbidden. The only permitted copies are compressed MJPEG input packets (~25 KB) into the decoder packet buffer and compressed H.264 packets out of the encoder.
- **I/O & logging:** Use `iostream` only (`printf`/`sprintf`/`snprintf` are forbidden). All diagnostics go through `Log` ([`include/core/log.hpp`](include/core/log.hpp)) injected as `const Log&`. Only [`src/main.cpp`](src/main.cpp) owns `Log(std::cout, std::cerr)` and `print_usage(std::cout)`.
- **Include order:** System and SDK headers first (`<...>`), project headers last (`"..."`). Reference: [`src/gpu_box_drawer.cpp`](src/gpu_box_drawer.cpp).
- **Header layout:** Within each visibility section (`public:` / `private:`), member functions and data members live in separate blocks — functions first, then a fresh visibility label for data. Reference: [`include/camera_v4l2.hpp`](include/camera_v4l2.hpp).
- **Header extensions:** New C++ headers use `.hpp`. Nine legacy headers still use `.h` (`box_mapper.h`, `box_smoother.h`, `coco_names.h`, `console_reporter.h`, `frame_trace.h`, `net_encoder.h`, `rga_resizer.h`, `config/config_file.h`, `config/option_registry.h`); keep existing names unchanged unless refactoring the module.
- **No manual heap allocation:** No `new`/`delete`, no `std::unique_ptr`/`std::shared_ptr`, no heap allocations in recovery or frame paths. Data lives in fixed-size members (`std::vector`/`std::string` are permitted only as startup RAII containers). Reference: [`include/pod_list.hpp`](include/pod_list.hpp).
- **No statics:** No static data members, no static member functions, no function-local statics. Shared state belongs to the owning layer as a regular member; file-local helpers belong in an anonymous namespace inside the `.cpp`. Compile-time class constants use an unscoped `enum`.
  - *Exceptions:* disposable probes in `tools/` and vendored header-only libraries in `externals/` (`stb_easy_font.h`, `font8x8_basic.h`), which must only be included from `.cpp` files (e.g. [`src/core/bitmap_font.cpp`](src/core/bitmap_font.cpp)), never from a project header.
- **No virtuals or inheritance:** Zero vtables and zero dynamic polymorphism. Layer boundaries use composition of concrete members; static polymorphism uses templates (e.g. [`PodList<T, N>`](include/pod_list.hpp), [`PodRing<T, N>`](include/pod_ring.hpp), [`InferencePool`](include/inference_pool.hpp)).
- **`const` by default:** Non-mutating methods are `const`; read-only parameters are passed by `const&`; hot-path outputs go through out-parameters (e.g. `YoloDecoder::decode` fills `DetList& out`).
- **Branch tables over `if`-chains:** Dispatch over fixed sets via `switch` or `constexpr` tables (reference: `BoxMapper::det_color` in [`src/box_mapper.cpp`](src/box_mapper.cpp)).
- **Preallocated buffer contract:** Cross-frame output parameters are never assumed empty — the callee clears them at entry (e.g. `out.clear()` in `YoloDecoder::decode`).
- **SDK-free `core/`:** [`include/core/`](include/core/) and [`src/core/`](src/core/) may include only the C++17 standard library and `pod_list.hpp`/`pod_ring.hpp`. This keeps host unit tests free of MPP/RGA/RKNN/EGL/FFmpeg dependencies.

## Error & Exception Model

- **`Status` ([`include/core/status.hpp`](include/core/status.hpp)):** Operations that can fail for a runtime reason return `Status` (`Errc`, string-literal `what`, optional `Sys::Errno` / `Sys::Sdk` code); results go to out-parameters. Outcome enums (e.g. `FrameStatus`, `SourceChannel::Attach`, `HttpHead::Parse`) carry `Status` in their failure arm.
- **Startup-only exceptions:** `std::runtime_error` (via [`throw_on`](include/throw_on.hpp) when wrapping a `Status`) is permitted only on the startup path (constructors and `Pipeline` initialization).
- **`noexcept` frame path & threads:** Frame processing and thread bodies are `noexcept` — no exceptions thrown and no `catch (...)` in the frame path. Only two `catch (...)` exist for startup thread-launch rethrows (`InferencePool::start_workers`, `NetPreview` ctor), and `MediaSource` catches `std::bad_variant_access` solely on startup `emplace`.

## Output Side vs. Recognition Invariant

1. **No viewer connected:** The output side sleeps completely (no box drawing, no RGA compose, no `NO SIGNAL` frames, no H.264 encode; `enc` reads `0.0`).
2. **Recognition never sleeps:** Capture, decode, NPU inference, and tracking always run regardless of viewer state.
3. **With a viewer connected:** Online sources take the full draw/compose/encode path; absent sources display the pre-rendered gray `NO SIGNAL` tile.

## Comment Style & Formatting

- Max line length: **140 characters** (enforced by [`.clang-format`](.clang-format)).
- Structured comments (header line ending with `:`, one item per line, indented continuations) instead of long prose.
- Do not repeat rules from this file inside code comments; reference local invariants or verified behavior only.

## Pre-PR Verification Gate

Every PR targeting `fix/audit-p0` must pass the full board gate:

```bash
./scripts/build.sh > build/gate.log 2>&1; echo rc=$?; tail -n 40 build/gate.log
```

This runs:
1. Release CMake build (`light_ai`).
2. Host unit tests (`light_ai_tests` — 49 suites, 7,307 checks) + ThreadSanitizer suites (`light_ai_tests_tsan` — 4 suites, 19 checks).
3. Two-pass `clang-tidy` gate ([`scripts/tidy_run.sh`](scripts/tidy_run.sh)):
   - **main:** system `clang-tidy-11` against [`.clang-tidy`](.clang-tidy) (`tests/` uses [`tests/.clang-tidy`](tests/.clang-tidy)).
   - **cognitive:** `venv/bin/clang-tidy` (`23.1.4`) against [`scripts/tidy_cognitive.yaml`](scripts/tidy_cognitive.yaml) (`Threshold: 20`).

For pure-logic or multi-threaded changes, also verify Layer A reference parity (`bash tools/ref_replay.sh misc/ref_evening.mjpeg build/ref_p3.csv 3 reference/ref_evening.csv`) and live hardware execution on the board.
