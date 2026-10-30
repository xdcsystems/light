# light_ai — zero-copy video analytics on RK3588

Pipeline: V4L2 (MJPEG) -> MPP decoder (YUV422SP, dma-buf) -> RGA (im2d resize
straight into the dma-buf tensor of the NPU) -> RKNN (zero-copy) -> CPU
post-processing.

## Rules of the project
- GStreamer is forbidden (owner's decision).
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
  (anon-namespace helpers), box_smoother.hpp (plain member).
  tools/ is exempt (disposable probes).
- No virtual functions, no inheritance hierarchies: zero vtables, zero
  dynamic polymorphism. Layer boundaries are the composition of concrete
  members (pipeline.hpp), not an interface contract. Static polymorphism
  (templates) is the sanctioned replacement where one implementation
  serves several types: PodList<T, N> (include/pod_list.hpp) — one
  fixed-capacity list, two instantiations (DetList in
  yolo_postprocess.hpp, GpuBoxList in gpu_box_drawer.hpp), zero heap.
- const by default: member functions that do not mutate state are
  const; parameters are passed by const-ref unless the callee owns a
  copy. Out-parameters are plain references filled by the callee
  (yolo_postprocess fills DetList& out — no return-value vectors in
  the frame path).
- Branch tables over if-chains: a multi-way dispatch on a fixed set
  becomes a switch or a constexpr table, not a ladder. Reference:
  det_color in pipeline.cpp (switch).
- Preallocated buffer contract: a buffer that lives across frames
  (a member, not a local) is NEVER implicitly empty — the callee owns
  the reset and clears it at entry (DetList out.clear() in
  yolo_postprocess). Converting a returned std::vector into a
  preallocated member silently loses the "fresh vector each call"
  semantics; the 2026-10-27 live run caught exactly that (detections
  accumulated to the 128 cap, the box set froze, person went
  undetected — 705636b). A heap scan cannot catch this class of bug;
  only a live visual run can.

## Layers
- CameraV4L2 : MJPEG capture, mmap buffers, DQBUF/QBUF. G_FMT is verified
               against the requested mode; a driver-side silent mode
               substitution is a hard error. The node is NOT instance
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
               and trial-opened; the pipeline tracks the current node in
               cam_node_ and adopts a re-enumerated one transparently.
               A permanent unplug exhausts the 20x1 s window and ends
               the run with exit code 1.
- MppDecoder : MJPEG -> YUV422SP decode into an external dma-buf pool,
               returns the frame fd. Sized from Config, not hard-coded.
               Also hands out an aux dma-buf from the same pool for the
               multi-pass RGA path.
- RgaResizer : im2d resize by fd (wrapbuffer_fd + improcess). Two-pass path
               when the per-axis scale ratio exceeds the RGA3 limit (1/8),
               guarded against aux_fd < 0. All jobs are pinned to RGA3 core0
               via imconfig at startup.
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
- Pipeline   : model input geometry is read from the model file itself
               (input_attr), not from CLI flags. Aux allocation happens after
               model geometry is known (constructor-order bug fixed; a
               one-line "aux not needed" log confirms the single-pass path).

- GpuBoxDrawer (M3.7): box drawing on Mali G610 (EGL/GBM) straight
               into the NPU input tensor. The tensor dma-buf is imported
               as an EGLImage; one draw call per frame; native fence
               sync; falls back to CPU drawing (draw_boxes_cpu in
               pipeline.cpp) if the tensor pitch is not usable or init
               fails. --cpu-draw forces the CPU path (A/B baseline).
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
               band's native size (640x360 for the default 1280x720
               camera; 640x480 at the 1280x960 working mode since
               2026-10-30). One viewer at a time; no encode until the
               first viewer connects; a stalled viewer is dropped via
               SO_SNDTIMEO (2 s). Late joiners via repeated
               PAT/PMT/SDT.
               Client (pull, product path): ffplay -fflags nobuffer
               tcp://<board-ip>:1234
               Client (UDP push, legacy, low latency): ffplay -fflags
               nobuffer -flags low_delay -framedrop -probesize 32
               -analyzeduration 0 -vf setpts=0 -f mpegts
               udp://0.0.0.0:1234

## Platform constraints (learned the hard way, M2-prep)
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
  Detection mapping T: x_scene = x_det * cam_w / model_w, y_scene =
  (y_det - lb_top) * cam_w / model_w; detections on the bars (y_det outside
  the band) are filtered.
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
  reading — to reconcile in M2.6.
- yolov5 .rknn outputs are native fp16 (float16, NCHW) — no CPU-side
  dequantization needed; the lite-runtime wraps them as float32.
- int8-quantized yolov5s_relu_int8.rknn: built with rknn-toolkit2 2.5.0
  on Colab. v1: calibrated on 50 night frames (--cal 50 --cal-interval 4);
  v2: recalibrated on dataset_mix (25 day + 25 night frames) —
  see INT8 vs FP32 validation. v3 (night A/B done, day A/B pending; superseded as the
  working model by int8_mmse, 2026-10-30):
  recalibrated on 60 fresh day-4 daytime frames with mean=0/std=255 ->
  canonical input scale 1/255 (0.00392157) — see M2.4.
  int8_mmse (WORKING MODEL since 2026-10-30): mmse-quantized on
  dataset_mix (the same 50-frame set as v2); input zp=-128,
  scale=0.00436174 (fitted, same as v2 — raw RGB byte writes remain
  correct). Daytime steady session (1280x960, conf 0.25): infer
  identical to v3 (~29.5-30.3 ms, one NPU core either way), but
  detections steadier — cup and dining table present in nearly every
  60-frame block vs episodic on v3; working conf 0.25. Night A/B for
  mmse: pending (M2.6).
  v4 (2026-10-30): mmse on the fresh 960p daytime dump (dataset_day3,
  50 of 100 frames); day and dusk A/B vs v3 — equal (see INT8 build
  history); night A/B vs fp16 pending (M2.6 (d)).
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
  fill; see the dataset map.)
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
  (img[None]); a 3-dim NHWC array raises "need 4dims input".
- Default resolution is 1280x720: same model input geometry as 1080p
  (identical s, band and T), but single-pass RGA and cheaper MJPEG decode.

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
  --net-bitrate (default 12000 kbps). Fine for the preview; revisit when
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
write; SO_SNDTIMEO caps a stalled viewer at 2 s). No viewer: enc=0.0
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
960p dump — M2.6.

## Scripts
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
- tools/ab_run.py — static-frame A/B between two .rknn models over a
  file pattern (RKNNLite): --model, --pattern, --out, --conf; writes
  a per-frame predictions CSV. The v3-vs-v4 day/dusk A/Bs (INT8
  build history) ran through it.
- tools/compare.py + tools/list_missed.py — INT8 vs FP32 comparison
  (IoU>0.5 + class match, mean |dscore|); see INT8 vs FP32 validation.
- tools/probe_dark_heads.py, probe_gamma_sweep.py, probe_adaptive.py,
  probe_gamma3_det.py — dark-frame probes (see Night session findings;
  gamma3 results in misc/gamma3_det.csv).
- tools/peek_dark.py — renders the reference dark frames to PNG
  (dataset_night/, hardcoded triple, ignores CLI args); dark-band
  stats are printed separately.

## Build
    ./scripts/build.sh

Models are NOT tracked by git (*.rknn is ignored): they are build
artifacts of the Colab quantization pipeline (see Platform constraints
and M2.4) and live on each machine, transferred out of band. After a
fresh clone, place the .rknn files next to the executable — the
--model flag takes any path. The rkmpp wheel in misc/ stays tracked
(0.6 MB, toolkit reproducibility).

## Run
    sudo ./build/light_ai \
        --device /dev/video20 --model ./misc/yolov5s_relu_int8_mmse.rknn --frames 3000 \
        --net --net-h264 --core 7

(LD_LIBRARY_PATH=/usr/local/lib was required earlier; since the
2026-10-28 rebuild ldconfig resolves /usr/local/lib — ldd verified.
Reinstate the prefix only if the ldconfig cache is ever reset.)

Defaults: /dev/video20, 1280x960 MJPEG (since 2026-10-30 — full-frame
2 Mp, letterbox band 640x480, see the 960p performance section); the
model default is still the M1
mobilenet_v1 relic — always pass --model for detection runs. A missing
configured node (ENOENT) is not fatal: the pipeline falls back to
find_usb_capture_node() and adopts the winner (startup fallback, M3.8);
non-ENOENT open errors (EBUSY/EACCES) stay fatal. Options:
--help/-h prints the option list (generated from the parser table).
--device, --model, --frames, --w, --h, --dump-cal, --conf, --iou,
--core 0|1|3|7 (NPU core pinning: 0=AUTO default, 1=core0, 3=0+1,
7=0+1+2; measured: infer 25.1->21.2 ms on 012, FPS unchanged —
source-bound, M2.5),
--cal N, --cal-interval K (--cal saves up to N model-input frames into
dataset/, one every K frames — the int8 calibration dataset collector;
one frame is ~1.2 MB, so 200 frames are ~240 MB on /tmp's storage).
--box-log <path> (per-frame CSV of raw vs smoothed boxes — the
M3.10 jitter A/B tool; near-zero cost when unset).
Numeric values are strictly validated (garbage is an error, not a
silent zero).

--net enables the preview server (NUT container, raw model-input
rgb24, tcp port 1234); --net-fps N paces the stream (default 25).
--net-h264 switches the preview to the h264 path (h264_rkmpp,
MPEG-TS): the stream is the letterbox band at its native size
(640x360 for the default 1280x720 camera), with YOLO boxes drawn
in. Transport: TCP pull by default — the board listens on
--net-port (default 1234), the viewer connects by the board's LAN
IP; --net-host <pc-ip> switches to the legacy UDP push to that
destination. Bitrate via --net-bitrate (default 12000 kbps; 2500
is plenty for 640x360).
Viewer (pull, product path): ffplay -fflags nobuffer
tcp://<board-ip>:1234
Viewer (UDP push, legacy, low-latency, M3.9): ffplay -fflags
nobuffer -flags low_delay -framedrop -probesize 32
-analyzeduration 0 -vf setpts=0 -f mpegts udp://0.0.0.0:1234 — an
explicit IPv4 bind is required on Windows hosts (udp://@:1234
binds :: / IPv6 and silently receives no IPv4 packets; verified
with Get-NetUDPEndpoint). --cpu-draw forces CPU box drawing even
when the GPU drawer is up (A/B baseline; default is GPU drawing).

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
      (b) PARTIAL 2026-10-30: A/Bs ran as v3-vs-v4 on the day and
      dusk dumps — v4 == v3 on both profiles (numbers in INT8
      build history); the planned same-scene v4-vs-int8_mmse A/B
      is still pending — fold into the night session;
      (c) reconcile bench_npu 15.4 ms@012 against the single-core
      finding (parallel-contexts hypothesis -> M5);
      (d) night A/B for the winner — pending: tonight,
      dataset_mix, gamma 2.0 -> uint8 -> /255 preprocessing,
      conf 0.18.
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

## INT8 build history (rknn-toolkit2 2.5.0, Colab)

Versions in misc/ (build lineage):
- yolov5s_relu.rknn — fp16-convert build without quantization; the
  accuracy reference for every A/B.
- yolov5s_relu_int8.rknn — v1: default KLD quantization, mixed
  calibration; superseded (night ALARM — see M2.4).
- yolov5s_relu_int8_mmse.rknn / _v2.rknn — mmse, 50 frames
  (dataset_mix); the working model since 2026-10-30.
- yolov5s_relu_int8_v3.rknn — mmse. The build records disagree:
  one history notes a 6000-frame evening dump
  (cal_evening_6000.tgz), M2.4 records 60 fresh day-4 daytime
  frames with the canonical 1/255 scale (0.00392157) — the same
  filename was evidently reused across builds; treat the M2.4
  record as the current v3 state.
- yolov5s_relu_int8_v4.rknn — mmse on the fresh 960p daytime dump
  (dataset_day3: 50 frames, every 2nd of 100). Day and dusk A/B
  vs v3 — equal (2026-10-30, status below); night A/B pending
  (M2.6 (d)).

v4 recipe (build_v4.py; Colab notebook Untitled2.ipynb, a copy is
saved as colab_v4_recipe.ipynb in artifacts): mean [0,0,0],
std [255,255,255], quant_img_RGB2BGR=False,
quantized_method='channel', quantized_algorithm='mmse',
target_platform='rk3588', optimization_level=3,
do_quantization=True, dataset = 50 png. Build time 48 min (mmse
~31 s per iteration, 90 iterations; Colab log: DONE 2884.1 s).

Toolkit facts (Colab side):
- Colab does NOT support swap (fallocate / loop device / separate
  disk are all rejected by the kernel): mmse memory is capped by
  the 12 GB RAM -> at most 50 frames of 960p calibration.
  Verified.
- Expected input scale under mmse: ~0.004-0.0045. A flat 1/255
  (0.00392157) is the signature of the default algorithm, not
  mmse. Verify after each build: grep 'scale' build_v4.log.
- Toolkit 2.5.0 converts input AND outputs to int8 by default;
  deploying through the Runtime API means accounting for the
  dequant scale. RknnEngine already rewrites the output attr to
  FLOAT16 so the runtime dequantizes into the zero-copy buffer
  (see RknnEngine) — postprocess stays untouched; the "outputs
  are native fp16" note above refers to non-quantized builds.
- onnx source: yolov5s_relu.onnx, 28.9 MB, wget from
  ftrg.zbox.filez.com (link alive on 2026-10-30).
- Calibration dumps are duplicated to Drive; artifacts live in
  Drive (yolov5_rknn) and on the board (misc/). Colab-side
  files: postprocess.py (YOLOv5 decode: anchors 80/40/20,
  strides 8/16/32, NMS; input = 3 maps (255,H,W) post-sigmoid)
  and build_v4.py.

Status 2026-10-30 (v3-vs-v4 A/B):
- Day (dataset_day2, 50 frames): conf 0.25 — 101 vs 101
  detections; the discrepancies are threshold noise (cls 39,
  scores 0.256/0.259).
- Dusk (dataset_dusk, 100 frames, brightness ~86 vs ~116
  daytime): conf 0.18 — 223 vs 227 detections, ALL discrepancies
  sit in the 0.18-0.24 score band (mostly cls 56; symmetric
  split: 12 v4-only vs 8 v3-only, 37 IoU-matched); conf 0.25 —
  186 vs 186, two frames differ by one detection.
- Verdict: v4 == v3 on the daytime and dusk profiles; the mean
  dusk score is even slightly higher for v4 (0.491 vs 0.478).
- Note: these are INT8-vs-INT8 A/Bs; the v3-vs-fp16 day A/B of
  M2.4-b (a) stays open.
- Remaining: the night A/B — tonight after ~16:30, dataset_mix,
  gamma 2.0 -> uint8 -> /255 preprocessing, conf 0.18; verify
  first that tools/ab_run.py carries the gamma flag (check
  --help). The planned same-scene v4-vs-int8_mmse A/B (M2.6 (b))
  folds into the same session.

## INT8 vs FP32 validation (2026-10-26, yolov5s_relu_int8_v2)

Purpose: verify the freshly calibrated int8 model (v2) against a float32
reference on identical inputs — quantization verdict for the M2.4 re-check
(path B). The v1 ALARM (box shift ~17 px, dconf -0.04) predates this
recalibration; this section documents v2.

Methodology:
- 50 calibration frames (n1_cal_*/n2_cal_*), 640x640, same gamma-2.0 -> uint8
  -> /255 preprocessing on both sides.
- FP32 reference: yolov5s_relu.onnx via onnxruntime (Colab), artifacts
  misc/ref_preds.csv; INT8: RKNNLite on board, tools/save_board_csv.py ->
  misc/board_preds.csv; comparison tools/compare.py + tools/list_missed.py
  (IoU>0.5 + class match, mean |dscore|).

Pitfall learned: the ONNX model does NOT normalize its input (rknn divides
by 255 internally, std=255 in the quant config). Feeding raw 0..255 to
onnxruntime saturates the model -> hundreds of phantom detections. Input
must be 0..1.

Results:

| conf (both) | FP32 det | INT8 det | matched | missed | extra | mean \|dscore\| |
|---|---|---|---|---|---|---|
| 0.25 | 89 | 89 | 86 (96.6%) | 3 | 3 | 0.028 |
| 0.25 ref / 0.18 int8 | 89 | 98 | 89 (100%) | 0 | 9 | 0.028 |
| 0.18 | 101 | 98 | 97 (96.0%) | 4 | 1 | 0.028 |

Conclusions:
- v2 INT8 == FP32 within noise: mean |dscore| 0.028 at both thresholds, box
  geometry matches to a pixel, no loss of any detection with score >= 0.30.
  All discrepancies sit in the 0.18-0.29 band (threshold-edge candidates).
  Missed at 0.25: cls58 (0.2509/0.2874/0.2561). Missed at 0.18: cls75
  (0.1831), cls39 (0.1893), cls58 (0.2205), cls41 (0.1825) — FP32 scores,
  i.e. objects the FP32 model itself was unsure about.
- Lowering the board conf to 0.18 pulls those edge candidates back; the 9
  "extra" detections at 0.18 are stable recurring objects (same boxes across
  frames: cls39 at ~(249,368)-(259,396), cls58 at ~(295,376)-(347,482)), not
  noise. Working conf for v2: 0.18-0.20 (v3: 0.25 — see M2.4).
- v1 box-shift ALARM is superseded for v2: no systematic shift on the
  calibration set (within-pixel agreement). M2.4 night A/B was re-run
  on v2 (2026-10-26): ALARM confirmed — see the last bullet below;
  M2.4 is closed on v3.
- Dark blindness (n2_cal_0096, n2_cal_0152: 0 detections) is identical in
  FP32 and INT8 — quantization-independent, a data/scene problem.
- Class-flip observation: the recurring object at ~(248,366)-(260,396) gets
  cls75/cls39/cls41 across frames; verify against ground-truth labels later.
- Night A/B on the board (2026-10-26, live camera): v2 ALARM confirmed
  (systematic dconf -0.045 vs fp16, vase never detected) — superseded by
  v3; see M2.4 for the closed verdict.

## Night session findings (2026-10-26): dark frames and gamma verdict

Dataset map (owner's clarification; current as of 2026-10-30):
- dataset_day2    — day-2 daytime dump, 50 frames cal_00XX.ppm
  (brightness two clusters: ~89 and ~114). The day A/B reference
  for the v3-vs-v4 comparison (2026-10-30).
- dataset_day3    — fresh 960p daytime dump (2026-10-30,
  --cal 100 --cal-interval 15): 100 frames, brightness ~116
  uniform. The quantization source of v4 (every 2nd frame = 50).
- dataset_dusk    — dusk dump (2026-10-30, same protocol): 100
  frames, brightness ~86 (vs ~116 daytime). cal_0000.ppm is an
  AE-settling outlier (brightness 35) — exclude it from any
  future calibration.
- The former day-4 daytime dump (the old dataset/, 60 frames:
  pre-letterboxed 640x640 network input, gray-128 padding rows
  0-139 and 500-639, ~44% of pixels, content 640x360 centered;
  one static daytime view; people intermittently cross the scene,
  occluding the lower half, Rmean 78-82 -> 53-57 in occluded
  frames, upper half unchanged, k=1.0; v3's quantization input,
  built 20:20 on 2026-10-26) is archived — duplicated to Drive.
  The dataset/ path was reused for fresh dumps (2026-10-30:
  dataset -> dataset_day3, then a fresh dump -> dataset_dusk).
- dataset_night/  — NIGHT calibration dump, day 1, the buggy
  pipeline that periodically produced row-tear artifacts.
  Hardest conditions for the model; all gamma probes ran on it.
  All cal_XXXX frame references in this section and in the
  dark-frame probes point HERE, not to the daytime dumps
- dataset_mix/    — calibration dump assembled proportionally
  from day-2 and night-1 frames; THE calibration set of
  int8_v2 / int8_mmse (50 frames: n1_cal_* every 2nd of 50,
  n2_cal_* every 8th of 200); v3's calibration set is separate —
  60 fresh day-4 daytime frames (see M2.4).
- There is NO retrained FP32. The earlier session note "model
  retrained on dataset_mix" was wrong: dataset_mix is a
  calibration set (unlabeled dumps), Colab has no runs/, no
  best.pt/last.pt exists. Any future training on night scenes
  needs labeled data.

Dark-frame classes (night_survey*.py, probe_dark_heads.py,
probe_gamma_sweep.py):
- "alive in darkness": cal_0037 (band median 3, max_obj 0.83),
  cal_0149, cal_0065 —
  backlight: dark silhouette against a bright doorway; the model sees these.
- "revive under extra gamma": cal_0096, cal_0152 — signal exists, outside the
  training domain; gamma 3.0 brings them back.
- "dead regardless": cal_0067 (person very close to camera, max
  scale + heavy row tears), cal_0068 (confirmed visually: row
  tears across the whole frame, scene unreadable — legacy-dump
  quality, not the camera). Untreatable by gamma; future
  treatment is data (needs labeling).

Gamma 3.0 verdict (tools/probe_gamma3_det.py, int8_v2, full decode+NMS, band
[140,500) filter, results in misc/gamma3_det.csv):
- Total gamma is negative for BOTH domains: dark — coverage up
  (131 -> 142 frames with detections) but detections down
  310 -> 261 (-16%), the stable recurring plant (cls58, 28
  consecutive frames) dies entirely; bright — count ~flat but
  mean conf drops (0.448 -> 0.382 @0.18), -19 detections @0.25.
- Conditional gamma (dark only): +14 revived frames (incl. cal_0030 person 0.81
  confirmed visually: real backlit silhouette (person) in
  doorway + potted plant, row-tear artifacts present,
  cal_0047/0100/0152 chairs 0.4-0.6) minus 3 fully killed
  frames and ~50 detections; also noise revivals (cal_0154 cls13 0.45).
- VERDICT (owner's decision): conditional gamma 3.0 NOT adopted — "considered;
  detection A/B: insufficient gain for the loss of a stable
  class". A gamma change would also require a fresh calibration
  anyway (the v2 scale 0.00436174 is fitted to the
  old distribution; v3 uses canonical 1/255, also
  distribution-dependent).
- Adaptive gain by band median stays rejected (probe_adaptive.py).
- Remaining dark-frame lever: fresh clean night dump -> v3 recalibration
  (DONE 2026-10-26 — M2.4 closed on v3).

