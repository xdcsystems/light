# light_ai — zero-copy video analytics on RK3588

Pipeline: V4L2 (MJPEG) -> MPP decoder (YUV422SP, dma-buf) -> RGA (im2d resize
straight into the dma-buf tensor of the NPU) -> RKNN (zero-copy) -> CPU
post-processing.

## Rules of the project
- No GStreamer
- No OpenCV
- A frame exists only as a dma-buf; pixel copies through the CPU are forbidden.
  The only permitted copies are the compressed MJPEG frame (~25 KB)
  into the decoder packet buffer and the compressed h264 packets out
  of the encoder (both are compressed byte streams, never pixels).
- I/O is iostream-only: no printf/sprintf/snprintf anywhere. All diagnostics
  go through std::cout / std::cerr.
- Include order: system/service headers first (angle brackets), the
  project's own headers last (quotes). Reference: gpu_box_drawer.cpp.
- Header layout: within each visibility section (public/private),
  member functions and data members live in SEPARATE blocks — functions
  first, then a fresh visibility label for the data. Reference:
  camera_v4l2.hpp (xioctl in its own private: block, data in the next).
- Dynamic memory allocation is forbidden: no new/delete, no
  std::unique_ptr/shared_ptr anywhere, no heap in recovery
  paths. Data lives in fixed-size members; mutable identity
  (e.g. the current camera node) lives with the owning layer.
  std::vector/std::string are allowed as RAII wrappers;
  manual heap ownership (raw new/delete, bare owning
  pointers) is forbidden. Reference: camera_v4l2.hpp +
  pipeline.cpp + pod_list.hpp (fixed-capacity lists in the detection
  path).
- Statics are forbidden: no static data members, no static
  member functions, no function-local statics. Shared state
  lives with the owning layer as a regular member; file-local
  helpers become free functions inside an anonymous namespace
  in the layer's .cpp. Compile-time class constants use an
  unscoped enum (camera_v4l2.hpp kBufCount); references:
  camera_v4l2.cpp / gpu_box_drawer.cpp / net_preview.cpp
  (anon-namespace helpers), box_smoother.h (plain member).
  tools/ is exempt (disposable probes).
- No virtual functions, no inheritance hierarchies: zero vtables, zero
  dynamic polymorphism. Layer boundaries are the composition of concrete
  members (pipeline.hpp), not an interface contract. Static polymorphism
  (templates) is the sanctioned replacement where one implementation
  serves several types: PodList<T, N> (include/pod_list.hpp) — one
  fixed-capacity list, three instantiations (DetList in
  yolo_postprocess.hpp, GpuBoxList in gpu_box_drawer.hpp, TrajList in
  tracker.hpp), zero heap.
- const by default: member functions that do not mutate state are
  const; parameters are passed by const-ref unless the callee owns a
  copy. Out-parameters are plain references filled by the callee
  (yolo_postprocess fills DetList& out — no return-value vectors in
  the frame path).
- Branch tables over if-chains: a multi-way dispatch on a fixed set
  becomes a switch or a constexpr table, not a ladder. Reference:
  det_color in box_mapper.cpp (switch).
- Preallocated buffer contract: a buffer that lives across frames
  (a member, not a local) is NEVER implicitly empty — the callee owns
  the reset and clears it at entry (DetList out.clear() in
  yolo_postprocess). Converting a returned std::vector into a
  preallocated member silently loses the "fresh vector each call"
  semantics; the 2026-10-27 live run caught exactly that (detections
  accumulated to the 128 cap, the box set froze, person went
  undetected — 705636b). A heap scan cannot catch this class of bug;
  only a live visual run can.

### Comment style
- Comments are structured, not prose: a header line ending with `:`,
  options/cases one per line, continuation lines indented under their item.
- Max line length: 140 characters (enforced by `.clang-format` `ColumnLimit: 140`).
- Do not duplicate rules stated in this README in code comments;
  code comments reference ("see README") or explain local specifics only.
- A comment describing a mode/value must be verified by an actual run
  (example: pool=2 was documented while --pool rejected it, and the
  clamp silently allowed it).
  
## Layers
- CameraV4L2 : MJPEG capture, mmap buffers, DQBUF/QBUF. At startup S_FMT
               requests the preferred 1280x960 MJPEG mode and adopts the
               dimensions reported by G_FMT; on a mid-run reconnect G_FMT
               is verified against the locked session dimensions so a
               replug never changes resolution mid-stream. The node is NOT instance
               state: setup_device(dev)/reconnect(dev) take the path as
               an argument, teardown() is public (no dynamic allocation
               anywhere in the recovery path). Disconnect recovery:
               ENODEV/EIO from dequeue() are recoverable, other errnos
               are fatal (exit 1 — silent exit 0 on camera death is
               gone); each attempt re-runs the full open+verify path,
               nothing is trusted blindly across a disappearance. Node
               discovery lives outside the class (find_usb_capture_node
               in camera_v4l2.cpp): the configured path is preferred,
               otherwise numeric /dev/video* candidates are USB-filtered
               (rkcif/rkisp platform nodes rejected before any S_FMT)
               and trial-opened; the pipeline tracks each source node in `SourceSlot::node` (`srcs_`) and adopts a re-enumerated one transparently.
               A mid-run recoverable failure (ENODEV/EIO) flips the
               slot to absent; probe_loop() re-attaches on its own
               cadence (recover() for a detached slot, setup_device for
               a fresh one). A permanent unplug no longer ends the run:
               the remaining sources keep streaming.
               Robustness (2026-11-02, P1-5/P1-7): every setup_device
               error path after open() calls teardown() itself — a
               failed setup leaks neither fd nor mappings; the usb
               node check requires V4L2_CAP_STREAMING (a read()-only
               node is rejected with a clear diagnosis instead of a
               confusing QUERYBUF failure); REQBUFS returning fewer
               than 3 buffers warns loudly (ping-pong depth lost).
               teardown() skips empty slots instead of breaking on
               the first one. The consecutive-corrupt-buffer streak
               (BUF_FLAG_ERROR) is a class member: it survives
               dequeue() calls and is reset only by a clean frame or
               by reconnect() after a successful STREAMON. dequeue()
               drains any newer ready V4L2 buffers (poll_ready(0)) so
               the capture ring never accumulates stale-frame lag.
- NetMjpegSource : HTTP multipart/x-mixed-replace MJPEG pull source
               (--net-src http://host:port/path; repeatable alongside
               --device in Config::sources for M5 multi-camera). Non-blocking TCP connect + SO_RCVTIMEO
               (5 s), structural JPEG framing into a 4x1 MiB mmap pool
               (skips header/EXIF segments by length up to SOS, extracts
               frame dimensions from SOFn, verifies trailing EOI); primes
               frame 0 in setup_device() so stream geometry is known at
               startup without --w/--h, drains init-time TCP backlog in
               start(), buffers into a 256 KiB socket window and verifies
               that the full multipart payload (Content-Length or closing
               boundary) is resident before reporting ready in
               poll_part_ready(timeout_ms), skips burst-queued stale frames
               in read_latest_part(), and drops incomplete/corrupt JPEGs
               with EAGAIN before decode. Reconnects through the same
               recovery loop as CameraV4L2.
- MppDecoder : MJPEG -> YUV422SP decode into an external dma-buf pool
               (adaptive, 2026-11-06: a 4:2:0 source decodes to
               YUV420SP instead — the format is picked from the JPEG
               SOFn Y/Cb/Cr sampling factors; the x2 pool covers either),
               returns the frame fd. Sized from the active source
               (V4L2 G_FMT or JPEG SOFn), not from CLI flags.
               Recovery (2026-11-02, P0-1): the packet is drained
               per call on every path (error and success) — MPP no
               longer holds a reference across the decode gap; on
               -6/-7 (output poll/dequeue failure) mpi->reset()
               runs first, un-wedging the input path (was: permanent
               -3 after a single -6).
               Also hands out an aux dma-buf from the same pool for the
               multi-pass RGA path.
- RgaResizer : im2d resize by fd (wrapbuffer_fd + improcess). Two-pass path
               when the per-axis scale ratio exceeds the RGA3 limit (1/8),
               guarded against aux_fd < 0. All jobs are pinned to RGA3 core0
               via imconfig at startup.
               Letterbox geometry is a per-call LbGeom value carried
               through dispatch into the pipeline slot (2026-11-02,
               P1-1) — the shared mutable members are gone; under
               --pool 3 they were a cross-thread race.
- RknnEngine : init/query failures throw (fail loudly like CameraV4L2) —
               main() reports the real cause, no zombie objects. Options:
               rknn_create_mem + rknn_set_io_mem; the input tensor fd is
               handed to RGA as the destination (resize writes directly into
               the NPU tensor). N output tensors (std::vector of
               rknn_tensor_mem), each sized exactly (attr.size). Output type
               is read from the model; for int8-quantized models the attr is
               rewritten to FLOAT16 (buffer becomes n_elems * 2) so the
               runtime performs per-channel dequantization into our
               zero-copy buffer — postprocess stays untouched.
               Non-copyable by design (2026-11-02, P0-3): copying
               would corrupt rknn contexts on reallocation; the move
               ctor transfers ctx/tensor mems and leaves a hollow
               shell (ctx_=0), keeping vector::reserve legal.
- Pipeline   : both source geometry (V4L2 G_FMT or JPEG SOFn) and model
               input geometry (model input_attr) are read from the source
               and model themselves, not from CLI flags (--w/--h and
               --mw/--mh are abolished). Aux allocation happens after
               model geometry is known (constructor-order bug fixed; a
               one-line "aux not needed" log confirms the single-pass path).
               Concurrency & shutdown (2026-11-02): publish_oldest()
               waits at most 5 s with quit_ in the predicate — on
               timeout the slot is marked drop and freed by its
               worker after the in-flight rknn_run completes (never
               while the NPU reads the tensor). SIGINT/SIGTERM set a
               stop flag: run() exits through the normal path (ring
               drain, TOTAL summary, encoder flush, camera
               teardown) instead of being killed outright.
- BoxSmoother (M3.10): 32-slot class-aware greedy IoU (> 0.30) + EMA
               (alpha = 0.5) box stabilizer; decays unmatched tracks
               after 2 misses; optional raw-vs-smoothed CSV via
               --box-log.
- Tracker (M3.10-b / M3.11): 8-slot constant-velocity Kalman tracker
               (2-state filter per axis, velocity clamp 500 px/s,
               0.90x per-miss velocity decay, size-consistency gate,
               spawn-dedupe gate, global greedy IoU association) with a 300-point trajectory ring
               per track; recent_points() feeds the trail renderer.
- BoxMapper  : fill_gpu_boxes() maps smoothed detections and confirmed
               trajectory trails into the 128-box GpuBoxList VBO
               budget, clamped to the exact LbGeom band [left,
               left+w] x [top, top+h].
- CalDumper / ConsoleReporter / Timings : calibration PPM dumper
               (--dump-cal single frame, --cal N --cal-interval K
               series under --cal-dir, DMA-synced CPU read),
               throttled detection/track console + CSV reporter, and
               per-stage FrameTrace accumulator.
- CliOptions / OptionRegistry / ConfigFileLoader (config/): fixed
               21-entry std::array option registry with table-driven
               setter callbacks (OptionRegistry::apply), shared by the
               CLI parser and the inih-backed INI loader (--config,
               applied before CLI overrides) — zero if/else ladders.
               Repeatable --device and --net-src accumulate into
               Config::sources (std::vector<SourceEndpoint>) for M5
               hybrid USB + HTTP MJPEG setups; CLI source flags override
               INI sources, and /dev/video20 is defaulted only when no
               sources are specified.
- TcpPullServer : RAII single-viewer TCP pull transport (listener socket,
               poll-based accept, TCP_NODELAY / SO_SNDBUF / SO_SNDTIMEO,
               and custom 32 KiB AVIOContext) shared by NetEncoder and
               NetPreview.

- OverlaySink : owns the overlay rendering and network preview output
               path (GpuBoxDrawer + draw_boxes_cpu fallback, NetEncoder
               H.264 and NetPreview raw NUT), keeping Pipeline focused
               on source capture, NPU pool scheduling and per-source state.
- GpuBoxDrawer (M3.7): box drawing on Mali G610 (EGL/GBM) straight
               into the NPU input tensor. The tensor dma-buf is imported
               as an EGLImage; one draw call per frame; native fence
               sync; falls back to CPU drawing (draw_boxes_cpu in
               overlay_sink.cpp) if the tensor pitch is not usable or init
               fails. --cpu-draw forces the CPU path (A/B baseline).
               attach_slot() fixes the shared geometry on the first
               attach; any later attach with different w/h/stride
               fails loudly with partial attach cleanup (2026-11-02,
               P0-5).
- NetPreview : opt-in preview stream (--net): TCP server on port 1234,
               sends the model-input frame (640x640 letterboxed rgb24)
               as rawvideo inside a NUT container, paced by --net-fps
               (default 25). VLC plays it directly (tcp://IP:1234) —
               no decoder needed on the viewer side. One viewer at a
               time. Status 2026-10-26: diagnostic viewer — the daily
               viewer is the h264 TCP pull (NetEncoder); raw NUT runs
               ~246 Mbps (640x640 rgb24 @ 25 fps) vs ~12 Mbps h264.
- NetEncoder : h264 preview (--net --net-h264): h264_rkmpp hardware
               encode (self-built ffmpeg 8.1, installed libavcodec
               62.28.103 with the rkmpp EOS fixes, 2026-10-28),
               MPEG-TS. Transport:
               TCP pull by default (empty --net-host — the encoder
               listens on --net-port and the viewer connects to the
               board's LAN IP, so the run command carries no
               addresses); --net-host <pc-ip> keeps the legacy UDP
               push to that destination. The source is the NPU tensor
               AFTER box drawing: RGA crops the visible letterbox
               band (srect y=[lb_top, lb_top+lb_h)) and converts
               RGB888 -> NV12 1:1 into its own ping-pong MPP dma-bufs
               — no upscale, zero CPU pixels (the frames leave for the
               encoder as zero-copy DRM_PRIME descriptors, 2026-10-28,
               see the enc notes below). The stream size is the
               band's native size (640x480 for the default 1280x960 camera, 640x360 for 1280x720; 640x480 at the 1280x960 working mode since
               2026-10-30). One viewer at a time; no encode until the
               first viewer connects; a stalled viewer is dropped via
               SO_SNDTIMEO (15 s). Late joiners via repeated
               PAT/PMT/SDT.
               The NV12 dma-buf pool is three ring buffers deep (2026-11-02,
               P1-4): two could run out exactly at the 3-deep rkmpp
               task queue when VFR frames arrive in clumps after a
               network pause; the slot index advance is modulus-based.
               In M5 multi-camera mode (nsrc > 1) a 4th NV12 buffer holds
               a persistent composite multi-view canvas (1280x480 Side-by-Side
               2x1 for 2 sources, 1280x960 Grid 2x2 for 3 sources, 640x480 per
               tile): RGA3 updates the active source's tile and DMA-copies the
               composite canvas into the current ring slot (0.9-1.3 ms enc).
               2026-11-01: board-side stream fixes verified live —
               TCP_NODELAY + 2 MiB SO_SNDBUF on the accepted socket,
               max_delay=0, flush_packets=1,
               +pat_pmt_at_frames+resend_headers; --net-bitrate 3000
               confirmed by the MPP log ("set rc cbr bps [3000000:...]"),
               the viewer-side bitrate reads ~2.4 Mbps (CBR
               under-delivery on a static scene). Result: zero
               client-side "(Buffering)" per session in timed mode.
               2026-11-06: the accepted-socket pair was retuned in code —
               SO_SNDBUF 2 MiB (~10 s of jitter buffering at 1500 kbps),
               SO_SNDTIMEO 15 s (a dead viewer eventually releases the
               socket); the 2026-11-01 numbers above were updated to the
               current values.
               Client (pull, product path): ffplay -fflags nobuffer
               tcp://<board-ip>:1234
               Client (UDP push, legacy, low latency): ffplay -fflags
               nobuffer -flags low_delay -framedrop -probesize 32
               -analyzeduration 0 -vf setpts=0 -f mpegts
               udp://0.0.0.0:1234

## Platform constraints (learned the hard way, M2-prep)

**VFR encoder pitfall (M5 follow-source):** `ctx->framerate = 0/1`
reaches MPP as the auto marker (65535) — rc-fps degenerates, the CBR
per-frame budget collapses (~183 bits/frame, the stream dries to
~83 kbps) and the pts step becomes 1 tick. Follow-source must hand
rate control a nominal fps (25) while pts stay per-dequeue
(av_rescale µs -> 1/90000 ticks).

- MPP decoder: output frame buffers must be allocated with the height
  aligned to 16 (1080 -> 1088 rows). An exact-size allocation trips the
  `slot_size <= buffer_size` assertion before info_change is ever reported.
- RGA3 scales at most 1/8 per axis; 1920x1080 -> 224x224 is 8.57x and needs
  the two-pass path through a 960x540 intermediate buffer. When comparing
  the ratio, compare by multiplication: integer division masks the excess
  (1920/224 == 8, but the real ratio is 8.57).
- RGA2 cannot map physical memory above 4G: 1080p decoder dma-bufs land
  above 4G and the scheduler falls back to RGA2 (dmesg:
  "rga_mm: RGA_MMU unsupported memory larger than 4G!"), which fails.
  The pipeline is structurally RGA3-only (two-pass keeps every ratio
  within the limit) with imconfig as a belt-and-suspenders pin.
- Camera fps is exposure-dependent: ~30 FPS in daylight (measured at
  640x480, M1), ~20-24 FPS in the evening in every resolution alike
  (640x480, 1280x720, 1280x960, 1920x1080) — pre-YOLO numbers; the 720p
  yolov5s_relu_int8 steady state with a viewer attached is ~26 FPS
  (see the 720p section below), although the UVC descriptor advertises
  30/1 in all modes. Forcing exposure (exposure_absolute=33) does not lift
  the ceiling — Manual exposure is a no-op on this camera, and FPS
  pacing is a discrete firmware state (see the camera facts
  below). 1280x960 in daylight (2026-10-30): pacing held 20 FPS in
  the steady runs (600 frames in 30.6-30.9 s) — see the 960p
  performance section. 1080p in daylight: not yet measured.
- Camera is USB2-only BY SPEC (Canyon CNS-CWC5 = Sunplus 1bcf:2283,
  bcdUSB 2.00): always enumerates on the hub's USB2 half (480M),
  SuperSpeed half stays empty — normal. USB bandwidth is a non-issue
  at 720p MJPEG (~1 MB/s).
- FRAME PACING is a discrete FIRMWARE state, not addressable by any
  control (verified 2026-10-25 evening, Dev 10 + fresh Dev 11):
  the camera delivers one of {20, 25, 30} FPS and switches between
  levels across successive streams with NO control changes. A replug
  resets pacing to 20. UVC exposure controls are accepted, persisted
  (exposure_absolute=33 survived a full replug) and read back OK, but
  change NOTHING: the S1-S4 matrix (auto/manual x successive streams)
  gave 19.3-19.4 FPS in all four runs, detections and scene brightness
  identical. Mid-stream SET_CUR on exposure_absolute stalls with
  EPIPE when auto mode makes the control inactive (spec-correct).
  get-parm always reports the REQUESTED rate (30.000) regardless of
  what is actually delivered — do not trust it as a status read.
  Wall-clock proof that 30 FPS is deliverable: 900 frames in 30.5 s
  (29.5 real FPS) when the camera is in its 30-FPS state — the SoC is
  not the bottleneck, cam pacing is. Detections identical at 20/25/30
  FPS: AE holds exposure constant, pacing varies independently.
  30-FPS state observed ONCE (21:08); full-disturbance prelude replay
  (SIGTERM mid-stream + mid-stream SET_CUR x2 + S_PARM) did NOT
  reproduce it — trigger unknown, treat 20 FPS as the reliable rate.
- PURE INFER, camera-independent (tools/bench_npu, 2026-10-25,
  performance governors): 15.4 ms/frame on 3 NPU cores, 18.1 on
  2, 22.0 on 1 — day/night identical, cores=7 retained. Core
  scaling is poor (1.43x on 3x hardware): inter-core sync eats
  most of the theoretical gain.
  The [timing] 'infer' figure is NOT pure compute:
  infer_shown = pure_infer + stall, where stall = frame interval
  minus (cam + dec + rga). All three pacing levels fit exactly:
  30 FPS -> 21.2 shown (15.4 + 5.8 stall), 25 -> 24.6 (15.4 + 9.2),
  20 -> 30.6 (15.4 + 15.2). Serial work sum is ~34.5 ms/frame ->
  SoC ceiling ~29 FPS even with a perfect 30-FPS camera (~6 ms
  slack at 30). NPU load during a 20-FPS run: ~14% per core
  (rknpu load) — the NPU idles 86% of the time.
  'Forcing exposure does not lift the ceiling' was right in result and
  now also right in mechanism: Manual exposure simply does not affect
  the camera; the earlier day ~30 vs evening ~20 attribution is
  superseded — pacing level, not lighting, was the variable.
- Resize is letterboxed: uniform scale by width (s = model_w / cam_w), the
  visible band occupies rows [lb_top, lb_top + lb_h) of the model input, the
  rest stays gray (128, memset once; RGA writes only the drect band).
  Letterbox geometry is derived from the model, not hard-coded. Examples:
  1280x720 -> 224x224: s = 7/40, lb_h = 126, lb_top = 49;
  1280x720 -> 640x640: s = 1/2,  lb_h = 360, lb_top = 140.
  1280x960 -> 640x640: s = 1/2,  lb_h = 480, lb_top = 80.
  Detection mapping T (band-based, axial): x_scene = (x_det - lb_left) *
  cam_w / lb_w, y_scene = (y_det - lb_top) * cam_h / lb_h, where
  (lb_left, lb_top, lb_w, lb_h) is the letterbox band geometry — a per-frame `LbGeom` returned by `RgaResizer::resize`, carried in `Slot::lb` and shared by the RGA crop,
  the encoder crop and this scale. Detections on the bars (outside the
  band) are filtered. (The pre-M3.11 note "x_det * cam_w / model_w"
  described only the landscape band, where lb_left = 0 and
  lb_w = model_w.)
- The calibration dump file is created by whatever user runs the binary;
  a dump left by a root run cannot be overwritten by a normal user run
  ("calibration dump failed"). Clean it up between sudo and non-sudo runs.
- NPU multi-core (verified 2026-10-30; runtime version confirmed via
  `strings /lib/librknnrt.so` -> "librknnrt version: 2.5.0"): a single
  RKNN context is NEVER split across cores. Under --core 1 debugfs
  shows only Core0 busy (~30%), Core1/2 idle; under --core 7 all three
  read ~11-13% — the same total work seen through core rotation /
  counter smoothing, NOT layer parallelism. Mask A/B (600 frames,
  steady): --core 1/3/7 -> infer 32.5/32.6/30.0 ms — the mask buys
  <=2.5 ms, not a 3x split. Version triples in build logs (rga_api
  1.10.6 etc.) belong to other components, not the RKNN runtime.
  Multi-core throughput comes from parallel contexts, one per core
  (the M5 pattern); bench_npu's 15.4 ms@012 is consistent with that
  reading — reconciled 2026-11-01: 15.67 ms @7 reproduces 15.4 @012 within 2% (see docs/state.md registry).
- yolov5 .rknn outputs are native fp16 (float16, NCHW) — no CPU-side
  dequantization needed; the lite-runtime wraps them as float32.
- int8-quantized yolov5s_relu_int8.rknn: built with rknn-toolkit2 2.5.0
  on Colab. v1: calibrated on 50 night frames (--cal 50 --cal-interval 4);
  v2: recalibrated on dataset_mix (25 day + 25 night frames) —
  see docs/state.md (INT8 vs FP32 validation). v3 (night A/B done, day A/B vs fp16 done 2026-11-01; superseded as the
  working model by int8_mmse, 2026-10-30):
  recalibrated on 60 fresh day-4 daytime frames with mean=0/std=255 ->
  canonical input scale 1/255 (0.00392157) — see M2.4.
  int8_mmse (superseded as the working model by int8_mmse_v5,
  2026-11-01 — see docs/state.md, INT8 build history): mmse-quantized on
  dataset_mix (the same 50-frame set as v2); input zp=-128,
  scale=0.00436174 (fitted, same as v2 — raw RGB byte writes remain
  correct). Daytime steady session (1280x960, conf 0.25): infer
  identical to v3 (~29.5-30.3 ms, one NPU core either way), but
  detections steadier — cup and dining table present in nearly every
  60-frame block vs episodic on v3; working conf 0.25. Night A/B for
  mmse: done 2026-10-30 (M2.6 closed, EQUAL vs v4;
  see docs/state.md).
  v4 (2026-10-30): mmse on the fresh 960p daytime dump (dataset_day3,
  50 of 100 frames); day, dusk and night A/Bs vs v3 — equal (see docs/state.md INT8 build history; night includes one isolated lost detection at
  n2_cal_0040).
  Day A/B vs fp16 (2026-11-01): CLOSED, four-way
  (v3/mmse/v4/fp16) on the restored 960p scene: v3 = the
  fidelity line, mmse/v4 = the recall line (cup boost,
  bowl/vase cost); full record in docs/state.md.
  MmseQuant2 is memory-hungry: 50 samples fit in 12 GB, 200 OOM. Outputs
  are int8 (per-channel scale/zp) — RknnEngine requests fp16 conversion
  via set_io_mem, the runtime dequantizes into the zero-copy buffer.
  Input (v2): zp=-128, scale=0.00436174 (not 1/255 — fitted from
  the calibration distribution; confirmed present in the v2 model
  file, byte-level, 2026-10-26). Input (v3): zp=-128, scale=1/255
  (0.00392157, canonical mean/std config) — raw RGB byte writes
  remain correct for both.
  (Calibration caveat: the cal set embeds the gray-128 letterbox
  padding — ~44% of pixels per frame in the 720p-era sets, ~25% in
  the 960p sets (640x480 band) — its stats partly come from the
  fill; see docs/state.md (dataset map).)
  A/B vs fp16 (daylight, 720p, conf 0.25): infer 43.1 -> 28.1 ms (~1.55x),
  classes and boxes match within a few px, conf drop ~0.02. Night A/B:
  v1 ALARM (superseded), v2 ALARM confirmed 2026-10-26 (systematic
  dconf -0.045, vase missed in every steady block), v3 closed —
  see M2.4.
- yolov5s_relu output semantics (probed on a real night frame,
  scripts/probe_yolo.py): ALL 255 channels are already post-sigmoid
  (range 0..1, box channels included) — the export applied the full
  yolov5 head. CPU-side decode is direct: xy = (v*2 - 0.5 + grid)*stride,
  wh = (v*2)^2 * anchor; strides 8/16/32 for grids 80x80/40x40/20x20.
  obj/cls are plain probabilities — no CPU sigmoid anywhere. Night-scene
  sanity: obj>0.5 in 2 cells at 40x40 and 3 cells at 20x20 (max obj 0.66),
  the 80x80 head stays quiet — the model reacts to the low-light scene.
- RKNNLite (python) feeds this model only with a 4-dim input tensor
  (img[None]); a 3-dim NHWC array raises "need 4dims input" — but a
  byte-array input given data_format='nhwc' does NOT raise: inference()
  silently returns None (no exception). Always check for None; never
  assume a zero-detection run means "no detections". Working recipes:
  raw uint8 + img[None] with NO data_format kwarg (tools/save_board_csv.py),
  or float input + data_format='nhwc'. inputs_pass_through, when used,
  also needs the 4-dim shape.
- Default resolution is 1280x960 (since 2026-10-30; 1280x720 earlier):
  full-frame 2 Mp, single-pass RGA into the 640x640 tensor (640x480
  letterbox band, lb_top = 80).

- RGA fill/rectangle is KNOWN-BROKEN on this board — do not use it for
  drawing. The kernel refuses fill jobs outright: RGA3 cores cannot fill
  at all, and RGA2E's policy matrix rejects rgb888 fill (dmesg:
  "rga_policy: invalid function policy", "failed to get scheduler"). A
  standalone imfill probe with a virtual-address buffer crashed the
  kernel itself ("Internal error: Oops: 96000005") and took the board
  down with a reboot — no further probing of RGA fill, ever. Drawing is
  GPU-drawer (combat path) or CPU (fallback). RGA stays in the pipeline
  only for resize (works: ~1 ms, RGA3) and the h264-path color
  conversion.
- NPU tensor as an EGLImage source: the tensor pitch must be acceptable
  to the GPU (checked by attachTensor). The real model tensor passes
  (640x640, stride 1920 = 640*3, 64-byte aligned); a pitch of 672
  failed earlier with 0x3003 — invalid pitch falls back to the CPU
  path automatically, never a hard error.

- Two ffmpeg stacks coexist on the board: the distro one (4.3, libav*
  .so.58 in /lib) and the self-built ffmpeg-rockchip (8.1, libav*
  .so.62 in /usr/local). Building light_ai against distro headers and
  running against distro libs was the root cause of the codecpar
  segfault and the NUT muxer's "No codec tag defined": the AVStream
  layout differs between branches, so writer and muxer disagreed on
  field offsets. The build is now pinned to /usr/local via
  PKG_CONFIG_LIBDIR in CMakeLists.txt (configure fails hard if
  libavformat.pc is not there), and the SONAME split (58 vs 62) makes
  a runtime mixup impossible by construction. Do NOT purge the distro
  libav* runtime — chromium/VLC/mpv depend on it and apt would cascade
  them away; the dev packages can stay too, PKG_CONFIG_LIBDIR hides
  them from cmake.
  Note (2026-10-28): inside the self-built stack TWO lines share the
  SONAME 62: the installed 8.1.3 line (local master 129a3cf0d7,
  libavcodec.so.62.28.103) and the nyanmisaka master + EOS-fix port
  (PR #273, branch xdc-rkmpp-eos-fixes-v2) — the latter is built
  in-tree for tests only. A stray `make install` from the wrong tree
  would silently flip the whole system between lines — never install
  in-tree builds without an explicit decision (see STATE-NOTES.md in
  ~/work/ffmpeg-rockchip). light_ai was rebuilt 2026-10-28 against
  the installed .103.
- rkmpp encoder input: hand hardware buffers as DRM_PRIME
  descriptors, never as system-memory pixels. The wrapper
  (rkmppenc.c) routes any non-DRM_PRIME frame through
  av_hwframe_get_buffer + av_hwframe_transfer_data — a full-frame
  CPU copy plus per-frame descriptor/map allocations on every
  encode call (2026-10-28: this was the monotonic enc 1.8 ->
  ~15 ms drift; fixed by the DRM_PRIME input, 801ba85). Two review
  myths closed by reading the wrapper: (a) "missing hw_context" is
  not a thing — rkmpp_encode_init creates its own RKMPP hwdevice
  and internal frames pool when none is set (log line "Created a
  RKMPP hardware device"); (b) with DRM_PRIME input encode_init
  returns BEFORE creating those pools and imports the caller fd
  via mpp_buffer_import — zero copies. The wrapper derives the
  vertical stride from the descriptor's chroma plane offset/pitch,
  so the 16-aligned ver stride must be set in BOTH the RGA
  destination and the descriptor (MPP log: stride [640:368]).
- h264_rkmpp rc_mode: the live encoder does NOT accept rc_mode (log line:
  "enc: rc_mode option not accepted"); the stream runs CBR at
  `--net-bitrate` (default 12000 kbps). Fine for the preview; revisit when
  bitrate control is actually needed.
- mpp_buffer_group_get requires a caller string: mpp_assert(caller)
  in mpp_buffer_group_init (mpp/base/mpp_buffer_impl.c:809,
  introduced by 147aec8a "Refactor C++ mpp_buffer to C", 2025-05)
  fires on NULL caller and PRINTS AND CONTINUES — the group is
  created either way, so the print is a lost-attribution
  diagnostic, not a failure. Our mpp_decoder (2 groups) and
  net_encoder (1) passed nullptr: three asserts per run. Fixed by
  passing __FUNCTION__ (2026-10-27): zero prints since, [timing]
  unchanged. The buffer manager lives in mpp/base/ in this branch,
  not osal/ — line numbers of foreign layouts do not apply.

## Measured performance (1920x1080 MJPEG, evening light, camera-bound)

| Stage | Average, ms |
|-------|-------------|
| cam (sensor, exposure-dependent) | 35.0-43.4 |
| dec (MPP MJPEG -> YUV422SP)      | 2.4       |
| rga (two-pass 1080p->224x224)    | 2.3       |
| infer (mobilenet_v1 @ 224x224)   | 2.0       |
| off-camera sum                   | ~6.7      |

Steady-state throughput is set by the sensor: ~20-24 FPS in the evening
(sum = 41.7-50.1 ms/frame). M1 reference (640x480, daylight, single-pass
RGA): 33.3 ms total, 30.0 FPS. Stability: 5000-frame continuous run at
1080p, zero frame loss, no dmesg RGA errors, timing flat to the last
second of the run.

## Measured performance (1280x720 MJPEG, yolov5s_relu @ 640x640, NPU-bound)

infer=46.9-47.7 ms dominates; cam drops to ~10 ms (the V4L2 queue absorbs
the NPU latency). Steady state ~16.8 FPS at 720p evening. The pipeline is
now NPU-bound: pipelining (M4) would no longer change FPS, only infer
headroom does. The 47 ms figure motivated the yolov8n alternative —
tested and closed negative in M2-alt below.

## Measured performance (1280x720 MJPEG, yolov5s_relu_int8 @ 640x640, steady, viewer connected)

Sum ~38 ms -> ~26 FPS steady. The largest term is infer (shown =
pure + cam pacing stall): ~22-26 ms in the 2026-10-26 daytime runs
(cam ~12-14 ms); the older "infer ~35-36 ms" figure belongs to a
run where cam stalled near zero — same 38 ms sum, different
breakdown. The overall run average (~17-18 FPS evening, ~23.8 FPS
daytime 2026-10-26) includes the viewer-less warmup where cam
blocks at ~23-53 ms (evening) / ~20-22 ms (daytime) — compare only
steady blocks with a viewer attached. CPU vs GPU box drawing is a timing parity (38.0 vs
38.3-38.6 ms); GPU wins on render quality (subpixel-accurate outlines)
and costs zero CPU writes into the NPU tensor.

enc= stage in [timing] (2026-10-27, M3.5 item 7): the report carries a
fifth term — the synchronous NetEncoder::publish in the pipeline thread
(draw-fence poll + RGA band crop RGB888->NV12 + h264 encode + mux + TCP
write; SO_SNDTIMEO caps a stalled viewer at 15 s). No viewer: enc=0.0
by construction (publish early-returns). Viewer attached, 300-frame
evening run: 1.8-1.9 ms steady (the first avg/60 block after a mid-run
connect is inflated — compare steady blocks only). The sum does NOT
grow by enc: at the camera's 20-FPS pacing the frame budget is fixed,
and the publish cost previously hid inside the pacing stall reported
as part of 'infer' — steady blocks show infer 30.9 -> 29.0-29.6 with
enc ~1.9 (cam 17.0 -> 16.2, within noise), sum unchanged (49.3-50.1
vs 49.6-49.8 baseline), FPS unchanged (~20.1-20.3). Decision (owner):
publish is measured whole, the fence-poll stays inside enc — the poll
wait is ~0 (single draw call per frame), and a future growth of enc
with a flat encode time would flag a GPU-path regression on its own.
Update 2026-10-28 (DRM_PRIME zero-copy input, 801ba85): the wrapper
system-memory input path did av_hwframe_get_buffer + a full-frame CPU
copy (av_hwframe_transfer_data, ~345 KB/frame) on every encode call;
with the input switched to DRM_PRIME descriptor import enc is flat
1.0-1.4 ms from the first block after connect (the "inflated first
block" note above no longer applies) across a 1200-frame run — the
1.8 -> ~15 ms in-run growth is gone, sum and FPS stay flat
(39.6-40.0 ms, ~25.1 FPS). MPP confirms the descriptor geometry on
the live log: "set prep cfg w:h [640:360] stride [640:368]"
(16-aligned ver stride) replaces the startup default [640:384] on
the first frame; colors verified clean on the stream (no chroma
misalignment).
Update 2026-11-03..10 (Batch B/C + P0 audit + M5 multi-view): prep_frame
(fence poll + RGA band crop) runs before mx_; send_frame moves encoded
packets into a 32-deep preallocated AVPacket ring buffer (av_packet_move_ref,
zero per-frame heap allocs, sized to absorb ~200 ms Wi-Fi stalls without
queue flushes) and serve() drains them into the muxer on the worker thread —
the pipeline thread never touches the network. With no viewer attached,
publish_oldest() skips both draw and encode.

## Measured performance (1280x960 MJPEG, yolov5s_relu_int8 @ 640x640, daytime, steady, viewer attached)

Current working mode (2026-10-30): full-frame 2 Mp, single-pass RGA
("aux not needed: 1280x960 fits single-pass into 640x640"), letterbox
band 640x480 (lb_top=80). Steady [timing] avg/60: cam 14.4-18.2,
dec 1.3-1.4, rga 1.1, infer 29.0-30.3 (identical for v3 and int8_mmse
within noise), enc 1.0-1.4 with a viewer attached; sum 49.5-50.2 ms
-> ~20.0 FPS steady; wall clock 600 frames in 30.6-30.9 s ->
19.4-19.6 FPS. Source-bound at 20-FPS pacing. Quality vs 720p:
4x pixels in the tensor (visible band 640x480 vs 640x360) — cup
(~0.25-0.32) and dining table detected in nearly every steady block,
3-5 detections per frame typical. Mask A/B and the single-core
finding: see Platform constraints (NPU multi-core) and M2.5. Lighter
models are REJECTED by owner decision 2026-10-30 (quality first) —
the model stays yolov5s @ 640x640. Follow-up: v4 recalibration on a
960p dump — M2.6, done 2026-10-30 (docs/state.md,
  INT8 build history).

## Scripts
- scripts/install_rknn.sh — installs rknn-toolkit-lite2 (aarch64, Python 3.9):
  downloads the wheel from GitHub Releases (tag rknn-toolkit-lite2-v2.5.0),
  verifies SHA256 and pip-installs it. The wheel is not tracked by git.
- scripts/set_governors.sh — pins CPU/NPU/DDR governors to
  performance (sudo). Run before any benchmark: the cores-012 infer
  figures are CPU-frequency dependent (see M2.5).
- scripts/test_inference.py — RKNNLite smoke test: shapes, non-negativity
  (ReLU check), PASS/FAIL exit code.
- scripts/diag_zero_out.py  — multi-input diagnostic (noise/zeros/gray) for
  yolov5s_relu + a system mobilenet sanity check.
- scripts/probe_yolo.py     — output semantics probe on a real /tmp/cal.ppm
  frame (per-head box/obj/cls ranges, hot-cell counts). Feeds the model a
  4-dim input (img[None]).
- tools/save_board_csv.py   — RKNNLite predictions dump (misc/board_preds.csv).
- tools/oracle_check.py — inference parity check: C++ detections (--box-log
  "raw" lines) vs an independent RKNNLite oracle over --dump-cal PPM dumps.
  Feed recipe: raw uint8 + img[None], no data_format kwarg (the
  save_board_csv.py path, calibrated against misc/board_preds.csv);
  best-frame matching across --cal-interval (dump numbering follows the
  FRAME counter, not the dump slot). Usage: oracle_check.py <dump_dir>
  <box-log.csv>; PASS/FAIL exit code. 2026-11-01 verdict (pool + base):
  see docs/state.md.
- tools/ab_run.py — static-frame A/B between two .rknn models over a
  file pattern (RKNNLite): --model, --pattern, --out, --conf; writes
  a per-frame predictions CSV. The v3-vs-v4 day/dusk A/Bs (INT8 build history - docs/state.md) and the
  v4-vs-v5 night/day A/Bs (2026-11-01: dataset_night 200 ppm, gamma 2.0;
  dataset_day3 100 ppm, gamma 1.0; conf 0.18) ran through it.
- tools/compare.py + tools/list_missed.py — INT8 vs FP32 comparison
  (IoU>0.5 + class match, mean |dscore|); see docs/state.md (INT8 vs FP32 validation).
- tools/probe_dark_heads.py, probe_gamma_sweep.py, probe_adaptive.py,
  probe_gamma3_det.py — dark-frame probes (see docs/state.md, Night session findings;
  gamma3 results in misc/gamma3_det.csv).
- tools/peek_dark.py — renders the reference dark frames to PNG
  (dataset_night/, hardcoded triple, ignores CLI args); dark-band
  stats are printed separately.

## Build
    ./scripts/build.sh

Target CPU flags (CMakeLists.txt TARGET_CPU_FLAGS, 2026-11-02):
-mcpu=cortex-a76+dotprod+crypto -mtune=cortex-a76.cortex-a55. The dot form is legal
only in -mtune (GCC 10.2 rejects -mcpu=cortex-a76+cortex-a55: "+"
there means a feature modifier). Measured effect on the camera-bound
pipeline: none (sum 95.5 ms, ~20.0 FPS before and after — the
boundary is the camera pacing, inference lives on the NPU); kept as
a documentation of intent.

Models are NOT tracked by git (*.rknn is ignored): they are build
artifacts of the Colab quantization pipeline (see Platform constraints
and M2.4) and live on each machine, transferred out of band. After a
fresh clone, place the .rknn files next to the executable — the
--model flag takes any path. The rknn-toolkit-lite2 wheel is NOT tracked:
it lives in GitHub Releases (tag rknn-toolkit-lite2-v2.5.0), installed by
scripts/install_rknn.sh (download + SHA256 verify + pip install).

## Run
    sudo ./build/light_ai \
        --device /dev/video20 \
        --frames 3000 \
        --net --net-h264 \
        --net-fps 0 \
        --pool 3

    sudo ./build/light_ai \
        --device /dev/video20 \
        --net-src http://192.168.1.2:8080/video  \
        --pool 3 \
        --net \
        --net-h264 \
        --net-fps 0 \
        --net-bitrate 3000 \
        --frames 3000

(LD_LIBRARY_PATH=/usr/local/lib was required earlier; since the
2026-10-28 rebuild ldconfig resolves /usr/local/lib — ldd verified.
Reinstate the prefix only if the ldconfig cache is ever reset.)

Defaults: /dev/video20, 1280x960 MJPEG (since 2026-10-30 — full-frame
2 Mp, letterbox band 640x480, see the 960p performance section); the model default is the working INT8 model (misc/, see Build); --model overrides it.
Note (2026-11-01, updated 11-02): the Run line relies on the
built-in default model (yolov5s_relu_int8_mmse_v5.rknn — the working
model per docs/state.md; --model overrides). M5 standard run:
--pool 3 (NPU context pool) + --net-fps 0 (VFR follow-source pts);
--core is ignored with pool > 1 (the pool pins its own masks).
A missing
configured node (ENOENT) is not fatal: the pipeline falls back to
find_usb_capture_node() and adopts the winner (startup fallback, M3.8);
non-ENOENT open errors (EBUSY/EACCES) stay fatal. Options:
--help/-h prints the option list (generated from the parser table).
--config <path> loads an INI config file (flat key = value; applied
before CLI flags so CLI always overrides).
--device, --net-src http://host:port/path (HTTP MJPEG pull source;
both flags are repeatable and accumulate into Config::sources for M5
multi-camera; resolution is auto-detected from JPEG SOFn,
including portrait streams such as 720x1280 -> 368x640 band),
--model, --frames (>= 1; default: run until Ctrl-C), --dump-cal,
--conf, --iou,
--core 0|1|3|7 (NPU core pinning: 0=AUTO default, 1=core0, 3=0+1,
7=0+1+2; measured: infer 25.1->21.2 ms on 012, FPS unchanged —
source-bound, M2.5),
--cal N, --cal-interval K (--cal saves up to N model-input frames into
dataset/, one every K frames — the int8 calibration dataset collector;
one frame is ~1.2 MB, so 200 frames are ~240 MB on /tmp's storage).
--dump-cal (no argument) writes ONE model-input frame (P6 PPM) to
/tmp/cal.ppm, once per run — the first eligible frame. The oracle input
for tools/oracle_check.py is the SERIES: --cal N --cal-interval K writes
cal_%04d.ppm into dataset/, numbering follows the frame counter: with
--cal-interval K the dumps are frames 0, K, 2K, .... The dump files are
created by whoever runs the binary — root/normal-user ownership
conflicts abort with "calibration dump failed" (see Platform constraints).
--cal-dir PATH redirects the --cal SERIES directory (default: dataset/).
The directory is created and write-probed at startup, so a read-only or
uncreatable path fails immediately, not mid-run. Without --cal the option
is rejected (--cal-dir alone does nothing; --dump-cal is unaffected and
always writes /tmp/cal.ppm). Example: --cal 50 --cal-interval 4
--cal-dir /tmp/night_cal -> /tmp/night_cal/cal_%04d.ppm, same numbering
as the dataset/ default.
--box-log <path> (per-frame CSV of raw vs smoothed boxes — the
M3.10 jitter A/B tool; near-zero cost when unset).
Numeric values are strictly validated (garbage is an error, not a
silent zero).

--net enables the preview server (NUT container, raw model-input
rgb24, tcp port 1234); --net-fps N paces the stream (default 25).
--net-h264 switches the preview to the h264 path (h264_rkmpp,
MPEG-TS): the stream is the letterbox band at its native size
(640x480 for the default 1280x960 camera), with YOLO boxes drawn
in. Transport: TCP pull by default — the board listens on
--net-port (default 1234), the viewer connects by the board's LAN
IP; --net-host <pc-ip> switches to the legacy UDP push to that
destination. Bitrate via `--net-bitrate` (default 12000 kbps; 1500-2500 is plenty for 640x480).
Viewer (pull, product path): ffplay -fflags nobuffer
tcp://<board-ip>:1234
Viewer (UDP push, legacy, low-latency, M3.9): ffplay -fflags
nobuffer -flags low_delay -framedrop -probesize 32
-analyzeduration 0 -vf setpts=0 -f mpegts
udp://0.0.0.0:1234 — an explicit IPv4 bind is required on Windows
hosts (udp://@:1234 binds :: / IPv6 and silently receives no IPv4
packets; verified with Get-NetUDPEndpoint).
On the legacy Windows laptop the maintained viewer is mpv (software
decode + gpu-next/d3d11 render, auto-reconnect loop — see the Viewer
section, revised 2026-11-01; the 2026-10-31 dxva2-copy combo stays as
the CPU-unload fallback). --cpu-draw forces CPU
box drawing even when the GPU drawer is up (A/B baseline; default
is GPU drawing).

Graceful shutdown (2026-11-02): SIGINT/SIGTERM do not kill the
process; the frame loop exits through the normal path — the log ends
with "stop: signal received, draining and flushing...", the TOTAL
timing line, "Processed N frames ..." and a clean teardown (encoder
flush, camera STREAMOFF). The same applies to kill <pid> (SIGTERM) —
use it in automated tests instead of SIGKILL.

## Viewer (mpv, legacy Windows laptop; recorded 2026-10-31, revised 2026-11-01)

The daily viewer record for the viewing box: Core 2 Duo T9550, ATI Mobility
Radeon HD 3400 (legacy driver 8.970.100.0, 2013-04-24 — the final release for
this series), DirectX 10.1 — the GPU EXPOSES D3D11 at feature level 10_1
(verbose-log verified, fact 1), UVD2 h264 decode via DXVA2, OpenGL without
WGL_NV_DX_interop; a clean Windows build, mpv 0.41 64-bit. The stream is the
h264 TCP pull (tcp://192.168.1.10:1234, MPEG-TS confirmed by the demuxer log:
"Found 'mpegts' at score=100") — the 640x480 band of the 1280x960 working
mode (see NetEncoder). Note on cadence: the stream is paced by --net-fps
(default 25), but camera pacing is a discrete firmware state {20, 25, 30}
(see Platform constraints) — a cadence mismatch is a pacing level, not a
player defect.

REVISION 2026-11-01 (A/B session): the product viewer is now SOFTWARE decode
+ gpu-next/d3d11 render. Two 2026-10-31 findings are superseded: fact 1
("gpu-next does not work") and the fact-3 working combo (dxva2-copy runs but
is measurably slower end-to-end than software decode). --untimed is
disqualified as a live-stream measurement method (fact 9). Standing facts
keep their 2026-10-31 numbers.

Established facts:
1. gpu-next WORKS on this GPU (supersedes the 2026-10-31 0x887a0004
   finding): "Initializing GPU context 'd3d11'", "Using Direct3D 11
   feature level 10_1", device ATI Mobility Radeon HD 3400 (1002:95c4),
   flip-model presentation, SPIR-V via shaderc. The legacy 8.970.100.0
   driver serves FL 10_1 — the minimum — and that is enough for gpu-next.
   Do not treat "no D3D11" as a given for this card.
2. Zero-copy decode is unreachable — UNCHANGED (2026-10-31: win/angle/
   dxinterop; 2026-11-01 adds d3d11va — unsupported by the legacy driver,
   software fallback under an explicit name). Do not re-probe.
3. dxva2-copy is SLOWER than software decode at 640x480 h264 (supersedes
   "the copy is negligible"): the copy-mode decoder pulls every frame
   GPU->system RAM and the renderer copies it back into a texture — two
   copies per frame on a chip that copies VRAM->CPU slowly (mpv docs:
   copy-mode hwdecs are "likely no more efficient than software decoding
   except for some codecs" — h264 is not one of them). Live A/B:
   latency visibly increases. Keep the combo as the documented CPU-unload
   trade, never the default. INVALID_ENUM chatter at texture creation is
   driver noise, the picture is unaffected.
4. hwdec=auto rejected — UNCHANGED (four device creations per start).
   The product path carries NO --hwdec at all: the default is software
   decode, nothing to probe, the start is deterministic.
5. "non-existing PPS 0 referenced" at connect is the mid-GOP norm —
   UNCHANGED; dies down by itself after the first captured keyframe.
6. The board drops the stream — an auto-reconnect loop is mandatory —
   UNCHANGED (reconnect* options are HTTP-only, tcp:// is not covered).
7. Start timing — UNCHANGED: ~0.73 s base (connect + wait for the next
   keyframe); lavf/probe/recv options measure zero.
8. low-latency profile carries cache=no etc. — UNCHANGED; --cache=no is
   kept in the launch lines for self-documentation.
9. --untimed on a LIVE tcp:// stream is a measurement ARTIFACT (NEW):
   untimed plays faster than real time, runs into the live edge of a
   realtime source and shows periodic "(Buffering)" every 2-3 s. Untimed
   runs are invalid channel diagnostics; all buffering and latency
   numbers in this record are timed-mode. The judder note (25 fps on a
   60 Hz monitor) stands for offline files.
10. Overlay markers (NEW): the (hw) suffix after the codec name is the
    hardware-decode marker — its ABSENCE means software decode;
    "Context: d3d11" in the VO line is the RENDER context, not the
    decoder (the d3d11va decode failure does not prevent the d3d11
    render context). Press `i` for Frame Timings / Dropped Frames.
11. Stream health in timed mode (NEW, after the board-side socket/muxer
    fixes — TCP_NODELAY, 2 MiB `SO_SNDBUF` (256 KiB initially, raised 2026-11-06), max_delay=0, flush_packets,
    +pat_pmt_at_frames+resend_headers — and --net-bitrate 3000, verified
    by the MPP log "set rc cbr bps [3000000:...]"): ZERO "(Buffering)"
    per session; the status-line cache settles (0.0-1.0 s) and stops
    moving. The overlay Bitrate reads ~2.40-2.45 Mbps vs the configured
    3000 — MPP CBR under-delivery on a static scene, normal.
12. Render-path A/B at 640x480 (NEW, `i` stats, 20 FPS source):
    gpu-next/d3d11: fresh ~10.7-11.5 ms, redraw ~1.5 ms; gpu/opengl:
    fresh ~4.0-4.1 ms, redraw ~0.15-0.26 ms; dropped frames 0 on both.
    The frame budget at 20 FPS is 50 ms — both paths are far under; the
    product choice of d3d11 rests on flip-model presentation, a silent
    log (no INVALID_ENUM) and a clean 0.0 s cache, not on per-op numbers.
    The OpenGL run's stats page showed Total Cache 232 KiB (0.8 s)
    despite --cache=no — an unexplained one-off; d3d11 runs show none.

Launch. No config file is used (%APPDATA%\mpv\mpv.conf removed — all
options on the command line). Product path (software decode + d3d11
render), C:\mpv64\watch.bat:

    @echo off
    :loop
    echo %date% %time% >> C:\mpv64\restarts.log
    mpv.com --cache=no --profile=low-latency --network-timeout=5 tcp://192.168.1.10:1234
    timeout /t 1 >nul
    goto loop

One-shot from PowerShell:

    mpv.com --cache=no --profile=low-latency --network-timeout=5 tcp://192.168.1.10:1234

Fallback (CPU unload at a latency cost; the 2026-10-31 OpenGL combo):

    mpv.com --vo=gpu --gpu-api=opengl --gpu-context=win --hwdec=dxva2-copy --profile=low-latency --network-timeout=5 --opengl-swapinterval=0 tcp://192.168.1.10:1234

Option notes (revised 2026-11-01):
- NO --hwdec — software decode is the product path: d3d11va is not
  supported by the legacy driver, zero-copy dxva2 is unreachable (fact
  2), dxva2-copy adds latency (fact 3). At 640x480 software decode is
  negligible CPU load.
- profile=low-latency — unchanged (fact 8).
- network-timeout=5 — unchanged.
- opengl-swapinterval=0 and stream-lavf-o=recv_buffer_size=32768,
  tcp_nodelay=1 — the OpenGL fallback only. The d3d11 product path
  needs neither; the 32 KiB receive trim is counterproductive for the
  board's IDR bursts, and Nagle is already off on the board side.
- The bat loop IS the auto-reconnect — unchanged (fact 6); the echo
  line timestamps every drop, remove it if the log is unwanted.
- bat nuances — unchanged: Ctrl+C or close the console to stop; q
  inside mpv restarts instantly.

Latency (timed mode only; the --untimed row is removed as invalid):

| Configuration                        | Lag                                    |
|--------------------------------------|----------------------------------------|
| default mpv (cache on)               | 2-3 s                                  |
| + low-latency + cache=no (PRODUCT)   | ~1 s (2026-11-01, stopwatch estimate)  |
| + dxva2-copy                         | visibly worse (two copies/frame)       |
| photo-timestamp measurement          | PENDING                                |

The residual second most likely lives in the board encoder — now
consistent: with the demuxer cache cut to zero the client no longer
contributes seconds, and the MPP log confirms "gop 25". Honest
measurement: the photo-timestamp run is still PENDING — burn a clock
into the board frame, photograph the monitor so both the stream and
the wall clock are in one shot; ~±50 ms accuracy. A --vo=null
comparison separates the network+decoder part from the output path.
Player-side cross-check: ffplay -fflags nobuffer -rtbufsize 100k
tcp://192.168.1.10:1234.

Board-side backlog (recorded 2026-10-31, folded into the M3.9 follow-up):
- B-frames off (low-delay / zerolatency in the encoder) — cut the
  per-frame encoder reorder delay; GOP 25 is already confirmed live
  (see above).
- UDP/RTP output instead of TCP, if the board can: a lost TCP packet
  head-of-line-blocks everything behind it until retransmission.


Constraints to remember (revised):
- Zero-copy is impossible on this machine by construction (fact 2).
- Software decode + d3d11 render is the optimum; dxva2-copy is a
  documented CPU-unload trade with a latency cost (fact 3).
- --untimed: acceptable for offline files (with judder), INVALID for
  live-stream diagnostics (fact 9).
- A bare tcp:// without a container is sensitive to a dirty bitstream
  from the board — self-healing with the short GOP, unchanged.

One spot remains unverified: the photo-timestamp latency number. Fill
it in and the record is fully closed.

Diagnostic cheat sheet (UNCHANGED from 2026-10-31):

    # board port alive
    Test-NetConnection 192.168.1.10 -Port 1234

    # log folder (mpv does NOT create it)
    New-Item -ItemType Directory -Force C:\mpv64

    # decode+network without graphics
    mpv.com --vo=null --profile=low-latency tcp://192.168.1.10:1234

    # output without OpenGL (plan B)
    mpv.com --vo=gdi --profile=low-latency tcp://192.168.1.10:1234

    # start timing: the "playback restart complete" marker in the log
    mpv.com --vo=null --frames=30 --profile=low-latency --log-file=C:\mpv64\log.txt tcp://192.168.1.10:1234

    # option acceptance check (silence in the grep = accepted; "Could not set" = rejected)
    mpv.com -v --frames=1 ... --log-file=C:\mpv64\opt.log tcp://192.168.1.10:1234
    Select-String -Path C:\mpv64\opt.log -Pattern "Could not set"

    # crash code decode
    echo $LASTEXITCODE
    Get-WinEvent -LogName Application -MaxEvents 5 | Where-Object Id -eq 1000 | Format-List TimeCreated, Message

    # log to file
    mpv.com ... --log-file=C:\mpv64\log.txt

    # options and profiles of this exact build
    mpv.com --list-options | findstr /i angle
    mpv.com --show-profile=low-latency

    # the GPU
    Get-CimInstance Win32_VideoController | Format-List Name, DriverVersion, DriverDate

Launch mpv.com (the console host), not mpv.exe — the GUI build prints
nothing to the console.

## Roadmap (milestones M1-M3.10)

The full roadmap with per-milestone history lives in docs/roadmap.md.
Milestone IDs referenced across this README and in docs/state.md
(M2.4, M2.5, M3.9 ...) resolve there. New milestone entries go to
docs/roadmap.md too.

## Model & quantization history

The accumulating record - INT8 build lineage (v1..v4), INT8-vs-fp16
validation and the night-session findings (incl. the dataset map) -
moved to docs/state.md. Pointers in this README like "see INT8 build
history", "see INT8 vs FP32 validation", "see the dataset map" and
"see Night session findings" all refer to that file.
