# Roadmap (light_ai)

The milestone plan and the closed-milestone history that used to live
under README.md. Pointers like [Platform constraints], [Run], [Viewer]
refer to README.md sections (../README.md).

## Roadmap
- [x] M1: baseline run with mobilenet_v1, honest FPS measurement.
- [x] M1.5: per-stage timing (cam / decode / resize / infer) — bottleneck
      identified as the camera source.
- [x] M1.6: hardening — model dims from the model file, G_FMT verification,
      strict CLI parsing, error-path cleanup in the decoder, exact output
      buffer sizing. FPS unchanged (camera-bound, as expected).
- [x] M2-prep: 1080p pipeline end-to-end — MPP 1088-row alignment, RGA3
      two-pass resize (aux dma-buf from the decoder pool), RGA3 pin,
      5000-frame soak; camera ceiling mapped (exposure-dependent fps).
- [x] M2.1: yolov5s_relu.rknn (640x640) on board — letterbox re-derived
      from the model (lb_top=140, lb_h=360, bars verified 128), RknnEngine
      N-output support, pipeline runs NPU-bound at ~16.8 FPS (720p evening).
- [x] M2: yolo post-processing — decode 3 heads (255 = 3 anchors x 85, NCHW,
      fp16, post-sigmoid), obj*cls threshold (0.25), NMS (IoU 0.45),
      coordinate mapping via T; letterbox-geometry consistency check.
      Verified visually: /tmp/cal.ppm overlay with boxes re-mapped scene ->
      det (PNG writer in pure stdlib, no PIL in the venv). Night scene:
      two chairs (cls 56) with partial-overlap duplicate box, potted plant
      (cls 58) boxed correctly — COCO class ids match the scene, so the
      decode is verified semantically, not just geometrically. Duplicate
      boxes survive NMS at conf ~0.3 (IoU below threshold) — tunable via
      conf_thr/iou_thr if it matters. FPS ceiling is now infer (~47 ms):
      post-processing is invisible in the timing sum.
- [x] M2-alt: tested yolov8n (fp16, same recipe) — infer 65 ms vs 47 ms
      yolov5s: the DFL head + flat (1,84,8400) output defeat the FLOPs
      advantage on the NPU (needed disable_rules=['seperate_conv_rs_split'],
      op fuse incomplete). Conclusion: lighter model is not the FPS lever;
      yolov5s stays. Next candidate lever: int8 quantization (needs a
      calibration dataset of night frames — dumper already exists).
- [x] M2.3: readable report (COCO-80 class names), CLI --conf/--iou with
      validation; cleanup: class-aware NMS, symmetric box clamping, letterbox
      geometry now read from RgaResizer (single source of truth).
- [ ] M2.4-b: model-optimization backlog (reconstructed 2026-10-28;
      the original deferred-step list lived only in the 2026-10-26
      session chat and the Colab notebooks — git carries no other
      trace): (a) formal day A/B for v3 — the model notes still say
      "day A/B pending" (night A/B x3 done, see M2.4); (b) MMSE
      re-quant with a larger sample budget — 50 samples fit the
      Colab 12 GB, 200 OOM (see the M2.4 recipe; an mmse model exists and is the working
      model since 2026-10-30 — see the int8 notes in Platform
      constraints); (c) further
      deferred steps: transcribe from the Colab notebooks.
- [ ] M2.6: v4 recalibration under the 1280x960 pipeline + mmse A/B
      (plan fixed 2026-10-30). Quality-first framing: lighter models
      (yolov5n, 512x512) REJECTED by owner decision — see the 960p
      section; one model = one NPU core (Platform constraints), so
      FPS work is frozen at ~20 FPS source-bound. Steps:
      (a) DONE 2026-10-30: fresh dump from the 960p pipeline
      (--cal 100 --cal-interval 15, ~120 MB on /tmp, now
      dataset_day3) -> re-quant via the model-zoo convert.py with
      mmse -> yolov5s_relu_int8_v4.rknn (48 min Colab — see INT8
      build history);
      (b) DONE 2026-10-30: three-way A/B (v3/v4/int8_mmse) on the
      day, dusk and night dumps within one session — see docs/state.md (INT8 build history);
      (c) reconcile bench_npu 15.4 ms@012 against the single-core
      finding (parallel-contexts hypothesis -> M5);
      (d) DONE 2026-10-30 (INT8-vs-INT8): night A/B on dataset_mix,
      gamma 2.0, conf 0.18 — v4 == v3 except one isolated lost
      detection (n2_cal_0040; details in docs/state.md (INT8 build history).
      Remaining from (b)+(d): the owner's working-model call
      (int8_mmse vs v4) and a formal fp16-reference check.
- [x] M2.4: int8 quantization — CLOSED 2026-10-26 on v3 (day-4
      daytime recalibration). v3 recipe: calibrated on 60 fresh day-4
      daytime frames, quant config mean=0/std=255 -> canonical input scale
      1/255 (0.00392157, zp=-128); the "not 1/255 — fitted" caveat
      in Platform constraints applies to v2 only.
      Night A/B vs fp16 (3 sessions x 300 frames each, --core 7,
      conf 0.25, static evening scene; r3 final): stable objects
      (chair, potted plant) match fp16 100% by class in every
      session, box shift 0.8-1.3 px (day ref <=2), dconf mean
      +0.002..+0.010 (no systematic bias; v2: -0.045), vase visible
      in 17/20 steady blocks by v3 (r3; fp16: 16/20 in the same run;
      v2: 0/20). infer 24.8 vs 28.9 ms
      at 25-FPS pacing (~15% int8 gain), FPS identical (source-
      bound) — headroom saved for M4. Working conf for v3: 0.25
      (vase 0.29-0.37 / bowl 0.33-0.45 in r3 whenever detected; both
      objects drop out entirely in 3/20 blocks — f60/f75/f135 — and
      fp16 drops them in 4 blocks of the same run: edge-object
      absence, not a conf shortfall).
      compare_night residual ALARM on r2/r3: confined to the conf
      band 0.18-0.45 (bowl/vase edge objects) where the fp16
      reference itself flickers between runs (bowl<->vase flips on
      the same box across sessions, duplicate conflicting-class
      boxes at f45 r3, conf dips below 0.25 in varying blocks);
      v3 holds these objects MORE consistently than fp16 — the
      binary verdict is too strict for the evening conf band,
      documented, not a defect. Two intermediate sessions were
      scene-contaminated (person crossed between the paired runs;
      also object layout changed between hours) — discarded;
      methodology lesson: A and B runs must capture the same static
      scene within one session.
      v2 verdict: night A/B re-run done 2026-10-26 — ALARM confirmed
      (systematic dconf -0.045, vase missed 20/20 steady blocks,
      bottle down to -0.24): the recalibration did not fix v2's
      night bias. v2 superseded by v3 for night scenes, file kept
      for reference. Fallback (path A) never needed for v3.
- [x] M2.5: multi-core NPU infer (--core, rknn_set_core_mask) — measured.
      Daytime A/B, 300 frames each, steady state (first 60-frame warmup
      block excluded): AUTO 25.1 ms infer, core0 25.1 ms (AUTO == core0 —
      the runtime already picked a single core), cores 012 21.2 ms (~15%
      faster, consistent across all five blocks). FPS unchanged (~30.2
      steady in all runs): the pipeline is source-bound at the camera's
      ~30 FPS cadence, sum stays 33.1 ms everywhere — the saved ~4 ms is
      headroom that pays off only when the source exceeds 30 FPS (M4).
      Recommended run flag: --core 7; default stays AUTO so the
      historical baseline remains reproducible. mpp_buffer init assert
      seen in every run incl. pre-patch smoke — benign so far;
      re-checked in the night A/B (2026-10-26): 2 asserts at startup
      of every run, A=2/B=2 in all compare outputs — still benign.
      Update 2026-10-27: root-caused — the prints were OUR OWN
      mpp_buffer_group_get calls with caller=nullptr (decoder x2,
      encoder x1); fixed by passing __FUNCTION__. Zero prints since,
      [timing] unchanged — see Platform constraints.
      Follow-up (same day): the 012 gain is CPU-frequency dependent.
      With CPU governors on performance, cores 012 reproduce 21.2 ms
      (steady 20.5-21.6) and AUTO stays 25.1 — both match the morning
      numbers exactly. After an idle period on the default governor,
      cores 012 degrade to ~25.1-26.0 (no gain at all), while AUTO is
      unaffected in both states: the multi-core split is coordinated by
      the CPU, so CPU performance is a prerequisite for the M2.5
      numbers. NPU/DDR governors = performance are confirmed harmless
      (AUTO baseline unchanged, 41.6C, npu 1 GHz, dmc 2400 MHz).
      NPU load during a cores-012 run (debugfs, sampled mid-run):
      Core0 20% / Core1 19% / Core2 16% — the work is actually spread
      across all three cores, evenly within counter resolution.
      Update 2026-10-30 (supersedes the spread reading): 1280x960
      pipeline, daytime, steady, mask A/B 600 frames each: --core 1
      32.5 ms / --core 3 32.6 / --core 7 30.0 (<=2.5 ms gain; the
      earlier 25.1->21.2 delta does not reproduce on v3/mmse at this
      pacing). Debugfs, --core 1: only Core0 busy ~30%, Core1/2 at
      zero — single-core execution; --core 7 reads ~11-13% per core
      (same total work through rotation/counter smoothing, not layer
      parallelism) — the earlier 20/19/16% reading is re-interpreted
      the same way. The 21.2 ms figure quoted in
      scripts/set_governors.sh belongs to the older setup.
- [x] M3-alt: NV12-family model input — probed and closed negative. The
      RKNPU2 API on board (rknn_type.h / rknn_api.h) has no YUV tensor
      types: rknn_tensor_type is numeric only (FLOAT/INT/BFLOAT),
      rknn_tensor_fmt is layout only (NCHW/NHWC) — set_io_mem cannot
      declare an NV12 input, zero-copy YUV does not exist in this SDK.
      Confirmed against the full toolkit changelog (v0.6..2.5.0, no NV12
      entry) and the official demos (all do YUV->RGB via RGA/CPU). Even
      hypothetically the win is ~1 ms of rga (0.8 -> ~0.4) on a 39.8 ms
      frame (<2%): not worth a model rebuild + requantization. The real
      FPS lever is the model input size — see M3-alt2.
- [x] M3-alt2: yolov5s_relu_int8 @ 512x512 — closed by decision, not
      measured: smaller input means smaller grids (mid head 40x40 ->
      32x32), and losing small/distant objects at night is unacceptable
      for this task. The ~5 ms projected infer saving does not buy back
      the recall risk. 640x640 stays; model-side resize levers are
      exhausted — remaining FPS levers are on the platform side.
- [ ] M3.5: netsend — network preview subsystem (design agreed
      2026-10-24). STATUS 2026-10-26 (day session, v2 model —
      superseded by v3 later the same day, see M2.4): phase A
      LIVE on both transports, PULL is the product.
      - h264 TCP pull (product path, VERIFIED live 2026-10-26):
        net_encoder listens on --net-port (default 1234), the viewer
        connects ffplay -fflags nobuffer tcp://<board-ip>:1234; the
        run command carries no addresses. One viewer; no encode until
        the first viewer connects (publish() sees a null context);
        a stalled viewer is dropped via SO_SNDTIMEO (2 s), a viewer
        drop reopens the listener without touching the pipeline.
        Live log confirmed: listener up before the viewer, "viewer
        connected" at connect time, pipeline steady (~26 FPS,
        sum ~38 ms).
      - Raw NUT (tensor path, --net): TCP server, 640x640 letterboxed
        rgb24; verified end-to-end in VLC. Demoted to diagnostic duty
        2026-10-26: ~246 Mbps (640x640 rgb24 @ 25 fps) vs ~12 Mbps
        h264 (default CBR; 2500 kbps is plenty) — fine for
        pixel-accurate debugging, but saturates a LAN link.
      - UDP push (--net-host <pc-ip>) stays the legacy opt-in (the
        Windows IPv6-bind lesson below applies to push mode only).
      NetControl DESCOPED (owner decision 2026-10-28): the pull
      model makes stream on/off implicit — publish() is a no-op
      with no viewer, a stalled viewer is dropped (SO_SNDTIMEO)
      and the listener reopens — no explicit control is needed;
      fps/quality are startup flags (--net-fps/--net-bitrate),
      per-run changes are not required. Revisit only if headless
      remote control (no SSH) or M5 per-stream control ever
      demands it. RTSP phase B and NetSink (RTSP/WebRTC) are DESCOPED
      (owner decision 2026-10-28): not needed in the foreseeable
      future — the h264 TCP pull is the final preview transport;
      revisit only with a concrete remote-viewing requirement.
      FrameTap full-frame path removed the same day (owner decision):
      publish_full_frame (clean
      stream without overlays) deleted from the code — the
      overlayed model-input stream is the only wanted preview;
      FrameTap as a design element dies with it.
      net/ family (design, after the descoping — see above):
      NetEncoder (libavcodec h264_rkmpp wrapper — the installed
      self-built ffmpeg (8.1 line, libavcodec 62.28.103, tip
      129a3cf0d7) has rkmpp ENCODERS, the hardware does the encode,
      zero-CPU like the MPP decoder; the tree carries the rkmpp EOS
      fixes also upstreamed as PR #273; qp_*/intra-refresh available,
      rc_mode REJECTED by the live encoder — see Platform
      NetSink (RTSP/WebRTC) — descoped 2026-10-28 with RTSP phase B
      (owner decision, see above; not a live plan). H.264 packets =
      second legal memcpy alongside the MJPEG packet.
      Preview path is strictly opt-in, never part of
      the standard run or benchmarks. Priority vs M4: owner decision
      — if netsend is a working tool, it goes first (it is also the
      debug tool for M4).
      History (dated facts kept as recorded):
      - 2026-10-24: phase A landed as raw NUT over TCP:1234, not the
        designed h264 first (libavformat 62 from the self-built
        ffmpeg 8.1, ABI pin, see Platform constraints); the tap was
        the designed full-frame tap was publish_full_frame
        (removed 2026-10-28 with the FrameTap descoping, see above).
      - 2026-10-25: live streaming verified (continuous frames with
        YOLO overlays; the earlier "first frame only" issue gone).
        h264_rkmpp MPEG-TS/UDP landed as the tensor path: the
        encoder publishes the NPU input tensor AFTER GPU box drawing
        (draw-fence poll before RGA), crops the visible letterbox band
        (srect y=[140,500) for the 640x640 tensor) and converts
        RGB888->NV12 1:1 into its own ping-pong NV12 dma-bufs. The
        stream is the band at its NATIVE size — 640x360 for 1280x720,
        no upscale (the x2 upscale was tried first and dropped: adds
        no detail, only bitrate and RGA work). Boxes ride into the
        stream for free. Muxer patch for late joiners: mpegts_flags
        +pat_pmt_at_frames, pat_period 0.1, sdt_period 0.25.
        Measured: 600-frame run, steady ~48 ms sum (~20.8 FPS
        evening), the encode does not stall the pipeline.
      - Viewer-side lesson (UDP push, Windows): ffplay udp://@:1234
        binds :: (IPv6) and receives nothing — bind
        udp://0.0.0.0:1234 explicitly (verified via
        Get-NetUDPEndpoint). The 2026-10-25 UDP latency closure is
        M3.9, player-side.
      - A benign-looking mpp_buffer ref_count warning appears at
        shutdown on the rkmpp-encoder path (the init assert itself was
        root-caused and fixed 2026-10-27, see Platform
        constraints) — watch on future runs. Seen again on the
        2026-10-26 pull run, still benign.
        Update 2026-10-28 (closure): h264 pull run against the rebuilt
        binary (installed .103, EOS fixes f5f6da6337 + 129a3cf0d7),
        1200 frames, viewer attached from connect through shutdown —
        stderr clean, no ref_count warning at teardown (the final
        mpp_buffer line is the normal service_deinit info, not the
        warning). Note closed: the symptom was the encoder
        teardown/flush path, covered by the EOS fixes (PR #273).
        tools/test_rkmpp_flush.c removed with the closure.
      - 2026-10-28: encoder input switched to zero-copy DRM_PRIME
        (801ba85): the ping-pong NV12 dma-bufs are allocated with a
        16-aligned ver stride (360 -> 368 rows) and handed to
        h264_rkmpp as AVDRMFrameDescriptors (chroma plane at
        w * vstride); the wrapper imports the fd (mpp_buffer_import)
        — zero CPU pixel copies, the per-frame system-memory copy
        path is retired. Verified live (1200 frames, viewer
        attached): MPP accepts the descriptor geometry ("stride
        [640:368]"), enc flat 1.0-1.4 ms (the 1.8 -> ~15 ms drift
        is gone), colors clean on the stream. The earlier "missing
        hw_context" review hypothesis was disproven by reading
        rkmppenc.c — see Platform constraints, rkmpp encoder input.
- [x] M3.6 (closed negative): box drawing via RGA — planned, probed
      live with yolov5 detections, retired. The open check (a) turned
      out fatal: RGA fill is not routable on this board (see Platform
      constraints) — every imrectangle call failed with "invalid
      function policy", no boxes were drawn, and a standalone probe
      crashed the kernel. Never re-open. The useful findings survive:
      the 90-degree rotation myth was the improcess usage arg
      (IM_HAL_TRANSFORM_ROT_90), and box rects must be clipped to
      [lb_top, lb_top+lb_h) — the GPU path inherits both lessons.
      CPU drawing returned as the fallback (draw_boxes_cpu,
      pipeline.cpp).

- [x] M3.7: GPU box drawing (Mali G610, EGL/GBM) straight into the
      NPU tensor. The rknn tensor dma-buf is imported as an EGLImage
      (attachTensor); quads drawn with one draw call per frame; native
      fence sync; the h264 path orders GPU/RGA by poll() on the exported
      draw fence (this RGA driver ignores improcess acquire fences),
      syncCpu() remains for the CPU reader only. Y-orientation:
      GL origin (bottom-left) IS memory row 0 — no flip; the earlier
      mirrored boxes came from a probe that sat at the center row,
      which is symmetric under a flip and could not detect it (a live
      frame did). Outline 1 px (owner's pick, visually verified on
      live frames). Colors/geometry identical to the CPU fallback
      (scale = model_w/cam_w, lb_top, person red 255/64/64, chair
      green 64/255/64, others yellow). A/B, 240 frames each, viewer
      connected, steady blocks: CPU 38.0 ms (~26.3 FPS) vs GPU
      38.3-38.6 ms (~26.0) — timing parity; GPU wins on outline
      quality and zero CPU writes into the tensor. mpp_buffer init
      assert seen in every run incl. this milestone — confirmed
      benign. Update 2026-10-27: root-caused and closed — see
      Platform constraints (mpp_buffer_group caller).

- [x] M3.8: camera disconnect recovery (USB replug). dequeue() classifies
      the failure: ENODEV/EIO are recoverable, any other errno is a hard
      error ending the run with exit code 1 (run() -> -1, main() maps it).
      Recovery: up to 20 attempts x 1 s (the original 10x1 s window was
      too short — observed USB re-enumeration on this board took ~13 s);
      each attempt re-runs the full open+verify path (S_FMT, G_FMT
      check, REQBUFS, mmap, QBUF, STREAMON). Node discovery is outside
      the camera class (the mutable-node design was rejected and
      refactored away in the same revision): find_usb_capture_node()
      prefers the configured path, falls back to USB-filtered numeric
      /dev/video* candidates with a trial setup; the pipeline tracks
      cam_node_ and adopts a re-enumerated node (verified live: video20
      -> video21, recovery on attempt 13, stream continued, viewer
      re-synced via repeated PAT/PMT without a restart). Permanent
      unplug: gives up after 20 attempts, exit code 1 — verified. Known
      cosmetic (owner's call, left as is): the "scanning /dev/video*"
      line prints on every attempt. The no-heap constraint is a
      project rule now (see Rules of
      the project); the refactor record is closed.
      Post-recovery artifacts fixed (2026-10-25): a dying camera can
      emit a zero-size buffer and mid-stream JPEG fragments; MPP may
      decode a fragment "successfully" into garbage that RGA stretches
      into the NPU tensor, so YOLO hallucinated phantom boxes on the
      stream right after a replug. Fixes: zero-size frames skip the
      decode (pipeline), a 3-frame warm-up discard follows reconnect
      (pipeline, c870455), and the decoder rejects any packet without
      SOI as defense in depth (f0ff5bb). Verified live (video20 ->
      video21, recovery on attempt 9, ~2 s): clean stream after
      reconnect, no phantom boxes, recognition unchanged; the SOI
      guard did not trigger (the driver resumed with full frames).
      Startup fallback added (2026-10-25, 1f851e6): a configured node
      missing at launch (ENOENT, e.g. after a replug re-enumeration
      before the app starts) no longer aborts the run — the pipeline
      falls back to find_usb_capture_node() and adopts the winner
      (verified live: --device /dev/video20 with the camera
      re-enumerated on video21 -> "discovery adopted /dev/video21",
      normal start, detections live). Non-ENOENT open failures (busy,
      permissions) stay fatal: a silently substituted source is worse
      than a loud one.

- [x] M3.9: preview latency — closed 2026-10-25, player-side. Symptom:
      the UDP/MPEG-TS H.264 preview on the PC lagged reality by ~5 s
      while the pipeline itself runs ~48 ms end to end (cam ~17-28 +
      infer ~29) — the delay is transport/player-side, not the
      pipeline. Cause: ffplay defaults (analyzeduration 5000000 us = 5 s
      of probing plus input buffering; the player starts 5 s behind and
      keeps a buffer). Fix: the reference viewer invocation is now
      ffplay -fflags nobuffer -flags low_delay -framedrop -probesize 32
      -analyzeduration 0 -vf setpts=0 -f mpegts udp://0.0.0.0:1234
      (confirmed live: lag dropped drastically, hand-wave test). No code
      changes. Update 2026-10-26: with the h264 TCP pull the same
      no-buffering principles apply; the pull viewer command
      (ffplay -fflags nobuffer tcp://<board-ip>:1234) needs no input
      format flags — ffplay autodetects MPEG-TS. Follow-up if backlog
      ever accumulates: source pts from
      camera wallclock instead of the fixed 3600-tick step (21-26 real
      FPS vs 25 declared); muxer omit_video_pes_length stays untouched.
      2026-10-31, from the mpv viewer session: the next latency front is
      the board encoder — GOP 25 frames or shorter (start and post-drop
      recovery are keyframe-bound), B-frames off, UDP/RTP instead of TCP
      (head-of-line blocking); see the Viewer section.

- [x] M3.10: detection smoothing — closed 2026-10-26. BoxSmoother
      (include/box_smoother.hpp): EMA on box coords (alpha=0.5,
      effective window ~2-3 frames), greedy IoU>0.3 + class match,
      miss<=2 decay, fixed 32-slot array, zero heap. Per-frame CSV
      via --box-log: raw jitter ~4 px per object -> ~2.5 px smoothed
      (~1.5-1.7x); timing unchanged (26.2 FPS steady). Earlier ~17 px
      estimate superseded: it counted low-conf flicker
      (bottle<->vase flips, n=46 vs n=300).
- [x] Refactor audit (owner-requested, closed 2026-10-27): a full pass
      over the audit checklist, all findings fixed and verified live.
      - det_color -> switch (a469e06).
      - GpuBoxDrawer: the last raw owning pointer (pimpl State*) removed,
        State by value in the header; make_current/make_program/draw_quad
        became member functions (State is private — anon-namespace free
        functions lost access); zero new/delete left (185b843).
      - done() leak fix: GBM device destroy + gbm_fd close + dlclose now
        unconditional (init may fail after dlopen; the old early return
        on dpy==NO_DISPLAY leaked all three on that path) (ed523cf).
      - DetList/GpuBoxList: PodList<T, N> template through the
        postprocess -> smoother -> pipeline chain; per-frame detection
        buffers are preallocated Pipeline members; signatures switched
        to out-parameters (f501ea1).
      - Regression caught on the live run (see the buffer contract rule
        above): fixed in 705636b.
      - Heap scan after the pass: the only real allocations are
        rknn_engine out_attrs_/out_mems_ (init-time); zero heap traffic
        in the steady-state per-frame path.
      - Live verification (3000 frames, h264 pull, steady): det count
        2-6 per frame, person tracked in and out, sum ~48 ms ~20.8 FPS —
        parity with the M3.7-era baseline. Box jitter visibly reduced —
        a side effect: the 32-slot tracker no longer churns on ~30
        duplicate copies of one object per frame (the old accumulating
        DetList fed them to BoxSmoother every frame).
- [ ] M6: toolchain hygiene (recorded 2026-10-27 by owner, restored
      from session notes 2026-10-28): (a) clang-format — one enforced
      style across the tree; the recurring whitespace-only artifacts
      (empty lines sneaking into headers, see the pipeline.hpp restore
      of 2026-10-28) are the symptom this closes; (b) gtest unit
      tests — GoogleTest against the pure-CPU logic that today is
      verified only by live runs: yolo postprocess decode (grid/anchor
      math, NMS, letterbox mapping T) and BoxSmoother (EMA, IoU
      matching, miss decay). Hardware stages (MPP/RGA/RKNN/encoder)
      stay out of unit scope — live-run territory. Open sub-questions:
      the exact .clang-format profile (reference files: gpu_box_drawer
      style per Rules) and the first covered module.
- [ ] M4: pipelining (async RGA), NPU/DDR governor = performance. Only pays
      off once the source delivers more than 30 FPS (infer headroom analysis:
      even a hypothetical 10-15 ms model@640x640 would fit the frame budget
      without pipelining while the source stays at 20-30 FPS — the measured
      yolov8n is 65 ms (M2-alt), so the figure is a target for a future
      lighter head, not a measurement).

- [ ] M5: multi-camera (proposal, owner decision 2026-10-26): 3x USB
      cameras from different room angles. Platform budget fits: 3x MJPEG
      ~3 MB/s on the shared USB2 half, MPP decode trivial. RGA needs core
      spreading — everything is pinned to RGA3 core0 today (imconfig);
      three single-pass 720p->640x640 jobs want RGA3 c0/c1 + RGA2E or the
      scheduler. NPU fits: ~3x current load is ~50% per core; pure infer
      is 15.4 ms/frame regardless of stream count — scheduling strategy
      (round-robin frames vs 3 RKNN contexts) is an open question.
      Update 2026-10-30: the one-model-one-core finding (Platform
      constraints) settles the scheduling question in favor of 3
      RKNN contexts with masks 1/2/4 — a single context never uses
      more than one core; single-context infer reads ~30 ms, so the
      3-context ceiling is ~90 FPS inference-side, camera pacing
      permitting.
      Pipeline refactor: an array of cam+dec+rga chains, each with its
      own dma-buf/tensor pool, plus a round-robin dispatcher; per-cam
      BoxSmoother. Fusion is NOT volumetric on this NPU (BEV/3D
      transformers are out of reach at 22 ms/infer): path A is one-time
      geometric calibration (chessboard or 4+ known floor points per
      camera) -> homography cam->floor plan -> box-bottom projection
      gives per-object (x, y) in meters; cross-camera track matching by
      world proximity + class; EMA reused as-is. Cheap first step:
      single-camera floor projection + minimap overlay in the stream.
      Known traps: per-camera night exposure differs -> per-cam
      conf/gamma tuning; 10 cm of calibration error is a permanent
      10 cm object error. Portability: moving to another room = geometry
      recalibration per camera (lighting-independent); model INT8
      recalibration only if the lighting regime differs materially
      (v1->v2 precedent), conf band re-checked from logs on day one.
