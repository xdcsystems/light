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
- Shared working copy: run `git status` before any checkout or merge. Uncommitted changes
  under light_ai/ -> stop and ask the user; changes elsewhere are the user's -- leave them,
  and if git refuses the checkout because of them, ask. Never stash, reset or discard.
- Stage explicit paths inside light_ai/ (paths are relative to this directory: `git add src/x.cpp`),
  never `git add -A` / `git add .` from the git root / `git commit -a`.
- Base branch: fix/audit-p0. One branch per stage: refactor/stageN-<name>; PR into fix/audit-p0
  with `gh pr create --base fix/audit-p0`. The user reviews and merges.
- Never push to fix/audit-p0 directly, never force-push, never rewrite pushed history.
- Before a PR the full gate must be green on this board:
      ./scripts/build.sh > build/gate.log 2>&1; echo rc=$?; tail -n 40 build/gate.log
  (CMake build + unit tests + clang-tidy gate; SKIP_TESTS=1 / SKIP_TIDY=1 to narrow it.)
  On a tidy failure the per-file logs are in /tmp/tidy/<file>.log.
- Unit tests alone: bash scripts/test.sh (host-side, no SDK; see README, Tests).
- Live run: timeout -s INT 60 sudo -n ./build/light_ai --device /dev/video20 --pool 3 --frames 600
  SIGINT exercises the graceful-shutdown path; exit 124 = stopped by the timer (normal).
  Redirect to build/run.log and read it with tail/grep.
- Permissions: .claude/settings.json (tracked) pre-approves the gate, test, run and git/gh
  commands above and denies edits in light_control/ and light_sensor/, history rewrites,
  `git add -A` / `commit -a` and `git stash`. Personal overrides: .claude/settings.local.json.
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
- Comments match the code and README; a comment about a mode/value is verified by a run.
  Max line length 140.

## Migration plan (audit doc: "light_ai: архитектурный аудит и план переработки")
- [x] 0 Safety net: domain types to core/, host unit tests (PR #6).
      Open: record a reference detection CSV on a fixed MJPEG file -- stage 6 compares against it.
- [x] 1 Global state: SignalWaiter/ProcessEnv (no g_pipe), Config grouped by consumer, Log (PRs #7-#9).
      Open: ~170 direct std::cout/cerr writes migrate per stage; no CLI option for the log level yet.
- [ ] 2 InferencePool: slots, workers, FIFO, core masks out of Pipeline; InferSlot with private
      state; OverlaySink takes FrameView (PR #10, awaiting the board check).
      Check: --pool 1 and --pool 3 FPS and TOTAL timings within noise of fix/audit-p0;
      boxes drawn (GPU and --cpu-draw); camera unplug/replug under --pool 3; Ctrl-C summary.
- [ ] 3 Sources: SourceChannel with an explicit State {NeverAttached, Detached, Live},
      SourceSet, SourceProber, UsbNodeLocator; merge init_one_source with the first_attach
      branch of probe_source. Pipeline loses ~9 methods; run() fits in 30 lines.
      Check: unplug/replug a USB camera and the net source; start with no sources.
- [ ] 4 NetEncoder, one class at a time: ParamSetCache, PtsClock, BitmapFont + IdlePlate,
      PacketQueue, FrameComposer, MuxSession; the remainder is H264Encoder (100-200 lines each,
      trivial destructors). Check: viewer joins mid-stream, 1 and 3 sources, NO SIGNAL plate,
      exit without MPP errors.
- [ ] 5 NetMjpegSource: BufferedSocket, MultipartReader, HTTP header parsing as a pure function
      over a buffer. Check: tests on recorded streams (cut-off, broken JPEG, no Content-Length);
      live run.
- [ ] 6 Complex functions: YoloDecoder and BoxMapper as classes; KalmanAxis, Associator; split
      setup_device and GpuBoxDrawer::init. Goal: no function above cognitive complexity 20.
      Check: the reference CSV matches byte for byte; tighten the clang-tidy thresholds.
- [ ] 7 Errors: one Status type; exceptions only at startup; no catch (...) in the frame path.
      Check: runs with a wrong model, a busy port, a missing camera.
Out of scope: light_control, light_sensor, tools/, scripts/.
