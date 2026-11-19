# light_ai -- working notes for Claude Code

Claude Code runs ON the dev board (CoolPi RK3588, Debian 11, aarch64): build, unit tests,
clang-tidy and live runs are local commands. For full architecture, CLI reference, and coding
rules see [README.md](README.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## Where things are
- Work directory: `/mnt/nvme_data/linaro/Projects/light/light_ai` (use the physical NVMe path;
  `~/Projects/light` is only a symlink from eMMC `/userdata/home/linaro` to `/mnt/nvme_data/linaro/Projects/light`).
  The git root is `/mnt/nvme_data/linaro/Projects/light`: the repo also holds `light_control/` and
  `light_sensor/` -- other projects, strictly out of scope: never read for the task, edit, stage,
  or commit anything outside `light_ai/`.
- User-owned local dirs (never delete or rewrite): `dataset*/`, `venv/`, `artifacts/`, `build-tidy/`.
- Everything the project generates stays inside `light_ai/build/` (git-ignored): CMake tree,
  test builds, tidy logs and cache, run logs, CSVs, frames, baselines. Never `/tmp` or home (eMMC
  wears out). Claude Code's own `$TMPDIR` (`/mnt/nvme_data/claude/tmp`, NVMe) is for session scratch only.
  `build.sh` / `tidy_run.sh` warn when their output is on eMMC (`scripts/storage_guard.sh`).
- Models (`*.rknn`) are not in git: they live in `light_ai/misc/` (default: `./misc/yolov5s_relu_int8_mmse_v5.rknn`).

## Workflow
- Tasks arrive as GitHub issues ("Выполни issue #N"): read the issue, post the plan as a comment
  on it (`gh issue comment N --body-file ...`), wait for the user's OK there, then code.
- Shared working copy: run `git status` before any checkout or merge. Uncommitted changes
  under `light_ai/` -> stop and ask the user; changes elsewhere are the user's -- leave them,
  and if git refuses the checkout because of them, ask. Never stash, reset or discard.
- Stage explicit paths inside `light_ai/` (paths relative to this directory: `git add src/x.cpp`),
  never `git add -A` / `git add .` from the git root / `git commit -a`.
- Base branch: `master` (`fix/audit-p0` merged in PR #73, `fix/audit-p1` merged in PR #81). One branch per task/stage; PR into `master`
  with `gh pr create --base master`. The user reviews and merges.
- Never push to `master` directly, never force-push, never rewrite pushed history.
- Before a PR, the full gate must be green on this board:
      ./scripts/build.sh > build/gate.log 2>&1; echo rc=$?; tail -n 40 build/gate.log
  (CMake build + 61 unit suites + 6 TSan suites + 2-pass clang-tidy gate; `SKIP_TESTS=1` /
  `SKIP_TSAN=1` / `SKIP_TIDY=1` to narrow intermediate iterations).
  Two-pass tidy gate: self-test + `scripts/rules_lint.sh` first, then `main` = system `clang-tidy-11`
  (`.clang-tidy`, `tests/.clang-tidy`) and `rules` = pinned `clang-tidy==23.1.4` in `venv/`
  (`scripts/tidy_rules.yaml`: cognitive complexity 20 + the `custom-*` project rules; `cognitive` before #93).
  Per-file logs: `build/tidy/{src,tests}/<path>.log` and `build/tidy/rules/{src,tests}/<path>.log`;
  self-test and lint: `build/tidy/selftest/`.
  Result cache: `build/tidy-cache` (`TIDY_NO_CACHE=1` disables it). See [README.md](README.md#build--static-analysis).
- Quick tidy on changed files (#93): the main session may run it itself on 1-3 changed TUs and print only
  the findings, never the full output:
      TIDY_FILES="src/a.cpp tests/b.cpp" bash scripts/tidy_run.sh > build/tidy-files.log 2>&1; echo rc=$?
      grep -E ': error: |^tidy: (PASS|FAIL|bad)' build/tidy-files.log
  Self-test, lint, both passes, the same cache and logs, only those TUs (`.cpp` only; a header -> a TU that includes it;
  exit 2 on a bad entry). It is not the gate: the full gate and live runs stay with `board-check` before
  every push, and the per-commit `commit-reviewer` review does not change.
- Editor diagnostics: `clangd 22.1.1` in `venv/` with the tracked `.clangd` (both compile DBs, cognitive
  complexity 20, no background index). The `clangd-lsp` plugin starts `clangd` from `PATH`, so a user-level
  link points at the venv: `~/.local/bin/clangd -> /mnt/nvme_data/linaro/Projects/light/light_ai/venv/bin/clangd`
  (remove: `rm ~/.local/bin/clangd`, then clangd 11 from `/usr/bin` is used again; a rebuilt venv without
  `clangd` leaves the link dangling -> reinstall: `venv/bin/pip install --no-cache-dir clangd==22.1.1`).
  CLI check of one file: `venv/bin/clangd --check=src/a.cpp 2>&1 | grep -E '^E\[[^]]*\] \['` (diagnostics only;
  the exit code is not a verdict: `--check` also fails on refactoring tweaks it cannot apply).
  The diagnostics are hints only; the gate stays the authority.
- Unit tests alone: `bash scripts/test.sh` (host-side, no SDK; 61 suites / 9,012 checks + 6 TSan suites / 23 checks).
  Single suite / flake hunt: `./build/tests/light_ai_tests [substr...] [--repeat N]` (also `light_ai_tests_tsan`;
  exit 2 on bad args).
- Live run:
      sudo -n timeout -s INT 90 ./build/light_ai --device /dev/video20 --pool 3 --frames 600
  `timeout` goes INSIDE `sudo`: `timeout ... sudo ./build/light_ai` does not deliver SIGINT -- `sudo`
  does not relay a signal sent from its own process group. To exercise SIGINT, make the timeout shorter
  than the frame budget (e.g. `15` s); exit 124 = stopped by timer (normal).
  Redirect to `build/run.log` and read with `tail`/`grep`.
  After every `sudo` run that writes files (`build/cal.ppm`, `build/<cal-dir>/`, `dataset/`, logs),
  restore ownership: `sudo -n chown -R linaro: build dataset`.
- Permissions: `.claude/settings.json` (tracked) pre-approves gate, test, run and git/gh commands
  and denies edits in `light_control/` and `light_sensor/`, history rewrites, `git add -A` / `commit -a`,
  and `git stash`. Personal overrides: `.claude/settings.local.json`.
- USB camera tests (unplug / replug, hot attach, start without a camera): NEVER ask the user
  to pull the connector. Use logical sysfs disconnect:
      sudo -n /usr/local/sbin/light-usb-replug list                       # nodes and busids
      sudo -n /usr/local/sbin/light-usb-replug cycle /dev/video20 5       # off 5 s, then on
      sudo -n /usr/local/sbin/light-usb-replug off /dev/video20           # prints the busid
      sudo -n /usr/local/sbin/light-usb-replug on <busid>
  After a replug cycle the camera may return as `/dev/video21` -- check `light-usb-replug list`.
  It is a root-owned copy of `scripts/usb_replug.sh`; after changing the script, ask the user to reinstall it.
- Logs are long: read tails and grep for errors, not whole files.
- Only one heavy job at a time (3.8 GB RAM): no build while a live run is measuring FPS.

## Model and delegation
- Main session owns architecture, threading, `dma-buf`/ownership decisions, non-trivial edits,
  commits, and PRs.
- Subagents (`.claude/agents/`):
  - `board-check`: every build / unit-test / TSan / tidy gate and every live run. Reports verdicts,
    first errors, and FPS/TOTAL figures; never edits code or touches git.
  - `gate-fixer`: mechanical gate failures (includes, naming, const, casts) with an obvious local fix.
    Anything touching logic, threads, or interfaces stays in the main session.
- Branch switching for A/B runs stays in the main session: check out, then ask `board-check` to measure;
  keep baseline logs (`build/run_*.log`) to avoid re-measuring.

## Project rules (full text: [CONTRIBUTING.md](CONTRIBUTING.md))
(gate) = enforced by `scripts/tidy_run.sh` (`custom-*` checks of the `rules` pass, `scripts/rules_lint.sh`, both
self-tested on `tests/tidy_probe/`): nobody re-checks it by hand. Fix a (gate) finding like any other gate finding;
a NOLINT that switches a `custom-*` check off (by name or glob: `*`, `cust*`; every NOLINT of a line counts) only
with `: <reason>` and a line in `scripts/rules_allow.txt` (a CONTRIBUTING-listed site), decided by the main session.
(review) = only `commit-reviewer` can judge it.
- C++17 (review). No virtuals or inheritance hierarchies (gate); templates for static polymorphism.
- No manual heap (`new`/`delete`, `malloc` family, `unique_ptr`/`shared_ptr`/`weak_ptr`), no statics of any
  kind, no globals, no `const_cast` (gate). Frame path: no allocation; preallocated members cleared at entry
  by the callee (review).
- Pixels exist only as `dma-buf`s: no CPU pixel copies (review).
- Dependencies injected by reference to a concrete class (`StopFlag&`, `const Log&`) (review).
- Diagnostics through `Log` (`include/core/log.hpp`); only `main.cpp` owns `std::cout`/`std::cerr`/`std::clog`;
  no C stdio output (gate).
- Header layout: in each visibility section, functions first, then a fresh label for data (review).
- Header extensions: new headers use `.hpp`; nine legacy `.h` headers stay as-is unless refactored (gate).
- `core/` includes only the C++17 standard library and `pod_list.hpp`/`pod_ring.hpp` (keeps tests SDK-free)
  (gate); include order: system and SDK headers first (gate); no GStreamer / OpenCV (gate).
- Exception (review): header-only libraries in `externals/` (`stb_easy_font.h`) may keep file
  statics; include them only from `.cpp` files, never from a project header.
- Errors (`Status` / `noexcept`): recoverable failures return `Status` (`include/core/status.hpp`);
  exceptions (`std::runtime_error` via `throw_on`) are startup-only; frame path and thread bodies are
  `noexcept` (no `throw`: review; no `catch (...)` outside the three allowlisted sites of CONTRIBUTING: gate).
  Exit codes (review): `0` clean, `1` runtime fatal, `2` bad CLI/config, `3` startup failure.
- Output side vs recognition (review): no viewer -> output side sleeps completely (`enc=0.0`); recognition never
  sleeps; with viewer, online sources run full path and absent sources show `NO SIGNAL`.
- Comments match the code and docs (review); max line length `140` (`.clang-format`; gate).

## Completed refactoring stages (0–7, `fix/audit-p0` @ `3ccd8ae`, merged in PR #73)
Full milestone history and A/B records live in [docs/roadmap.md](docs/roadmap.md) and [docs/state.md](docs/state.md).

- [x] **0 — Safety net (PR #6, #32):** `core/` domain types, host unit tests, Layer A (`misc/ref_evening.mjpeg` + `reference/ref_evening.csv`), Layer B (`reference_chain`).
- [x] **1 — Global state (PRs #7–#9, #51):** `ProcessEnv`, `SignalWaiter`, grouped `Config`, `Log` injection across all modules. *(Closed: `--log-level` CLI/INI flag added.)*
- [x] **2 — `InferencePool` (PR #10):** `InferSlot`, worker threads, ordered FIFO, core masks, `OverlaySink` taking `FrameView`.
- [x] **3 — Sources (#15, PRs #16–#17):** `SourceChannel` state machine, `UsbNodeLocator`, `SourceProber`, `SourceSet`, `Pipeline::run()` simplified.
- [x] **4 — `NetEncoder` (#19, PRs #20–#22, #23, #24):** decomposed into `EncoderConfig`, `StreamLayout`, `ParamSetCache`, `PtsClock`, `EncodePacer`, `BitmapFont`, `IdlePlate`, `EncoderFramePool`, `FrameComposer`, `PacketQueue`, `SessionGate`, `MuxSession`, `H264Encoder`, `NetworkInit`, `SessionWarmup` (`= default` destructor).
- [x] **5 — `NetMjpegSource` (#28, PRs #29–#30):** `ByteWindow`, `HttpUrl`, `HttpHead`, `MultipartReader`, `BufferedSocket`, `MjpegSlotPool`, loopback tests. *(Closed: `HttpHead` trims trailing horizontal whitespace on unquoted boundaries.)*
- [x] **6 — Complexity & 2-pass tidy (#34, #39, #42, #43, #48, #51, #55, #37):** `YoloDecoder`, `KalmanAxis`, `Associator`, `TrailWindow`, `BoxMapper`, `ConsoleReporter` + `fixed_fmt.hpp`, 2-pass `clang-tidy-11` + `clang-tidy-23` (`Threshold: 20`), NVMe checkout enforcement.
- [x] **7 — Unified errors & `noexcept` (#61, PRs #62–#71):** `Status` / `Errc` / `ErrnoText`, exit codes `0..3`, `retry_listen()` busy-port recovery, startup 3-head `yolo_head_shape` check, `noexcept` frame path and thread bodies (`49` unit suites / `7,307` checks + `4` TSan suites / `19` checks).

## Completed hardening & Re-ID stages (1–6, `fix/audit-p1`, `3411069`..`8c6b879`)
- [x] **Stage 1 (PR #74):** `bitmap_font` ASCII bounds guard, `RknnEngine` NHWC/NCHW dim/size/lifecycle safety, `YoloDecoder`/`Associator` NaN/cap guards, expired `TRACK_TENT` directly to `TRACK_FREE`.
- [x] **Stage 2 (PR #75):** `StopFlag` wakeup in `NetMjpegSource` / `BufferedSocket`, `TcpPullServer::abort_client()` teardown (`~4.2 s` vs `19 s`), epoch-guarded `SessionGate`, `kDecErrMax` recovery, `EINTR`/`bytesused==0` V4L2 fixes.
- [x] **Stage 3 (PR #76):** `H264Encoder::drain()` `LOW_DELAY` + `AVDictionary` CBR/IDR, `SessionPtsFilter` reconnect PTS rebase, real `stride_bytes` (`TensorRef`), targeted `DmaSync`, grow-only `MppDecoder::pkt_buf_`.
- [x] **Stage 4 (PR #77):** `usb_cap_capture_streaming` (`device_caps`), `Tracker` `kMaxMiss = 6` + round-2 `anchor_clipped` + `kHpDecayStreak`, `YoloDecoder` band recall + `Box::seq` NMS, `BoxSmoother` skips `cls == 0`, Layer A/B re-baseline.
- [x] **Stage 5 (PR #78):** `GlyphFont` (8x8 ASCII+Cyrillic `128x96` `GL_ALPHA` atlas, free functions), `compute_callouts` non-overlapping EN/RU labels (`--label-lang`), `GpuBoxDrawer` batching + CPU parity.
- [x] **Stage 6 (`8c6b879`, PR #79):** `GlobalTrackTable` (`<= 32` global IDs, `45 s` TTL), `BasicReidQueue` + `ReidWorker` (4 slots, `FP32`/`FP16`/`INT8`/`UINT8`), `RgaResizer::crop_resize` (`RGA2` RGB888) + `--src-rot 0|90|180|270` (`RGA3`), `--reid-model`, `--reid-thr` (`0.62`), `4` sources (`2x2` grid), `54` unit suites / `7,481` checks + `5` TSan suites / `22` checks (plus post-merge `AutoOrient`, default `--reid-model`, and `CalloutHistory` hysteresis in PR #80).
