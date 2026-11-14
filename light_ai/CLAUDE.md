# light_ai -- working notes for Claude Code

Claude Code runs ON the dev board (CoolPi RK3588, Debian 11, aarch64): build, unit tests,
clang-tidy and live runs are local commands. This file is the handoff between sessions:
keep "Migration plan" current when a stage changes state.

## Where things are
- Work directory: ~/Projects/light/light_ai (start `claude` here). The git root is
  ~/Projects/light: the repo also holds light_control/ and light_sensor/ -- other projects,
  out of scope: never read for the task, edit, stage or commit anything outside light_ai/.
- Physical path: /userdata/home/linaro/Projects/light (eMMC). This is also the user's own
  working copy: dataset*/, venv/, artifacts/, build-tidy/ belong to the user -- never delete
  or rewrite them.
- Home and /tmp are on eMMC (15 GB): keep scratch files in $TMPDIR (NVMe,
  /mnt/nvme_data/claude/tmp); build logs go to build/ (git-ignored).
- Models (*.rknn) are not in git: they live in light_ai/misc/.

## Workflow
- Tasks arrive as GitHub issues ("Выполни issue #N"): read the issue, post the plan as a comment
  on it (`gh issue comment N --body-file ...`), wait for the user's OK there, then code.
- Shared working copy: run `git status` before any checkout or merge. Uncommitted changes
  under light_ai/ -> stop and ask the user; changes elsewhere are the user's -- leave them,
  and if git refuses the checkout because of them, ask. Never stash, reset or discard.
- Stage explicit paths inside light_ai/ (paths are relative to this directory: `git add src/x.cpp`),
  never `git add -A` / `git add .` from the git root / `git commit -a`.
- Base branch: fix/audit-p0. One branch per stage: refactor/stageN-<name>; PR into fix/audit-p0
  with `gh pr create --base fix/audit-p0`. The user reviews and merges.
- PRs into fix/audit-p0 do not auto-close issues (`Closes #N` fires only on the default branch).
  After the user merges the LAST PR of an issue, close the issue with
  `gh issue close N --reason completed --comment "Done in #PR..., merged into fix/audit-p0."`.
- Never push to fix/audit-p0 directly, never force-push, never rewrite pushed history.
- Before a PR the full gate must be green on this board:
      ./scripts/build.sh > build/gate.log 2>&1; echo rc=$?; tail -n 40 build/gate.log
  (CMake build + unit tests + TSan suites + clang-tidy gate; SKIP_TESTS=1 / SKIP_TSAN=1 / SKIP_TIDY=1 to narrow it.)
  On a tidy failure the per-file logs are in /tmp/tidy/<file>.log.
- Unit tests alone: bash scripts/test.sh (host-side, no SDK; see README, Tests).
- Live run: sudo -n timeout -s INT 60 ./build/light_ai --device /dev/video20 --pool 3 --frames 600
  timeout goes INSIDE sudo: `timeout ... sudo ./build/light_ai` does not deliver SIGINT -- sudo
  does not relay a signal sent from its own process group. To exercise the SIGINT path, make the
  timeout shorter than the frame budget; exit 124 = stopped by the timer (normal).
  Redirect to build/run.log and read it with tail/grep.
- Permissions: .claude/settings.json (tracked) pre-approves the gate, test, run and git/gh
  commands above and denies edits in light_control/ and light_sensor/, history rewrites,
  `git add -A` / `commit -a` and `git stash`. Personal overrides: .claude/settings.local.json.
- USB camera tests (unplug / replug, hot attach, start without a camera): NEVER ask the user
  to pull the connector -- repeated physical re-plugging wears the board's USB port. Unplug in
  software (sysfs `authorized`, a logical disconnect: the /dev/videoN node disappears and
  comes back, possibly under another number):
      sudo -n /usr/local/sbin/light-usb-replug list                       # nodes and busids
      sudo -n /usr/local/sbin/light-usb-replug cycle /dev/video20 5       # off 5 s, then on
      sudo -n /usr/local/sbin/light-usb-replug off /dev/video20           # prints the busid
      sudo -n /usr/local/sbin/light-usb-replug on <busid>
  Run the cycle from a second shell while a live run is going (the run under timeout in the
  background, its log in build/). It is a root-owned copy of scripts/usb_replug.sh, installed
  once by the user; after changing the script, ask the user to reinstall it. A physical
  re-plug only when the user explicitly asks for one.
- Logs are long: read tails and grep for errors, not whole files.
- Only one heavy job at a time (3.8 GB RAM): no build while a live run is measuring FPS.

## Model and delegation
- This main session runs Opus at high effort (.claude/settings.json: model, effortLevel):
  it owns design, refactoring, threading and ownership decisions, commits and PRs.
- Delegate routine work to the subagents in .claude/agents/ (Sonnet, medium effort):
  - board-check: every build / unit-test / tidy gate and every live run. It reports
    verdicts, first errors and FPS/TOTAL figures; it never edits code or touches git.
  - gate-fixer: mechanical gate failures (includes, naming, const, casts) with an
    obvious local fix. Anything touching logic, threads or interfaces stays here.
- Branch switching for A/B runs stays in this session: check out, then ask board-check
  to measure; keep baseline logs (build/run_*.log) to avoid re-measuring.

## Project rules (full text: README, "Rules of the project" and "Comment style")
- C++17. No virtuals or inheritance hierarchies; templates for static polymorphism.
- No manual heap (new/delete, unique_ptr/shared_ptr), no statics of any kind, no globals.
  Frame path: no allocation; preallocated members, the callee clears them at entry.
- Pixels exist only as dma-bufs: no CPU pixel copies.
- Dependencies injected by reference to a concrete class (StopFlag&, Log&).
- Diagnostics through Log (core/log.hpp). A class moves off std::cout/std::cerr in the
  stage that touches it; new code never writes to the streams directly.
- Header layout: in each visibility section, functions first, then a fresh label for data.
- core/ includes only the standard library and the pod containers (keeps tests SDK-free).
- Exception: header-only libraries in externals/ (stb_easy_font, ...) may keep their own file
  statics; include them only from a .cpp, never from a project header.
- Output side vs recognition (README, Run, "Output side and recognition"): no viewer -- the output
  side sleeps completely (no draw, no RGA compose, no NO SIGNAL frames, no encode); recognition
  (capture, decode, NPU, tracker) never sleeps; with a viewer, an online source gets the full path,
  an absent one the NO SIGNAL plate. Nothing on the output side runs before a session opens.
- Comments match the code and README; a comment about a mode/value is verified by a run.
  Max line length 140.

## Migration plan (audit doc: "light_ai: архитектурный аудит и план переработки")
- [x] 0 Safety net: domain types to core/, host unit tests (PR #6).
      Open: record a reference detection CSV on a fixed MJPEG file -- stage 6 compares against it.
- [x] 1 Global state: SignalWaiter/ProcessEnv (no g_pipe), Config grouped by consumer, Log (PRs #7-#9).
      Open: ~170 direct std::cout/cerr writes migrate per stage; no CLI option for the log level yet.
- [x] 2 InferencePool: slots, workers, FIFO, core masks out of Pipeline; InferSlot with private
      state; OverlaySink takes FrameView (PR #10).
      Check: --pool 1 and --pool 3 FPS and TOTAL timings within noise of fix/audit-p0;
      boxes drawn (GPU and --cpu-draw); camera unplug/replug under --pool 3; Ctrl-C summary.
- [x] 3 Sources: SourceChannel with an explicit State {NeverAttached, Detached, Live},
      SourceSet, SourceProber, UsbNodeLocator; merge init_one_source with the first_attach
      branch of probe_source. Pipeline loses ~9 methods; run() fits in 30 lines.
      Check: unplug/replug a USB camera and the net source; start with no sources.
      Issue #15, split 3a/3b/3c. 3a (SourceChannel, UsbNodeLocator, one attach path, TSan suite):
      merged (PR #16). 3b (SourceProber: own thread, RAII local of run()): merged (PR #17).
      3c (SourceSet, next_frame + step, run() in 30 lines): the PR that closes #15.
      Baselines (fix/audit-p0 @ 7ef8212): build/run_base_pool{1,3}.log, build/run_base_net.log.
- [x] 4 NetEncoder, one class at a time: ParamSetCache, PtsClock, BitmapFont + IdlePlate,
      PacketQueue, FrameComposer, MuxSession; the remainder is H264Encoder (100-200 lines each,
      trivial destructors). Check: viewer joins mid-stream, 1 and 3 sources, NO SIGNAL plate,
      exit without MPP errors.
      Issue #19 (plan + review in its comments), split 4a/4b/4c/4d. 4a (Log, EncoderConfig +
      StreamLayout, ParamSetCache, PtsClock, EncodePacer): merged (PR #20). 4b (BitmapFont +
      IdlePlate, EncoderFramePool, FrameComposer; review points 1-3 of #20): merged (PR #21).
      4c (PacketQueue with fail( epoch ), SessionGate, MuxSession; PtsClock restarted by the
      epoch on the pipeline thread; review of #21): merged (PR #22). Ownership table: #19.
      4d (H264Encoder, NetworkInit, ~NetEncoder() = default: the shutdown order is the member
      declaration order, written in net_encoder.h): the PR that closes #19.
      Baselines (fix/audit-p0 @ 2e8d17c, --net --net-h264, viewer connected):
      build/run_base_h264_pool{1,3}.log, build/run_base_h264_net.log, build/base_png*.png.
      Done after stage 4: #23 (every session starts with an IDR, UDP reopens too; branch
      fix/issue23-udp-idr); #24 (a multi-view session starts on a gray canvas and holds its first
      frame until every source cell is refreshed, <= 200 ms: SessionWarmup; the output-side rules in
      README and "Project rules"; branch fix/issue24-session-warmup). A/B logs:
      build/run24_{fix,fix2,fix3,base,base2}_mv.log, run24_*_{slow,grid}.log, frames build/24_*.png;
      the driver (live24.sh, cells.py, slow_mjpeg.py) was a session scratch script, not in git.
- [ ] 5 NetMjpegSource: BufferedSocket, MultipartReader, HTTP header parsing as a pure function
      over a buffer. Check: tests on recorded streams (cut-off, broken JPEG, no Content-Length);
      live run.
      Issue #28 (plan + review in its comments), split 5a/5b/5c. 5a (ByteWindow, HttpUrl, HttpHead,
      MultipartReader, jpeg_frame_check in core/; fixtures tests/data/; bare LF, the "--" boundary, the
      boundary from Content-Type only): PR #29. 5b: BufferedSocket, MjpegSlotPool + the mmap fix, Log,
      recover() removed. 5c: the NetMjpegSource remainder, loopback source tests; closes #28.
      Net disconnect check: iptables-legacy DROP of the phone's IP (from --net-src) for 8 s, one sudo -n sh -c,
      then -S; driver live28.sh was a session scratch script. A/B logs: build/run28_{base,5a}_{2x1,net,drop}.log.
- [ ] 6 Complex functions: YoloDecoder and BoxMapper as classes; KalmanAxis, Associator; split
      setup_device and GpuBoxDrawer::init. Goal: no function above cognitive complexity 20.
      Check: the reference CSV matches byte for byte; tighten the clang-tidy thresholds.
- [ ] 7 Errors: one Status type; exceptions only at startup; no catch (...) in the frame path.
      Check: runs with a wrong model, a busy port, a missing camera.
Out of scope: light_control, light_sensor, tools/, scripts/.
