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

## Layers
- CameraV4L2 : MJPEG capture, mmap buffers, DQBUF/QBUF. G_FMT is verified
               against the requested mode; a driver-side silent mode
               substitution is a hard error.
- MppDecoder : MJPEG -> YUV422SP decode into an external dma-buf pool,
               returns the frame fd. Sized from Config, not hard-coded.
               Also hands out an aux dma-buf from the same pool for the
               multi-pass RGA path.
- RgaResizer : im2d resize by fd (wrapbuffer_fd + improcess). Two-pass path
               when the per-axis scale ratio exceeds the RGA3 limit (1/8),
               guarded against aux_fd < 0. All jobs are pinned to RGA3 core0
               via imconfig at startup.
- RknnEngine : rknn_create_mem + rknn_set_io_mem; the input tensor fd is
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
  (640x480, 1280x720, 1920x1080), although the UVC descriptor advertises
  30/1 in all modes. Forcing exposure (exposure_absolute=33) does not lift
  the ceiling. 1080p in daylight: not yet measured.
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
- int8-quantized yolov5s_relu_int8.rknn: calibrated on 50 night frames
  (--cal 50 --cal-interval 4), built with rknn-toolkit2 2.5.0 on Colab.
  MmseQuant2 is memory-hungry: 50 samples fit in 12 GB, 200 OOM. Outputs
  are int8 (per-channel scale/zp) — RknnEngine requests fp16 conversion
  via set_io_mem, the runtime dequantizes into the zero-copy buffer.
  Input: zp=-128, scale=0.00436174 (not 1/255 — scale was derived from
  the night-frame distribution), so raw RGB byte writes remain correct.
  A/B vs fp16 (daylight, 720p, conf 0.25): infer 43.1 -> 28.1 ms (~1.55x),
  classes and boxes match within a few px, conf drop ~0.02. Night
  re-check pending.
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
headroom does. The 47 ms figure motivated the yolov8n alternative — tested and closed
negative in M2-alt below.

## Scripts
- scripts/test_inference.py — RKNNLite smoke test: shapes, non-negativity
  (ReLU check), PASS/FAIL exit code.
- scripts/diag_zero_out.py  — multi-input diagnostic (noise/zeros/gray) for
  yolov5s_relu + a system mobilenet sanity check.
- scripts/probe_yolo.py     — output semantics probe on a real /tmp/cal.ppm
  frame (per-head box/obj/cls ranges, hot-cell counts). Feeds the model a
  4-dim input (img[None]).

## Build
    ./scripts/build.sh

## Run
    sudo LD_LIBRARY_PATH=/usr/local/lib ./build/light_ai \
        --device /dev/video20 --model ./yolov5s_relu.rknn --frames 300

Defaults: /dev/video20, 1280x720 MJPEG; the model default is still the M1
mobilenet_v1 relic — always pass --model for detection runs. Options:
--device, --model, --frames, --w, --h, --dump-cal, --conf, --iou,
--core 0|1|3|7 (NPU core pinning: 0=AUTO default, 1=core0, 3=0+1,
7=0+1+2; measured: infer 25.1->21.2 ms on 012, FPS unchanged —
source-bound, M2.5),
--cal N, --cal-interval K (--cal saves up to N model-input frames into
dataset/, one every K frames — the int8 calibration dataset collector;
one frame is ~1.2 MB, so 200 frames are ~240 MB on /tmp's storage).
Numeric values are strictly validated (garbage is an error, not a
silent zero).

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
- [x] M2.4: int8 quantization — infer 43.1 -> 28.1 ms (~1.55x), detection
      quality matches fp16 (same classes, boxes within a few px, conf
      -0.02); build recipe and caveats in Platform constraints. Night
      re-check pending.
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
      seen in every run incl. pre-patch smoke — benign so far, re-check
      in the night A/B.
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
- [ ] M4: pipelining (async RGA), NPU/DDR governor = performance. Only pays
      off once the source delivers more than 30 FPS (infer headroom analysis:
      even a 10-15 ms yolov8n@640x640 fits the frame budget without
      pipelining while the source stays at 20-30 FPS).
