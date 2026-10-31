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
  see docs/state.md (INT8 vs FP32 validation). v3 (night A/B done, day A/B pending; superseded as the
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
  50 of 100 frames); day, dusk and night A/Bs vs v3 — equal (see docs/state.md INT8 build history; night includes one isolated lost detection at
  n2_cal_0040).
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
  a per-frame predictions CSV. The v3-vs-v4 day/dusk A/Bs (INT8 build history - docs/state.md) ran through it.
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
2 Mp, letterbox band 640x480, see the 960p performance section); the model default is the working INT8 model (misc/, see Build); --model overrides it. A missing
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
-analyzeduration 0 -vf setpts=0 -f mpegts
udp://0.0.0.0:1234 — an explicit IPv4 bind is required on Windows
hosts (udp://@:1234 binds :: / IPv6 and silently receives no IPv4
packets; verified with Get-NetUDPEndpoint).
On the legacy Windows laptop the maintained viewer is mpv (dxva2-copy
decode, auto-reconnect loop) — see the Viewer section below; the ffplay
lines above stay the generic reference. --cpu-draw forces CPU
box drawing even when the GPU drawer is up (A/B baseline; default
is GPU drawing).

## Viewer (mpv, legacy Windows laptop; recorded 2026-10-31)

The daily viewer record for the viewing box: Core 2 Duo T9550, ATI Mobility
Radeon HD 3400 (legacy driver 8.970.100.0, 2013-04-24 — the final release for
this series), DirectX 10.1 (no D3D11 at all), UVD2 h264 decode via DXVA2,
OpenGL without WGL_NV_DX_interop; a clean Windows build, mpv 0.41 64-bit.
The stream is the h264 TCP pull (tcp://192.168.1.10:1234, MPEG-TS confirmed
by the demuxer log: "Found 'mpegts' at score=100") — the 640x480 band of the
1280x960 working mode (see NetEncoder). Note on cadence: the stream is paced
by --net-fps (default 25), but camera pacing is a discrete firmware state
{20, 25, 30} (see Platform constraints) — a cadence mismatch is a pacing
level, not a player defect.

Established facts:
1. gpu-next does not work: libplacebo requires D3D11, the GPU has none —
   error 0x887a0004. Closed.
2. Zero-copy (dxva2 without -copy) is unreachable, all three contexts
   verified: win (WGL — no direct interop exists), angle (silent crash
   0xC0000005 in the interop code on the legacy driver; WER logs
   "faulting module: unknown"), dxinterop ("WGL_NV_DX_interop is not
   supported"). Do not re-probe.
3. Working combo: vo=gpu + gpu-api=opengl + gpu-context=win +
   hwdec=dxva2-copy. Decode runs in UVD2 in hardware, output is a real GPU
   path. INVALID_ENUM noise at texture creation is driver chatter, the
   picture is unaffected. Working-DXVA2 signature: the VO line
   "[gpu] 640x480 nv12" (a software decode would print yuv420p).
4. hwdec=auto is rejected: mpv probes d3d11va -> nvdec -> vaapi ->
   d3d11va-copy before reaching dxva2-copy — four device creations per
   start, including the noisy but by-design "Failed to create D3D11 Device
   (0x887a0004)". The explicit hwdec name removes the probes and makes the
   start deterministic.
5. "non-existing PPS 0 referenced" at connect is the mid-GOP norm: the
   decoder waits for the next keyframe, the messages die down by
   themselves.
6. The board drops the stream (~29 s and ~117 s in different runs) — an
   auto-reconnect loop is mandatory. ffmpeg reconnect* options do NOT
   apply to tcp:// (log-confirmed: "Could not set AVOption reconnect='1'"
   — the options exist for the HTTP protocol only); the reconnect is
   implemented as a bat loop or a Lua script, nothing else.
7. Start timing (3 runs per config, medians): ~0.73 s base; demuxer-
   lavf-analyzeduration=0 and demuxer-lavf-probe-info=no measure zero
   (even -15..20 ms) — MPEG-TS is identified instantly, acceleration
   options are useless. recv_buffer_size does not move the start either.
   The ~0.73 s is connect + wait for the next keyframe (board GOP) — not
   addressable from the player.
8. low-latency profile duplicates: explicit no-cache /
   demuxer-readahead-secs=0 / demuxer-max-bytes are redundant — the
   profile already carries cache=no, stream-buffer-size=4k,
   demuxer-readahead-secs=0 etc. (visible via --show-profile=low-latency).
9. Timing fork: --untimed is minimal latency but judders 25 fps on a
   60 Hz monitor (verified); --opengl-swapinterval=0 keeps the picture
   smooth, preserves PTS timing, costs +1-2 vsync frames. Chosen by the
   photo-timestamp measurement (below).

Launch. No config file is used (%APPDATA%\mpv\mpv.conf removed — all
options on the command line). C:\mpv64\watch.bat:

    @echo off
    :loop
    echo %date% %time% >> C:\mpv64\restarts.log
    mpv.com --vo=gpu --gpu-api=opengl --gpu-context=win --hwdec=dxva2-copy --profile=low-latency --network-timeout=5 --opengl-swapinterval=0 "--stream-lavf-o=recv_buffer_size=32768,tcp_nodelay=1" tcp://192.168.1.10:1234
    timeout /t 1 >nul
    goto loop

One-shot from PowerShell:

    mpv.com --vo=gpu --gpu-api=opengl --gpu-context=win --hwdec=dxva2-copy --profile=low-latency --network-timeout=5 --opengl-swapinterval=0 "--stream-lavf-o=recv_buffer_size=32768,tcp_nodelay=1" tcp://192.168.1.10:1234

Option notes:
- hwdec=dxva2-copy — hardware decode without probing; the copy at
  640x480 is negligible (fact 2).
- profile=low-latency — disables mpv buffering (incl. cache=no and the
  trimmed demuxer buffers; no separate lines needed, fact 8).
- network-timeout=5 — a hung socket (board silent, TCP not broken)
  unfreezes within 5 s instead of the FFmpeg default 60 s.
- opengl-swapinterval=0 — see fact 9; replace with --untimed if the
  photo-timestamp run shows the latency returned to 1.5-2 s (smoothness
  traded for minimal delay, at the cost of judder).
- stream-lavf-o=recv_buffer_size=32768,tcp_nodelay=1 — a trimmed socket
  receive buffer (insurance against Windows autotuning; measured: no
  effect, no harm either) and Nagle off.
- The bat loop IS the auto-reconnect: after each drop (EOF) a 1 s pause
  and a reconnect. Full picture freeze per drop ~1.5-2 s (1 s timeout +
  ~0.73 s start) — shrinkable only by a shorter board GOP.

bat nuances: stop the monitoring with Ctrl+C in the console window or by
closing it; q inside mpv triggers an instant restart (by design). The
echo line timestamps every drop into restarts.log — the per-run drop
frequency is read off it; remove the line if the log is unwanted.

Latency so far:

| Configuration            | Lag                                    |
|--------------------------|----------------------------------------|
| default mpv              | 1-2 s                                  |
| + low-latency            | 1-2 s                                  |
| + untimed                | ~1 s                                   |
| + swapinterval=0 (smooth)| UNMEASURED — photo-timestamp run pending|

The residual second most likely lives in the board encoder (GOP/buffers) —
not reachable from the player. Honest measurement: burn a timecode (or a
clock) into the board frame, photograph the monitor so both the stream and
the laptop clock are in one shot; the difference is the end-to-end latency,
~±50 ms accuracy. A --vo=null comparison separates the network+decoder part
from the output path. Player-side cross-check: ffplay -fflags nobuffer
-rtbufsize 100k tcp://192.168.1.10:1234 — a second there means dig into the
board settings.

Next latency front lives on the BOARD side (recorded 2026-10-31, folded
into the M3.9 follow-up):
- GOP 25 frames (1 s) or shorter — drives both the ~0.73 s start and the
  post-drop recovery time;
- B-frames off (low-delay / zerolatency in the encoder);
- UDP/RTP output instead of TCP, if the board can: a lost TCP packet
  head-of-line-blocks everything behind it until retransmission.

Diagnostic cheat sheet:

    # board port alive
    Test-NetConnection 192.168.1.10 -Port 1234

    # log folder (mpv does NOT create it)
    New-Item -ItemType Directory -Force C:\mpv64

    # decode+network without graphics
    mpv.com --vo=null --profile=low-latency tcp://192.168.1.10:1234

    # output without OpenGL (plan B)
    mpv.com --vo=gdi --profile=low-latency tcp://192.168.1.10:1234

    # start timing: the "playback restart complete" marker in the log
    mpv.com --vo=null --frames=30 --profile=low-latency --hwdec=dxva2-copy --log-file=C:\mpv64\log.txt tcp://192.168.1.10:1234

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

Constraints to remember:
- Zero-copy is impossible on this machine by construction; dxva2-copy at
  640x480 is practically indistinguishable by load — the copy is
  negligible, the decode is hardware anyway.
- dxva2 WITHOUT -copy is documented as "not safe" in mpv (timestamp
  corruption, crashes on broken drivers) — nothing is lost.
- untimed judders at 25 fps on a 60 Hz monitor (2-2-2-3 cadence); the
  smooth alternative is --opengl-swapinterval=0.
- A bare tcp:// without a container is sensitive to a dirty bitstream
  from the board: garbage bytes truncate the decode to the next
  keyframe — self-healing with a short GOP, not with a long one.

Two spots remain unverified: the actual latency with opengl-swapinterval=0
(the table row and fact 9 await the photo-timestamp run). Fill in the
number after the run and the record is fully closed.

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
