# light_ai -- working notes for Claude Code

Claude Code runs ON the dev board (CoolPi RK3588, Debian 11, aarch64): build, unit tests,
clang-tidy and live runs are local commands. This file is the handoff between sessions:
keep "Migration plan" current when a stage changes state.

## Where things are
- Work directory: ~/Projects/light/light_ai (start `claude` here). The git root is
  ~/Projects/light: the repo also holds light_control/ and light_sensor/ -- other projects,
  out of scope: never read for the task, edit, stage or commit anything outside light_ai/.
- Physical path: /mnt/nvme_data/linaro/Projects/light (NVMe, #48). ~/Projects/light is a symlink to it
  (home = /userdata/home/linaro, eMMC); a symlink, not a bind mount: if the NVMe is missing the project is
  unavailable, the system is fine. This is also the user's own working copy: dataset*/, venv/, artifacts/,
  build-tidy/ belong to the user -- never delete or rewrite them.
- Everything the project generates stays inside the project, under build/ (git-ignored): the CMake tree,
  the test builds, tidy logs and cache, run logs, CSVs, frames, baselines. Never /tmp or the home (eMMC,
  wears out); the system keeps its own /tmp and environment (no TMPDIR in the user's shells). Claude Code's
  own $TMPDIR (/mnt/nvme_data/claude/tmp, NVMe) is for session scratch only. build.sh / tidy_run.sh warn
  when their output is on the eMMC (README, Build, "Where the files live").
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
  The tidy gate has two passes (#43): main = system clang-tidy 11 with .clang-tidy (every check), then
  cognitive = the pinned clang-tidy 23.1.4 in light_ai/venv with scripts/tidy_cognitive.yaml (cognitive
  complexity only, Threshold 20). The gate never installs into the venv: a missing / other version fails at
  once with the install command (README, Build). TUs: src/ and tests/ (tests/.clang-tidy relaxes only the
  function size). On a tidy failure the per-file logs are in build/tidy/{src,tests}/<path>.log (main) and
  build/tidy/cognitive/{src,tests}/<path>.log. Both passes run the TUs in parallel
  (TIDY_JOBS=N, default by MemAvailable) with an exact result cache in build/tidy-cache (TIDY_NO_CACHE=1
  disables it); README, Build.
- Unit tests alone: bash scripts/test.sh (host-side, no SDK; see README, Tests).
  One suite / a flake hunt: build/tests/light_ai_tests [substr...] [--repeat N] (also light_ai_tests_tsan;
  exit 2 on a bad argument; README, Tests, "Options").
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
      Reference detections (issue #32, reference/README.md): clip misc/ref_evening.mjpeg (1063 frames,
      sha256 in the README); layer A reference/ref_evening.csv (--box-log of a drop-free replay,
      --pool 1 = --pool 3, cmp-identical); layer B tests/test_reference_chain.cpp (golden, in the gate).
- [x] 1 Global state: SignalWaiter/ProcessEnv (no g_pipe), Config grouped by consumer, Log (PRs #7-#9).
      The direct std::cout/cerr writes migrated per stage; the last ones (RknnEngine, CalDumper) in #51: only
      main.cpp's Log( std::cout, std::cerr ) and print_usage( std::cout ) remain. Open: no CLI option for the log level yet.
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
- [x] 5 NetMjpegSource: BufferedSocket, MultipartReader, HTTP header parsing as a pure function
      over a buffer. Check: tests on recorded streams (cut-off, broken JPEG, no Content-Length);
      live run.
      Issue #28 (plan + reviews in its comments and in #29 / #30), split 5a/5b/5c. 5a (ByteWindow,
      HttpUrl, HttpHead, MultipartReader, jpeg_frame_check in core/; fixtures tests/data/; bare LF, the
      "--" boundary, the boundary from Content-Type only): merged (PR #29). 5b (BufferedSocket,
      MjpegSlotPool + the mmap fix, Log, recover() removed; "source s<N>:" warm-up / zero-size texts):
      merged (PR #30). 5c (Read / Ready results, the thread rule in net_mjpeg_source.hpp, fcntl checked,
      NetMjpegSource end to end over loopback, README): the PR that closes #28.
      Open: an unquoted boundary keeps trailing blanks (review of #29, point 4; no server seen sends one).
      Net disconnect check: iptables-legacy DROP of the phone's IP (from --net-src) for 8 s, one sudo -n sh -c,
      then -S; driver live28.sh was a session scratch script. A/B logs: build/run28_{base,5a}_{2x1,net,drop}.log,
      build/run28_{base5b,5b}_{2x1,net}.log, build/run28_5b_drop.log, build/run28_{base5c,5c}_{2x1,net}.log,
      build/run28_5c_drop.log.
- [x] 6 Complex functions: YoloDecoder and BoxMapper as classes; KalmanAxis, Associator; split
      setup_device and GpuBoxDrawer::init. Goal: no function above cognitive complexity 20.
      Check: layer A -- bash tools/ref_replay.sh misc/ref_evening.mjpeg build/ref_p3.csv 3 reference/ref_evening.csv,
      and the same with pool 1 (~107 s each): PASS = cmp-identical, i.e. equal to 6 significant digits (the
      --box-log precision; a last digit flipped at a rounding boundary is explained, not a behavior change);
      layer B -- reference_chain in scripts/test.sh (tolerances in tests/data/README; never regenerate the
      golden in stage 6). Tighten the clang-tidy thresholds.
      Issue #34 (plan + review in its comments), split 6a-6e. 6a (box_math.hpp to core/, fp16_to_float with an exact
      software path, YoloDecoder in core/, one decoder per pool worker): merged (PR #35). 6b (KalmanAxis, Associator,
      TrailWindow in core/, the Tracker split; golden max |d| 0): merged (PR #36). 6c (BoxMapper class, BoxSmoother
      in steps + Log, the dead DEAD-ghost-box branch removed; golden max |d| 0): merged (PR #38). 6d (setup_device and
      GpuBoxDrawer::init in steps, the drawer on Log, function-size thresholds 100/110/18/5/17): merged (PR #40);
      #34 closed.
      6e: issue #39 (plan + approval in its comments 6894174610 / 6894216198), split 6e1/6e2/6e3. The eight functions
      above 20 outside the six targets, plus RgaResizer::resize and MppDecoder::retrieve_output (they hold the size
      thresholds); then the thresholds go to lines 90 / statements 60 / branches 11 / nesting 4 / variables 14.
      6e1 (stamp, probe_jpeg_sof, ConsoleReporter + core/fixed_fmt.hpp, CliOptions + the config/ layer on Log,
      the Log constructed before the parse): merged (PR #44). 6e2 (CameraV4L2::dequeue, NetMjpegSource::dequeue,
      MppDecoder::retrieve_output and RgaResizer::resize in steps; MppDecoder and RgaResizer on Log): branch
      refactor/stage6e2-capture, merged (PR #47). 6e3 (H264Encoder::drain, NetPreview::serve + NetPreview on Log, the
      function-size thresholds to lines 90 / statements 60 / branches 11 / nesting 4 / variables 14): branch
      refactor/stage6e3-output, merged (PR #50); #39 closed: stage 6 done. 6e3 logs (base binary build/light_ai_base6e2: the output side
      is unchanged from fix/audit-p0 @ 5e4d9d3): build/run6e3_{base,new}_{h264,udp,raw}.log, viewers .v1.log / .v2.log;
      driver live6e3.sh (session scratch, not in git): no viewer 0-10 s, ffmpeg pull 10-16 s, a second one through SIGINT.
      A UDP ffmpeg input never sees EOF: `timeout` alone does not end it, kill it after the run.
      Re-measure at f2f100d: cognitive list unchanged from 4b97d21; scripts in the session scratchpad (not in git).
      Baseline binaries: build/light_ai_base6e (f2f100d), build/light_ai_base6e2 (6d44748, fix/audit-p0 before 6e2).
      6e2 logs: layer A build/ref6e2_p{3,1}.*; a 1920x1080 camera clip replayed base vs new build/ref6e2_1080_*
      (one-pass only: the two-pass RGA path needs a source over 8x the 640x640 model, unreachable with misc/ models);
      build/run6e2_{base,new}_{pool3,pool1,gpu,cpu,net,2x1}.log, build/run6e2_new_{replug,drop}.log, build/6e2_*.png.
      After a replug cycle the camera may come back as /dev/video21: check light-usb-replug list before the next run.
      clang-tidy 11 by hand (size metrics) also needs the compile DB copy without -mtune (else clang-diagnostic-error).
      6e1 logs: build/run6e1_{base,new}_{pool3,nosig}.log, frames build/6e1_{base,new}_nosig.png (byte-identical),
      layer A build/ref6e1_p{3,1}.{csv,log,out}. NO SIGNAL check: light-usb-replug off /dev/video20 prints
      "usb_replug: <busid> off (...)" -- the busid is its 2nd field, not the whole line.
      Cognitive complexity: clang-tidy 11 has no such check (LLVM 12+); since #43 the gate's second pass enforces it
      (venv/bin/clang-tidy 23.1.4, --config-file=scripts/tidy_cognitive.yaml, Threshold 20). By hand (a list of
      values, not only the findings): the same binary with -p <copy of build/compile_commands.json without
      -mtune=cortex-a76.cortex-a55> and a config copy with Threshold 0.
      Baseline (4b97d21): associate 49, setup_device 44, collect_head 42, GpuBoxDrawer::init 36, Tracker::update 35,
      fill_trail_points 35, BoxSmoother::update 32; outside the targets: BitmapFont::stamp 33, CameraV4L2::dequeue 31,
      probe_jpeg_sof 29, ConsoleReporter::report 25, H264Encoder::drain 24, CliOptions::parse 23, NetMjpegSource::dequeue 22,
      NetPreview::serve 21. After 6a: YoloDecoder max 10; after 6b: Tracker max 13 (Associator::match); after 6c:
      BoxMapper max 7, BoxSmoother max 14 (collect_pairs); after 6d: setup_device 3 (steps max 11, map_buffers),
      GpuBoxDrawer::init 4 (steps max 7). Layer A logs: build/ref6{a,b,c}_p{3,1}.{csv,log}, build/ref6d_p3.{csv,log}.
      6d live logs (baseline binary build/light_ai_base6d): build/run6d_{base,new}_{pool3,pool1,gpu,cpu}.log,
      build/run6d_new_{replug,nocam}.log, frames build/6d_*.png. Function-size measurement: clang-tidy 11 with every
      readability-function-size threshold at 0 (nesting: one pass at the candidate threshold, threshold 0 shows level 1 only).
      Open: an alive track whose trail ages out completely never gets trail points again (TrailWindow::feed resets
      the median window on every call while the ring is empty, not once); kept in 6b (a refactoring): issue #37, after
      stage 6.
      After stage 6: #42 (parallel tidy gate + cache, PR #46) and #48 (NVMe checkout, PR #49) closed. #43 (tidy gate:
      plan + decision in its comments 6897676300 / 6897763312): the full move to LLVM 23 dropped (~1870 findings,
      almost all style or against the project rules; measurement logs build/tidy43/); the gate stays on clang-tidy 11
      plus a cognitive-only LLVM 23 pass. 43a (the second pass, the cache key with the --config-file bytes, the
      empty-list guard, widening / std::forward fixes from the LLVM 23 list): branch ci/issue43-tidy23, merged (PR #52).
      43a logs: build/tidy43/{cold,warm}.log, layer A build/ref43_p{3,1}.*, live (base binary build/light_ai_base43 =
      fix/audit-p0 @ 0dc8c86) build/run43_{base,new}_{pool3,gpu,cpu,replug}.log, frames build/43_*.png. 43b (tests/
      under both passes, tests/.clang-tidy, the test fixes): branch ci/issue43-tidy-tests, merged (PR #53); #43 closed. clang-tidy 11
      on tests/ before 43b: 42 findings (16 function-size, 26 in 12 checks), all fixed but the size; measurement
      logs build/tidy43b/.
      #51 (plan + OK in its comments): RknnEngine and CalDumper on Log; --dump-cal writes build/cal.ppm (<cal-dir>/cal.ppm
      with --cal-dir, now accepted with --dump-cal alone); "calibration dump failed: <path>"; scripts/probe_yolo.py [PPM]
      (rknnlite: run it with venv/bin/python). Branch refactor/issue51-log-tails. Logs (base binary build/light_ai_base43:
      rknn_engine / cal_dumper unchanged since): build/run51_{base,new_dump,new_caldir}.log, model lines
      build/run51_{base,new}.model.txt (identical), /tmp listings build/tmp51_{before,after}.txt (identical). A sudo run
      leaves build/cal.ppm root-owned: a later non-root dump fails until it is removed.
- [ ] 7 Errors: one Status type; exceptions only at startup; no catch (...) in the frame path.
      Check: runs with a wrong model, a busy port, a missing camera.
Out of scope: light_control, light_sensor, tools/, scripts/.
