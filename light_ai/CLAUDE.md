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

## Board SDK (self-built libraries, forks)
Checked 2026-11-20. Sources: `/mnt/nvme_data/work` (`~/work` links there): user-owned, read only -- never build,
install, rebase, push or clean there without the user's decision (a new `/usr/local` install changes every later gate
and FPS figure). The distro packages stay installed: `librockchip-mpp1` 1.5.0 and `librga2` 2.2.0 (dpkg versions;
their `.pc` files say 1.3.8 / 2.1.0), FFmpeg 4.3 (`so.58`).
- FFmpeg `ffmpeg-rockchip/`: our fork (`myfork` = `xdcsystems/ffmpeg-rockchip`, `origin` =
  `nyanmisaka/ffmpeg-rockchip`). Installed build `129a3cf0d7` (`ffmpeg -version`) = `origin/8.1` (`b187bf5276`, FFmpeg
  8.1.3) + 2 own `h264_rkmpp` EOS fixes (`f5f6da6337`, `129a3cf0d7`); `master` = `myfork/master` = `10b361b5d4`, which
  only adds `STATE-NOTES.md`. `/usr/local`, `libavcodec` 62.28.103, configured with `--prefix=/usr/local --enable-gpl
  --enable-version3 --enable-rkmpp --enable-rkrga --enable-libdrm --enable-libx264 --enable-libx265 --enable-libvpx
  --enable-pic --enable-shared --disable-static`. Upstream PR nyanmisaka/ffmpeg-rockchip#273 (open): head
  `xdcsystems:xdc-upstream-sync` = `a9f8736d02` = local `xdc-rkmpp-eos-fixes-v2` (upstream `master` + the 2 fixes;
  that local branch tracks `origin/master`, not the fork); built in-tree only, never installed. The fork's
  `STATE-NOTES.md` is its log but is stale: its "~592 local commits" count against `origin/master`
  (`origin/8.1..master` is 3), `myfork/master` = `129a3cf0d7`, and that the `.102` libs were removed (7 `*.so.*.102`
  are still in `/usr/local/lib`). CMake pins `libav*` via `FFMPEG_PKGCONF_DIR` (`/usr/local/lib/pkgconfig`) so the
  distro FFmpeg cannot leak in.
- MPP `mpp/`: `HermanChen/mpp` `develop` @ `20959ec3`, no local commits; Release, shared, prefix `/usr/local` (headers
  in `/usr/local/include/rockchip/`, `rockchip_mpp.pc` 1.3.10, installed 2026-11-03; file list in
  `mpp/install_manifest.txt`). `CMakeLists.txt` also adds `-I/usr/include/rockchip` (the distro 1.5.0 headers):
  harmless while every include is `<rockchip/...>` (resolved in `/usr/local/include`), but a bare
  `#include <rk_mpi.h>` would pick the distro header.
- RGA `librga/`: `airockchip/librga` 1.10.6_[3] (`2b32edc`). The vendor's prebuilt `libs/Linux/gcc-aarch64/librga.so`
  is copied to `/usr/local/lib` (`librga.so.2` links to it; hand-written `librga.pc`, `-I/usr/local/include`), and its
  headers are installed flat in `/usr/local/include/` (`im2d*.h`, `rga.h`, `drmrga.h`, `RgaApi.h`, ...; identical to
  `librga/include`, no `rga/` subdir). Headers and library match (1.10.6). The distro `/usr/include/rga/` (2.2.0,
  different `im2d_version.h` / `rga.h`) is reachable through the default `/usr/include`: harmless while RGA is
  included as `<im2d.h>` / `<rga.h>` (resolved in `/usr/local/include`, searched first; today only
  `src/media/rga_resizer.cpp` and `src/net/frame_composer.cpp`), but `#include <rga/im2d.h>` would pick the 2.2.0 headers
  against the 1.10.6 library.
- Include paths: of the pkg-config `MPP` / `RGA` / `AV` sets, only `AV_INCLUDE_DIRS` reaches
  `target_include_directories` (`CMakeLists.txt`; `MPP_INCLUDE_DIRS` / `RGA_INCLUDE_DIRS` are not passed, only their
  libraries are linked). The `/usr/local` headers come from `AV_INCLUDE_DIRS` or the default `/usr/local/include`, so
  the `-I` of the hand-written `librga.pc` plays no part.
- RKNN `rknn_model_zoo/`: V2.5.0 (`837db8e`); `/usr/lib/librknnrt.so` (2.5.0) is a copy of its
  `3rdparty/rknpu2/Linux/aarch64/librknnrt.so`. `rknn_api.h` exists only in that checkout: `CMakeLists.txt` finds it
  through the `HINTS` path `/mnt/nvme_data/work/rknn_model_zoo/3rdparty/rknpu2/include`, so the checkout is a build
  dependency (`-DRKNN_ROOT=` overrides).
- Which MPP/RGA a process loads depends on the loader: `ld.so.conf.d` lists `/lib/aarch64-linux-gnu` before
  `/usr/local/lib`. `light_ai` has `RUNPATH /usr/local/lib`, loads the `/usr/local` copies, and `libavcodec` (no
  RPATH) reuses them. The `/usr/local/bin/ffmpeg` CLI has no RPATH and runs on the distro MPP 1.5.0 / librga 2.2.0: a
  CLI test of the fork needs `LD_LIBRARY_PATH=/usr/local/lib`.
  Check: `ldd build/light_ai | grep -E 'av|mpp|rga|rknn'`.

## Workflow
- Tasks arrive as GitHub issues ("Выполни issue #N"): read the issue, post the plan as a comment
  on it (`gh issue comment N --body-file ...`), wait for the user's OK there, then code.
- Shared working copy: run `git status` before any checkout or merge. Uncommitted changes
  under `light_ai/` -> stop and ask the user; changes elsewhere are the user's -- leave them,
  and if git refuses the checkout because of them, ask. Never stash, reset or discard.
- Stage explicit paths inside `light_ai/` (paths relative to this directory: `git add src/x.cpp`),
  never `git add -A` / `git add .` from the git root / `git commit -a`.
- Base branch: `master` (no audit runs now): one branch per task/stage, created from `master`, PR back into it with
  `gh pr create --base master`. The user reviews and merges. A PR into `master` closes its issue through `Closes #N`.
  While an audit runs, its integration branch is the base instead: branches from it, PRs into it
  (`--base fix/audit-pN`), and the main session closes each issue (`gh issue close N`) after the user confirms the
  merge, because GitHub reads `Closes #N` only in a PR into the default branch. `master` gets the whole audit through
  one PR `fix/audit-pN` -> `master`, which the user merges; after it, the base is `master` again.
  Update this bullet and the next one when an audit branch is merged or a new one starts. Earlier audit branches:
  `fix/audit-p0` reached `master` through PR #5, a direct merge `64f71cb` and finally PR #73; `fix/audit-p1` through
  PR #81; `fix/audit-p2` through PR #114 (`9d63d8d`).
- Never push to `master` (or to an audit's integration branch while one runs) directly, never force-push, never
  rewrite pushed history. Work branches are `fix/*`, `feat/*`, `refactor/*` or `docs/*`: the push rules of
  `.claude/settings.json` allow exactly these. When an audit starts, its integration branch matches `fix/*`, so narrow
  that rule to `fix/audit-pN-*` for the audit's duration (as #106 did for `fix/audit-p2-*`).
- Before a PR, the full gate must be green on this board:
      ./scripts/build.sh > build/gate.log 2>&1; echo rc=$?; tail -n 40 build/gate.log
  (CMake build + 71 unit suites + 11 TSan suites + 2-pass clang-tidy gate; `SKIP_TESTS=1` /
  `SKIP_TSAN=1` / `SKIP_TIDY=1` to narrow intermediate iterations).
  Two-pass tidy gate: self-test + `scripts/rules_lint.sh` first, then `main` = system `clang-tidy-11`
  (`.clang-tidy`, `tests/.clang-tidy`) and `rules` = pinned `clang-tidy==23.1.4` in `venv/`
  (`scripts/tidy_rules.yaml`: cognitive complexity 20 + `misc-const-correctness` (#94) + the `custom-*` project
  rules; `cognitive` before #93).
  Per-file logs: `build/tidy/{src,tests}/<path>.log` and `build/tidy/rules/{src,tests}/<path>.log`;
  self-test and lint: `build/tidy/selftest/`.
  Result cache: `build/tidy-cache` (`TIDY_NO_CACHE=1` disables it). See [docs/development.md](docs/development.md#build--static-analysis).
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
- Unit tests alone: `bash scripts/test.sh` (host-side, no SDK; 71 suites / 10,135 checks + 11 TSan suites / 6,207 checks).
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

## Teamwork & Quota Discipline
- **Tier routing (assign minimal Tier before launch):**
  - **Tier 0** (docs, configs, `.bak`, issue close, 1-2 non-critical files): 1 agent, no sub-orchestrator,
    no recon, no LLM review; deterministic script only (`rules_lint.sh`, `sha256sum`, `gh api`, `git diff --stat`).
  - **Tier 1** (local fix/feature 1-5 files, tests, refactor): max 1 executor + 1 clean-context adversarial
    reviewer after full build + unit/TSan.
  - **Tier 2** (concurrency, RT core, queues, PTS, net stack): 1 scout + 1 executor + 1 reviewer + 1 fuzzer
    challenger (50k-100k steps before LLM review).
  - **Tier 3** (unknown bug, cross-host diag, A/B): <= 2 scouts (separate hosts) -> 1 A/B executor ->
    1 measurement auditor; raw JSON metrics.
  - Escalate Tier +1 only on explicit trigger (>5 files, mutexes/atomics/RT/net, or 2 gate failures in a row).
    Never run 5-verifier waves on Tier 0-1.
- **Flat hierarchy (`max_depth = 1`):** orchestrator invokes workers and reviewer directly; no sub-orchestrators.
- **`state.json` (<= 40 lines):** 1 shell call at session start and after major mutations (`timestamp_utc`, `repo`,
  `branch`, `head_sha`, `open_prs`, `open_issues`, `active_pids`, config `sha256`); no repeated multi-scout pre-flights.
- **`ledger.json`:** record real `commit_sha`, `pr_url`, `issue_state` after irreversible `git`/`gh` actions for
  idempotent `429` recovery.
- **Deterministic gate before LLM:** build, `clang-tidy`/`rules_lint.sh`, unit/TSan, public-hygiene `grep` (0 internal
  hostnames, LAN IPs, or local paths in `git diff`/PR drafts), and fuzzer (Tier 2) must pass before LLM review.
- **1 clean-context adversarial reviewer:** receives only `git diff`, test output, and `state.json` (no executor
  persuasive summary). Auto-reject any new OS/driver call in `git diff` without a primary-source link and rationale.
- **Context diet & wrappers:** stdout to LLM `<= 80` lines (`<= 25`-line summary for long wrappers with
  `trap cleanup EXIT`), `handoff.md <= 120` lines. Max 2 fix iterations per stage before escalating to the user.
- **Research-Before-Code Gate (Rule 9):**
  - **Trigger:** any task touching OS APIs, thread scheduler/priorities, timers/sleep, VSync/DWM/OpenGL, drivers
    (GPU, V4L2, MPP, RGA), audio streams, network backpressure/congestion control, or non-trivial concurrency.
  - **Before coding:** run 1-2 targeted primary-source queries (Microsoft Learn, Khronos, Linux kernel/RT, FFmpeg,
    Rockchip SDK, Chromium/SDL) and record 2-3 canonical links + explicit "Known anti-patterns we avoid" in the plan.
  - **Ablation rule:** never bundle system hacks (priorities, affinity, global timer) without measuring each one's
    contribution; drop any hack before PR if metrics hold without it.

## Project rules (full text: [CONTRIBUTING.md](CONTRIBUTING.md))
(gate) = enforced by `scripts/tidy_run.sh` (`custom-*` checks and `misc-const-correctness` of the `rules` pass,
`scripts/rules_lint.sh`, both self-tested on `tests/tidy_probe/`): nobody re-checks it by hand. Fix a (gate) finding
like any other gate finding; a NOLINT that switches a `custom-*` check or `misc-const-correctness` off (by name or
glob: `*`, `cust*`, `misc-*`; every NOLINT of a line counts) only
with `: <reason>` and a line in `scripts/rules_allow.txt` (a CONTRIBUTING-listed site), decided by the main session;
only `NOLINT` / `NOLINTNEXTLINE`, never a `NOLINTBEGIN` / `NOLINTEND` range.
(review) = only `commit-reviewer` can judge it.
- C++17 (review). No virtuals or inheritance hierarchies (gate); templates for static polymorphism.
- No manual heap (`new`/`delete`, `malloc` family, `unique_ptr`/`shared_ptr`/`weak_ptr`), no statics of any
  kind, no globals, no `const_cast` (gate). Frame path: no allocation; preallocated members cleared at entry
  by the callee (review).
- Pixels exist only as `dma-buf`s: no CPU pixel copies (review).
- Dependencies injected by reference to a concrete class (`StopFlag&`, `const Log&`) (review).
- `const` by default: unchanged locals, read-only pointees and read-only reference / pointer parameters of free
  functions (gate: `misc-const-correctness`); member function / constructor / lambda parameters, dependent locals
  in templates, `const` methods (review). A false positive is worked around in code or NOLINTed with a reason.
- Diagnostics through `Log` (`include/core/log.hpp`); only `main.cpp` owns `std::cout`/`std::cerr`/`std::clog`;
  no C stdio output (gate).
- Header layout: in each visibility section, functions first, then a fresh label for data (review).
- Header extensions: headers use `.hpp`, no `.h` (gate).
- `core/` includes only the C++17 standard library and `core/*` (keeps tests SDK-free)
  (gate); include order: system and SDK headers first (gate); no GStreamer / OpenCV (gate).
- Exception (review): header-only libraries in `externals/` may keep file statics; include them
  only from `.cpp` files, never from a project header.
- Errors (`Status` / `noexcept`): recoverable failures return `Status` (`include/core/status.hpp`);
  exceptions (`std::runtime_error` via `throw_on`) are startup-only; frame path and thread bodies are
  `noexcept` (no `throw`: review; no `catch (...)` outside the two allowlisted sites of CONTRIBUTING: gate).
  Exit codes (review): `0` clean, `1` runtime fatal, `2` bad CLI/config, `3` startup failure.
- Output side vs recognition (review): no viewer -> output side sleeps completely (`enc=0.0`); recognition is
  viewer-independent (runs whenever >= 1 source is live, sleeps when all sources are absent); with viewer, online
  sources run full path and absent sources show `NO SIGNAL`.
- Control flow (gate): no `if` / `else if` ladders (`if constexpr` too; `switch`, tables, early returns,
  independent `if`s), no loop with an empty body, no `for (;;)` / `while (true)`: every loop states its exit condition.
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

## Completed soft-rtos & preempt-kernel cycle (`#123`–`#131`, Sprint 1 & Sprint 2)
- **Kernel & boot cmdline (RK3588):** transitioned to `Linux 5.10.226-preempt #7 SMP PREEMPT` with cleaned `/proc/cmdline` (`isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 irqaffinity=0-3`) and GPU/`rkvenc-core` IRQ offload to A55 cores `0-3` (`#130`).
- **Soft-RTOS (`#123`–`#126`, PR `#127`):** stack prefault, `mallopt` tuning, RT page-fault/p99 telemetry (`#123`), lock-free `AsyncLogSink` + `PiMutex`/`PiCondVar` (`#124`), systemd `WatchdogSec=5s` `NOTIFY_SOCKET` client (`#125`), and `cyclictest` jitter bench (`350 us` gate in `#128`).
- **Sprint 1 (`#131`) & Sprint 2 (`#132`):** RT hot-path/resilience fixes (`StreamOutput`, `H264Encoder`, `SdWatchdog`, `CameraV4L2`, `MuxSession`) and expanded TSan/RT coverage (`71` unit suites / `9,617` checks + `11` TSan suites / `6,188` checks at Sprint 2; `10,135` / `6,207` on current `master`).
