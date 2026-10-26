# light_ai — zero-copy video analytics on RK3588

Pipeline: V4L2 (MJPEG) -> MPP decoder (YUV422SP, dma-buf) -> RGA (im2d resize
straight into the dma-buf tensor of the NPU) -> RKNN (zero-copy) -> CPU
post-processing.

## Rules of the project
- GStreamer is forbidden (owner's decision).
- A frame exists only as a dma-buf; pixel copies through the CPU are forbidden.
  The only allowed memcpy is the compressed MJPEG frame (~25 KB) into the
  decoder packet buffer.
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
  pipeline.cpp.

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
               encode (self-built ffmpeg 8.1), MPEG-TS. Transport:
               TCP pull by default (empty --net-host — the encoder
               listens on --net-port and the viewer connects to the
               board's LAN IP, so the run command carries no
               addresses); --net-host <pc-ip> keeps the legacy UDP
               push to that destination. The source is the NPU tensor
               AFTER box drawing: RGA crops the visible letterbox
               band (srect y=[lb_top, lb_top+lb_h)) and converts
               RGB888 -> NV12 1:1 into its own ping-pong MPP dma-bufs
               — no upscale, zero CPU pixels. The stream size is the
               band's native size (640x360 for the default 1280x720
               camera). One viewer at a time; no encode until the
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
  (640x480, 1280x720, 1920x1080) — pre-YOLO numbers; the 720p
  yolov5s_relu_int8 steady state with a viewer attached is ~26 FPS
  (see the 720p section below), although the UVC descriptor advertises
  30/1 in all modes. Forcing exposure (exposure_absolute=33) does not lift
  the ceiling — Manual exposure is a no-op on this camera, and FPS
  pacing is a discrete firmware state (see the camera facts
  below). 1080p in daylight: not yet measured.
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
  Detection mapping T: x_scene = x_det * cam_w / model_w, y_scene =
  (y_det - lb_top) * cam_w / model_w; detections on the bars (y_det outside
  the band) are filtered.
- The calibration dump file is created by whatever user runs the binary;
  a dump left by a root run cannot be overwritten by a normal user run
  ("calibration dump failed"). Clean it up between sudo and non-sudo runs.
- yolov5 .rknn outputs are native fp16 (float16, NCHW) — no CPU-side
  dequantization needed; the lite-runtime wraps them as float32.
- int8-quantized yolov5s_relu_int8.rknn: built with rknn-toolkit2 2.5.0
  on Colab. v1: calibrated on 50 night frames (--cal 50 --cal-interval 4);
  v2: recalibrated on dataset_mix (25 day + 25 night frames) —
  see INT8 vs FP32 validation. v3 (current; formal A/B done for night, day A/B pending):
  recalibrated on 60 fresh day-4 daytime frames with mean=0/std=255 ->
  canonical input scale 1/255 (0.00392157) — see M2.4.
  MmseQuant2 is memory-hungry: 50 samples fit in 12 GB, 200 OOM. Outputs
  are int8 (per-channel scale/zp) — RknnEngine requests fp16 conversion
  via set_io_mem, the runtime dequantizes into the zero-copy buffer.
  Input (v2): zp=-128, scale=0.00436174 (not 1/255 — fitted from
  the calibration distribution; confirmed present in the v2 model
  file, byte-level, 2026-10-26). Input (v3): zp=-128, scale=1/255
  (0.00392157, canonical mean/std config) — raw RGB byte writes
  remain correct for both.
  (Calibration caveat: the cal set embeds the gray-128 letterbox
  padding, ~44% of pixels per frame — its stats partly come from
  the fill; see dataset map.)
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
- h264_rkmpp rc_mode: the live encoder does NOT accept rc_mode (log line:
  "enc: rc_mode option not accepted"); the stream runs CBR at
  --net-bitrate (default 12000 kbps). Fine for the preview; revisit when
  bitrate control is actually needed.

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

## Scripts
- scripts/test_inference.py — RKNNLite smoke test: shapes, non-negativity
  (ReLU check), PASS/FAIL exit code.
- scripts/diag_zero_out.py  — multi-input diagnostic (noise/zeros/gray) for
  yolov5s_relu + a system mobilenet sanity check.
- scripts/probe_yolo.py     — output semantics probe on a real /tmp/cal.ppm
  frame (per-head box/obj/cls ranges, hot-cell counts). Feeds the model a
  4-dim input (img[None]).
- tools/save_board_csv.py   — RKNNLite predictions dump (board_preds.csv).
- tools/compare.py + tools/list_missed.py — INT8 vs FP32 comparison
  (IoU>0.5 + class match, mean |dscore|); see INT8 vs FP32 validation.
- tools/probe_dark_heads.py, probe_gamma_sweep.py, probe_adaptive.py,
  probe_gamma3_det.py — dark-frame probes (see Night session findings;
  gamma3 results in gamma3_det.csv).
- tools/peek_dark.py — renders the reference dark frames to PNG
  (dataset_night/, hardcoded triple, ignores CLI args); dark-band
  stats are printed separately.

## Build
    ./scripts/build.sh

## Run
    sudo LD_LIBRARY_PATH=/usr/local/lib ./build/light_ai \
        --device /dev/video20 --model ./yolov5s_relu_int8_v3.rknn --frames 3000 \
        --net --net-h264 --core 7

Defaults: /dev/video20, 1280x720 MJPEG; the model default is still the M1
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
      2026-10-24). STATUS 2026-10-26 (day session, v2 model): phase A
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
      NOT yet implemented: NetControl — the run is still configured
      by CLI flags, restart per change. Scope additions (owner
      request 2026-10-26): (1) NetControl priority raised —
      on/off/fps/quality (low/mid/high -> bitrate/fps mapping) +
      status, control TCP port next to the stream port; (2) owner
      decision: PULL model is the product, push-unicast stays an
      optional flag; (3) the early open questions are resolved by the
      same decision: transport = pull (push optional), port 1234,
      LAN trust model stands. RTSP phase B is the next transport
      target — h264, many viewers, standard player UX
      (rtsp://<board>:8554/live). NetControl commands
      (on/off/fps/quality/status) apply to whichever pull server is
      active. FrameTap full-frame path (publish_full_frame) sits in
      the code, clean stream without overlays, unused by the
      pipeline — reconcile during NetControl work.
      net/ family (design): NetControl (TCP listener, iostream,
      commands on/off/fps/quality — dynamic start/stop without
      restart), FrameTap (tap the full 1280x720 frame after MPP
      decode, before RGA letterbox; the frame goes to the encoder
      AS-IS — no RGA2 pass needed, RGA3 stays pinned to the model),
      NetEncoder (libavcodec h264_rkmpp wrapper — the self-built
      ffmpeg d90e3a1c18 has rkmpp ENCODERS, the hardware does the
      encode, zero-CPU like the MPP decoder; encoder accepts nv16 ==
      our YUV422SP, smoke-tested 720p lavfi: 60 frames, 20.4x at
      nv12 / 12.4x at nv16, valid MPEG-TS out; rc_mode VBR/CBR/CQP,
      qp_*, intra-refresh available — the LIVE encoder rejected
      rc_mode, see Platform constraints), NetSink (phase B: RTSP;
      phase C: WebRTC via libdatachannel only if remote access is
      ever needed). H.264 packets = second legal memcpy alongside
      the MJPEG packet. nv16 path still to be confirmed on live
      camera frames. Preview path is strictly opt-in, never part of
      the standard run or benchmarks. Priority vs M4: owner decision
      — if netsend is a working tool, it goes first (it is also the
      debug tool for M4).
      History (dated facts kept as recorded):
      - 2026-10-24: phase A landed as raw NUT over TCP:1234, not the
        designed h264 first (libavformat 62 from the self-built
        ffmpeg 8.1, ABI pin, see Platform constraints); the tap was
        the 640x640 model input — the designed full-frame tap exists
        now as publish_full_frame (unused, see above).
      - 2026-10-25: live streaming verified (continuous frames with
        YOLO overlays; the earlier "first frame only" issue gone).
        h264_rkmpp MPEG-TS/UDP landed as the tensor path: the
        encoder publishes the NPU input tensor AFTER GPU box drawing
        (syncCpu before publish), crops the visible letterbox band
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
        shutdown on the rkmpp-encoder path (unrelated to the known
        init assert) — watch on future runs. Seen again on the
        2026-10-26 pull run, still benign.
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
      fence sync; syncCpu() before publish so the CPU read (NUT path)
      and RGA read (h264 path) see finished pixels. Y-orientation:
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
      benign.

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
- [ ] M4: pipelining (async RGA), NPU/DDR governor = performance. Only pays
      off once the source delivers more than 30 FPS (infer headroom analysis:
      even a 10-15 ms yolov8n@640x640 fits the frame budget without
      pipelining while the source stays at 20-30 FPS).

- [ ] M5: multi-camera (proposal, owner decision 2026-10-26): 3x USB
      cameras from different room angles. Platform budget fits: 3x MJPEG
      ~3 MB/s on the shared USB2 half, MPP decode trivial. RGA needs core
      spreading — everything is pinned to RGA3 core0 today (imconfig);
      three single-pass 720p->640x640 jobs want RGA3 c0/c1 + RGA2E or the
      scheduler. NPU fits: ~3x current load is ~50% per core; pure infer
      is 15.4 ms/frame regardless of stream count — scheduling strategy
      (round-robin frames vs 3 RKNN contexts) is an open question.
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

## INT8 vs FP32 validation (2026-10-26, yolov5s_relu_int8_v2)

Purpose: verify the freshly calibrated int8 model (v2) against a float32
reference on identical inputs — quantization verdict for the M2.4 re-check
(path B). The v1 ALARM (box shift ~17 px, dconf -0.04) predates this
recalibration; this section documents v2.

Methodology:
- 50 calibration frames (n1_cal_*/n2_cal_*), 640x640, same gamma-2.0 -> uint8
  -> /255 preprocessing on both sides.
- FP32 reference: yolov5s_relu.onnx via onnxruntime (Colab), artifacts
  ref_preds.csv; INT8: RKNNLite on board, tools/save_board_csv.py ->
  board_preds.csv; comparison tools/compare.py + tools/list_missed.py
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

Dataset map (owner's clarification):
- dataset/        — DAYTIME calibration dump, day 4 (2026-10-26),
  60 frames cal_00XX.ppm. Pre-letterboxed 640x640 network input:
  gray-128 padding (rows 0-139 and 500-639, ~44% of pixels),
  content 640x360 centered. One static daytime view; people
  intermittently cross the scene, occluding the lower half
  (Rmean 78-82 -> 53-57 in occluded frames, upper half unchanged,
  k=1.0). Quantization input for v3 (built 20:20 same day).
  Replaces the former day-2 daytime dump.
- dataset_night/  — NIGHT calibration dump, day 1, the buggy
  pipeline that periodically produced row-tear artifacts.
  Hardest conditions for the model; all gamma probes ran on it.
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
[140,500) filter, results in gamma3_det.csv):
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

