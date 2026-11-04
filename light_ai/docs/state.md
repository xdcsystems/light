# State notes (light_ai)

The accumulating record that used to live at the end of README.md:
INT8 build history, INT8 vs FP32 validation and the night-session
findings (incl. the dataset map). Pointers like [Platform constraints],
Section pointers like [Platform constraints] or [INT8 build history] refer to README.md sections (../README.md); milestone IDs like [M2.4] or [M2.6] live in roadmap.md alongside this file.

## INT8 build history (rknn-toolkit2 2.5.0, Colab)

Versions in misc/ (build lineage):
- yolov5s_relu.rknn — fp16-convert build without quantization; the
  accuracy reference for every A/B.
- yolov5s_relu_int8.rknn — v1: default KLD quantization, mixed
  calibration; superseded (night ALARM — see M2.4).
- yolov5s_relu_int8_mmse.rknn / _v2.rknn — mmse, 50 frames
  (dataset_mix); the working model 2026-10-30 -> 2026-11-01,
  superseded by int8_mmse_v5 (see the v5 record below).
- yolov5s_relu_int8_v3.rknn — mmse. The build records disagree:
  one history notes a 6000-frame evening dump
  (cal_evening_6000.tgz), M2.4 records 60 fresh day-4 daytime
  frames with the canonical 1/255 scale (0.00392157) — the same
  filename was evidently reused across builds; treat the M2.4
  record as the current v3 state.
- yolov5s_relu_int8_v4.rknn — mmse on the fresh 960p daytime dump
  (dataset_day3: 50 frames, every 2nd of 100). Day, dusk and night
  A/Bs vs v3 — equal (2026-10-30, status below; night: one isolated
  frame loss, see below).
- yolov5s_relu_int8_mmse_v5.rknn — mmse on a MIXED calibration
  set: 30 night + 20 day frames (Colab build 2026-11-01; md5
  7c5656dd6aace3d186cc832c1501d8a8, verified on the board).
  The full A/B record vs v4 lives in Status below.

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
  are native fp16" note lives in README (Platform constraints) and refers to non-quantized builds.
- onnx source: yolov5s_relu.onnx, 28.9 MB, wget from
  ftrg.zbox.filez.com (link alive on 2026-10-30).
- Calibration dumps are duplicated to Drive; artifacts live in
  Drive (yolov5_rknn) and on the board (misc/). Colab-side
  files: postprocess.py (YOLOv5 decode: anchors 80/40/20,
  strides 8/16/32, NMS; input = 3 maps (255,H,W) post-sigmoid)
  and build_v4.py.

Status (2026-10-30, updated 2026-11-01): v3-vs-v4 A/B
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
  M2.4-b (a) is CLOSED 2026-11-01, see below.
- Night (dataset_mix, 50 frames, --gamma 2.0, conf 0.18;
  v3/v4/mmse): 102/106/98 detections over 48/47/49 of 50 frames —
  no night blindness in any version. IoU v3-vs-v4: 98 matched,
  7 v3-only / 8 v4-only (unverified one-offs: totals 102/106 imply v3-only = 4 at matched = 98 — recount from a fresh night A/B before citing), all in the 0.18-0.24 band EXCEPT ONE:
  n2_cal_0040 — v3 cls56 0.3574, mmse cls56 0.3808 (boxes agree
  within ~1 px), v4 sees NOTHING on that frame. A zone trace of
  that object across all 23 n2 frames: detected by v3 20/23,
  v4 19/23, mmse 20/23 — v4's miss is a single-frame quantization
  collapse, not systematic blindness (frames 0032/0072/0080 are
  missed by ALL three: edge-object absence, cf. M2.4). mmse stays
  the steadiest night profile (coverage 49/50, no lost detections).
- Verdict (2026-10-30, evening): v4 survives the night profile —
  v3-equivalent except one isolated lost detection. M2.6 (d) is
  closed as INT8-vs-INT8. The live night session (mmse vs v4)
  ran the same evening: EQUAL — mmse confirmed as the working
  model. The M2.4-b (a) day A/B vs fp16: CLOSED
  2026-11-01 (four-way; record appended below).
- Working-model decision (owner, 2026-10-30 evening): int8_mmse
  STAYS the working model — steadier at night (49/50 coverage, no
  losses) and proven across the live sessions. v4 is kept as the
  day/dusk candidate (fresher 960p calibration, higher dusk score
  0.491 vs 0.478). Closure (same evening): live night session done — mmse vs v4,
  600 frames each, conf 0.25, static scene, /dev/video20.
  RESULT: EQUAL. Steady blocks 120-599: no block diverges
  (2.02-2.05 vs 2.00-2.17 det/frame); chair 100% presence both,
  mean conf 0.827 vs 0.832; potted plant in every frame both,
  mean conf 0.670 vs 0.719; vase 4% frames both (mmse scores it
  higher: 0.413/0.571 vs 0.285/0.345); zero anomalous frames in
  either run. Infer on the night camera = ~22 ms, same as day —
  no night NPU penalty. First two blocks (warmup tail, vase
  fading at mmse start) are excluded, per M2.4 protocol.
- Conf-0.18 control (same evening, 240 frames each, static
  scene): cup and dining table ABSENT at 0.18 in both models —
  night absence, not a threshold effect. mmse again the
  conservative profile: vase in 14/240 frames (5%) vs 63/240
  (26%) for v4; weak (<0.30) off-profile detections 36 vs 162.
  Class-set divergence 70/240 frames, all driven by the
  0.18-0.26 noise band (vase episodes, phantom potted plant at
  ~(471,578)); main profile (chair + potted plant) intact in
  both. v4 note: empty frame 0 (cold-start miss) — second
  early-frame collapse pattern after M2.4 v3/vase. Decision
  unchanged: mmse stays the working model.
- Day A/B vs fp16 (2026-11-01, M2.4-b (a) CLOSED — four-way on
  the restored 960p scene: v3, mmse, v4, fp16 reference; 600
  frames each, conf 0.25, static scene. Bottle and bowl are
  new scene objects — the composition differs from the
  pre-weekend day records; internal A/B validity intact):
  * Performance: fp16 infer 29.7-30.4 ms vs ~22.06 int8 (+35-38%, clean repeat 11-01);
    the pipeline stays camera-bound at ~25 FPS for all four (a pacing level, cf. 20 on the 10-30 960p runs — camera switches levels freely) —
    the fp16 reference costs nothing end-to-end.
  * v3 tracks the reference tightly: chair 0.868 vs 0.868,
    bottle/bowl within 0.02, vase 29% frames vs 50% — the only
    int8 line that follows fp16 on small objects.
  * mmse-branch signature (mmse and v4 share zp/scale
    0.00436174): bowl under-scored 0.326/0.329 vs 0.480
    (identical gap in both), vase near-absent 3%/1% vs 50% —
    calibration-branch properties, not per-model quirks.
  * cup recall: mmse 90% frames vs 13% fp16 (v4 54%, v3 6%),
    same real box ~(470,639) — the mmse score band 0.26-0.34
    holds above the 0.25 threshold where fp16 dips below.
    Recall boost, not a phantom.
  * v4 extras: bottle 0.497 (lowest of the four, -0.06 vs the
    reference); cold start weak but not empty (frame 0: one
    chair det 0.75 vs six dets at fp16) — the empty-frame-0
    pattern did not repeat on the day scene; zero anomalous
    frames.
  * One-offs: v3 single-frame cls13 phantom (0.358); fp16 four
    phantom-burst frames (63, 68, 69, 556; the (456,564)
    region) driving its block wobble 4.45-5.33 det/frame.
  * VERDICT: the night equivalence (mmse == v3) does not carry
    to the day scene with small objects. Two int8 lines with
    distinct trade-offs: v3 = fidelity (closest to the
    reference, loses cup), mmse/v4 = recall (cup boost,
    bowl/vase cost). Owner flag: small-object fidelity
    matters -> v3; recall on real small objects matters ->
    mmse.
  * OWNER VERDICT (2026-11-01): mmse STAYS the working
    model — night stability (49/50, no losses) outweighs
    the day bowl/vase score dip (presence intact at 100%);
    the cup recall boost is a free win. v3 stays the
    fidelity reference among int8 lines. M2.4-b (a) fully
    closed.
- v4-vs-v5 A/B (2026-11-01 evening, tools/ab_run.py, conf 0.18,
  static files): night (dataset_night, 200 ppm, gamma 2.0) —
  v4 385 / v5 364 detections; median conf 0.443 / 0.474; the whole
  delta sits in the 0.18-0.3 noise band (113 -> 95), upper bands
  identical (0.5-0.8: 160/161, >=0.8: 17/17); cls 0 and cls 39
  counts identical (67/67, 38/38); per-frame coverage 178/200
  common, 1 frame only-v4, 1 only-v5. Day (dataset_day3, 100 ppm,
  gamma 1.0) — v4 403 / v5 414; 376 detections IoU-matched (>=0.5)
  with the same class on both sides — ZERO class flips; unmatched
  27 (v4) / 38 (v5), all threshold-edge noise. VERDICT: v5 keeps
  every confident detection on both scenes and sheds the night
  noise tail. Working model switches to int8_mmse_v5 (owner call,
  2026-11-01 evening) — this supersedes the mmse call above
  (same day). M2.6 follow-up: none required; day re-check done.
- fp16-reference check, v5, NIGHT half (2026-11-01 evening, live
  camera /dev/video20, 1280x960, one static scene, same session,
  600 frames each, conf 0.25, --core 7, viewer attached,
  box-log: misc/v5_live_night.csv, misc/fp16_live_night.csv):
  the scene gained objects since the earlier sessions — a vase on
  the table and bottles were restored before the runs. An earlier
  same-evening pair WITHOUT the vase (and with an inconsistent
  scene across the pair) is discarded — protocol violation, not
  recorded. Results (sampled every 15th frame, confirmed by eye
  against the logs):
  | object         | v5 (int8)      | fp16           | delta   | presence   |
  |----------------|----------------|----------------|---------|------------|
  | chair          | 0.84-0.86      | 0.86-0.89      | ~-0.03  | 100%/100%  |
  | potted plant   | 0.50-0.73      | 0.51-0.75      | parity  | 100%/100%  |
  | bowl           | 0.58-0.67      | 0.70-0.76      | ~-0.07  | 100%/100%  |
  | vase           | 0.26-0.39      | 0.28-0.41      | ~-0.01  | ~93%/100%  |
  | dining table   | 0.26-0.35, intermittent | 0.27-0.37, intermittent | parity | flickers in both |
  Boxes agree within ~3 px on every object; no empty frames in
  either run. VERDICT: v5 == fp16 on the night profile. The
  mmse-branch small-object signature reproduces SOFTLY (bowl
  underscored ~-0.07 — the same direction as the day four-way,
  but presence stays 100% with a wide margin above 0.25; no vase
  underscoring at night). The 3/40 vase drops of v5 sit ON the
  0.25 threshold (non-consecutive, conf dips to ~0.26 before the
  drop) — edge flicker, far from the v2-ALARM level (-0.045
  systematic + vase never caught). Timing: infer 23.9-24.6 (v5)
  vs 29.4-30.5 (fp16) — the int8 gain ~20% live; sum/FPS
  identical (~39.8 ms, ~25.2 FPS) — source-bound. Notes: (a)
  CORRECTED by visual check (stream screenshots, same evening):
  the wide low object at x~378 (62x37 px), detected as "bowl" by
  both models in 100% of frames, is the BLUE VASE (bowl-shaped,
  COCO bowl/vase ambiguity) — so the mmse small-object signature
  above applies to the VASE object: v5 med 0.636 vs fp16 0.727,
  ~-0.09, presence 100%/100%, same direction as the day four-way
  (bowl 0.326 vs 0.480), shallower. The tall narrow object at
  x~444 (24x69 px), labeled "vase" in the table above, is
  actually the BOTTLE — class confusion vase<->bottle; it is at
  parity between models. fp16 "bottle" records (4 total, 1% of
  frames, x~446) are duplicate boxes of the SAME bottle — class
  flap at the threshold, not a separate object. The true labels:
  "bowl" row above = blue vase; "vase" row above = bottle; (b) a single enc=4.8 ms
  block in the fp16 run (FPS dipped to 23.6 for one block, next
  block back to 39.7/~25.2) — one-off, not a pattern; (c) the
  fp16 startup log carries "net: tensor stride check:
  size_with_stride=2457600 vs 1228800" absent on int8 runs (fp16
  tensor attr vs the RGB888 buffer) — run is clean, note for a
  possible guard revisit, low priority. DAY half of the check:
  PENDING (next daylight session; plan: v5 + fp16 + optionally
  v4, 600 frames each, same protocol) — see the roadmap M2.6
  tail.

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
(matched % = matched / FP32 dets; missed = FP32-only detections, extra = INT8-only)

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
  dark-frame probes point HERE, not to the daytime dumps.
- dataset_mix/    — calibration dump assembled proportionally
  from day-2 and night-1 frames; THE calibration set of
  int8_v2 / int8_mmse (50 frames: n1_cal_* every 2nd of 50,
  n2_cal_* every 8th of 200); v3's calibration set is separate —
  60 fresh day-4 daytime frames (see M2.4). v5's mixed set
  (30 night + 20 day, 2026-11-01) is separate as well.
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


## NPU pool probe (2026-10-31): 3-context pool scaling (M5 groundwork)

Purpose: verify that N contexts of one model pinned to NPU cores
0/1/2 actually scale (M5 pattern from README) before it becomes a
design constant; also measure the yolov5m outlook. Disposable probe:
tools/bench_npu_pool.cpp (iostream-only, zero heap in the loop,
statics-exempt as a tool). Built directly with g++ (not via the
project build script).

Method:
- One model (misc/yolov5s_relu_int8_mmse.rknn), 3 rknn_contexts.
- Weight sharing: worker ctx 0 via rknn_init, workers 1-2 via
  rknn_dup_context — confirmed working on the board toolchain
  (duped contexts run independently, see core loads below).
- Inputs: zero-copy via rknn_create_mem + rknn_set_io_mem
  (size_with_stride), one-time gray-128 fill (bench-only, not a
  frame path). Outputs: zero-copy, 3 per context.
- Core pinning: rknn_set_core_mask, RKNN_NPU_CORE_0/1/2, one
  context per core.
- Timing: pure rknn_run loop, steady_clock, 10-frame warmup per
  context untimed; governors set to performance beforehand
  (set_governors.sh: cpu perf, npu 1 GHz, dmc 2.4 GHz).
- Two runs, 600 frames per phase each; parallel observation:
  /sys/kernel/debug/rknpu/load in a second console.

Results:
- serial (core0 only): 22.53 / 22.50 ms/frame (two runs).
- pool (3 workers, one per core, parallel):
  9.39 / 7.86 ms/frame effective; scaling 2.40x / 2.86x.
- Core load during pool phase: ~67% on ALL THREE cores — duped
  contexts do spread across cores (hypothesis "all contexts land
  on core0" refuted live; cf. M2.5 mask note: --core 7 mask
  gained <=2.5 ms — the difference is explicit per-core pinning).

Findings:
- serial pure-infer (22.5 ms) vs prod steady-state (29.5 ms):
  the ~7 ms delta is DDR contention with live neighbors
  (cam/dec/rga/enc) — both numbers are correct, they measure
  different surroundings. For M5 planning, use the prod figure.
- Scaling 2.4-2.86x matches community practice (pool of contexts,
  e.g. rknn-cpp-Multithreading: ~2.4x on yolov5s; 3.0x is not
  reachable — DDR-bound, cores sit at ~67%).
- Toolkit note: the board rknn_api.h is 1.x-era — no
  RKNN_QUERY_NUM_OUTPUT; output count goes through
  RKNN_QUERY_IN_OUT_NUM (rknn_input_output_num.n_output).
  rknn_init with size=0 = model path-as-string (our first run
  passed the path as a buffer — "rknn model is too small").

yolov5m outlook (estimate, not measured): m-infer ~85 ms/core
(x2.9 FLOPs vs s) -> effective ~30-35 ms/frame at the measured
2.4-2.86x scaling -> ~20+ FPS steady within the same ~20 ms
non-infer budget. The single-context m-projection (~10 FPS) is
therefore beaten by the pool. Remaining price: +2 frames latency
(~100 ms) — owner's decision, pending the end-to-end photo
latency measurement (below).

Open items:
- End-to-end latency baseline BEFORE any M5 integration: photo
  method (millisecond timer + viewer on one screen, 20-30 shots,
  median + p90; viewer is networked — ping stability check first,
  see conversation notes). Re-measure after M5 to verify the
  predicted ~+100 ms.
- bench_npu_pool.cpp: currently built ad hoc; either promote to a
  CMake tool target or keep as disposable (decide on next tools/
  pass).

## Pool oracle verification (2026-11-01): M5 pool correctness vs RKNNLite

Tool: tools/oracle_check.py (README, Scripts). Inputs: --dump-cal PPM
dumps + the run's --box-log CSV; verdict PASS/FAIL. The oracle runs the
pipeline built-in model (misc/yolov5s_relu_int8_mmse.rknn; v5 untouched)
in a separate RKNNLite instance, decodes with tools/postprocess, converts
to scene coordinates and matches the box-log "raw" lines of the SAME run.

Calibration chain (each link verified independently):
- feed recipe: raw uint8 + img[None], NO data_format kwarg (the
  save_board_csv.py path); RKNNLite silently returns None on a 3-dim
  input or a rejected kwarg — see README [Platform constraints],
  RKNNLite note.
- harness anchor: the CAL variant (v2 + gamma 2.0 on a dataset_mix
  frame) reproduces misc/board_preds.csv to the 4th decimal
  (cls 56, conf 0.8585, box 216.9/392.2);
- geometry: 960p working mode, s = 0.5, lb_top = 80 — measured live
  by scanning each dump for the gray-128 letterbox rows (80 top /
  80 bottom), not taken from config.

Dump/frame mapping (learned the hard way): --dump-cal numbering
follows the FRAME counter, not the dump slot — with --cal-interval 40
the three dumps of each run are frames 0/40/80 of that same run.
Best-frame matching (per-dump argmax over the log frames of the mean
class-matched IoU; scores 0.996-0.999) makes the oracle robust to any
--cal-interval.

Results (both PASS):

| run | matched | unmatched | phantoms | max px delta | max conf delta |
|---|---|---|---|---|---|
| base (600 frames, cal-interval 40) | 7/8 | 0 | 0 | 0.45 px | 0.0002 |
| pool (3 workers, same protocol) | 9/11 | 0 | 0 | 4.18 px | 0.0003 |

Reference boxes = the run's own box-log lines on the matched frames;
unmatched counts only confident boxes (BORDER_CONF = 0.35).

Residuals, all one class: cls 60 (dining table) at conf 0.26-0.28 —
the box is present in the NPU raw log, absent in the Lite decode.
Attribution: fp16 dec-graph divergence at the 0.25 threshold edge,
not quantization and not the pool. No confident detection was ever
unmatched; zero phantoms on both runs (the pool's core claim: no
spurious detections).

Verdict: the M5 worker pool is detection-correct. On the matched
frames the pool output reproduces a single-context NPU path to
dconf <= 0.0003 with IoU >= 0.94 — deeper than the NPU-vs-Lite gap
itself. Throughput (CORRECTED 11-01 late by the pool1/pool3 wall-clock A/B):
pool throughput is UNCHANGED vs pool1 (source-bound ~20 FPS both);
45.9 ms is the in-flight window latency (~3x15.2), not per-frame
cost.

Open notes:
- oracle TODO: class-aware NMS on the decode side — not needed at the
  current agreement level; revisit if scenes produce cross-class
  overlaps near the threshold.
- the oracle covers the dump window (3 frames per run by default);
  for routine checks raise --cal / --cal-interval as needed.
- pool-mode rga timing: the cal-dump file I/O is counted inside
  rga_ms (ms(t3,t3b)), unlike the single path — irrelevant without
  --cal, keep in mind for pool rga A/Bs.
- known accepted races (no live path reaches them today): the
  destructor drains the ring only via run()'s tail; and ts_base_
  reset in serve() vs send_frame() read (int64 write, benign).

## Pure-infer figure registry (reconcile, M2.6 (c))

Working formula: prod_shown_infer ~= pure_infer + DDR contention +
pacing-stall share. Isolated bench numbers with performance governors
are the pure figures; prod steady-state [timing] figures are NOT pure
(they carry the pacing stall inside 'infer').

| Figure | Source / tool | Model, era | Conditions | Date | Status |
|---|---|---|---|---|---|
| 15.67 ms @7 | tools/bench_npu | int8, 960p | isolated bench, quiet | 11-01 | RECONCILES 15.4 @012 (10-25, 720p era): reproduced within 2%, resolution-independent; 3-core parallel gain 1.40x real (15.67 vs 22.06 @1); M2.6 (c) closed; pipeline-side mask gain <=2.5 ms stays a separate M5 integration note |
| 18.1 / 22.0 @2/1 | tools/bench_npu, same run | int8, 720p era | same | 10-25 | scaling data only |
| 21.2 ms @012 | M2.5 steady [timing] | int8 (v2 era), 720p | in-pipeline; CPU-governor-dependent (degrades to ~25.1-26.0 after idle) | 10-26/27 | OLDER SETUP - do not cite outside M2.5 context |
| 25.1 ms (AUTO) | M2.5 steady [timing] | int8 (v2 era), 720p | in-pipeline | 10-26 | OLDER SETUP |
| 22.5 ms (serial, core0) | tools/bench_npu_pool | int8_mmse, 960p | isolated bench, governors perf | 10-31 | CURRENT PURE REFERENCE (960p era) |
| 29.0-30.3 ms | 960p section, steady [timing] | int8 mmse/v3 | prod, viewer attached | 10-30 | PROD steady-state (not pure) |
| ~29.5 ms | pool-probe derivation | int8_mmse, 960p | prod | 10-31 | PROD steady-state = pure 22.5 + ~7 ms DDR contention |
| 30.0 ms (--core 7) | mask A/B, steady | int8 mmse, 960p | prod | 10-30 | PROD steady-state |
| 22.07 ms | night session [timing] | int8_mmse | prod, night | 10-30 | VERIFIED clean repeat (bench_npu 200 iters, cores=1, quiet 11-01 12:51, dataset_mix frame); session 24.0 ms = noisy read; mmse ~= v3 (22.07 vs 22.06) |
| 22.06 ms | day A/B [timing] | int8, 960p | prod | 11-01 | VERIFIED clean repeat (bench_npu 200 iters, cores=1, quiet 11-01 12:49); morning 24.5 ms = noisy first read; prod delta +34% (29.5 vs 22.06), "~7 ms DDR" note consistent |
| 45.9 ms per 3 in-flight frames | M5 pool live A/B | int8_mmse, 960p | prod, 3 workers | 11-01 | CORRECTED 11-01 late: this is the in-flight WINDOW LATENCY (~3x15.2), not a per-frame cost; wall-clock throughput pool3 == pool1 (600 frames: 30.73 vs 30.60 s, both source-bound ~20 FPS; user-CPU 18.6 vs 10.5 s — 3 threads busy); [timing] sum/FPS overcounts in pool mode (the cam wait of frame k overlaps the compute of k-2, ~46 ms double-counted) — take FPS from wall clock until the accounting fix |
| NPU 10/9/10% per core | pool live, debugfs rknpu/load | int8_mmse, 960p | prod, 3 workers | 11-02 | VERIFIED live: three cores balanced under round-robin dispatch; total inference budget same as single-context |
| CPU ~8% user (vmstat) | pool + h264 stream live | — | prod | 11-02 | VERIFIED live: whole pipeline incl. encoder muxes on ~8% user CPU across cores; consistent with 18.6 s/600f registry row | 
| GPU practically idle | debugfs mali0 dvfs_utilization | — | prod | 11-02 | VERIFIED live: busy/idle ticks never fire (batch draw per frame is below the DVFS tick granularity), cur_freq stays 1 GHz in performance; boxes render (visual confirmation) — counter-blind, not draw-dead |
| -mcpu=cortex-a76 -mtune=cortex-a76.cortex-a55 | CMakeLists TARGET_CPU_FLAGS A/B | int8_mmse_v5, 960p | prod, h264 pull, viewer attached | 11-02 | NO EFFECT vs -mcpu=cortex-a76: sum 95.2-95.8 ms / ~20.0 FPS both — camera-bound, inference lives on the NPU. Kept as documentation of intent. The dot form is legal only in -mtune (GCC 10.2 rejects -mcpu=cortex-a76+cortex-a55: "+" there means a feature modifier) |

## Shutdown observations (2026-11-02): MPP prints on the graceful exit

Two post-P1-6 live runs, both Ctrl-C with a viewer attached:
- Run A (3000 frames, post-P1-5 binary): clean exit — the last MPP
  line is the normal "mpp_buffer: mpp_buffer_service_deinit cleaning
  misc group"; zero warnings.
- Run B (1021 frames, the CPU-flags rebuild): stderr carried
  "mpp_meta: put_meta invalid negative ref_count -1" and
  "mpp_mem_pool: mpp_mem_pool_put invalid mem pool ptr ...
  caller mpp_packet_deinit" before the same service_deinit line.
Working hypothesis (undiagnosed): a packet double-deinit on the
shutdown path, phase-dependent on where Ctrl-C lands relative to the
encoder flush / decoder state (run A hit a different phase). NOT the
M3.5-era ref_count warning (that was the mpp_buffer path, closed by
the EOS fixes) — this one is the mpp_packet/meta path. No crash, no
other symptoms; the "Processed N frames" line and the exit are clean
in both runs.
Backlog: P2 candidate "shutdown-path double packet_deinit" — needs
src/mpp_decoder.cpp + src/net_encoder.cpp teardown review. Not
reproducible on demand (phase-dependent; try Ctrl-C at different
points of the run and record which phase produces the prints).

## Pipeline unification (2026-11-03): unified slot-ring path (M4 follow-up)

Three commits over origin/master: 50ff7d4 (FrameTrace), 4b50a36
(unified path), 11d6d67 (cleanup).

- FrameTrace: per-frame timing record (cam/dec/rga/infer/enc) replaces
  the four bare doubles in Slot and the positional arguments of
  Timings::add. Ownership: dispatch fills cam/dec/rga, the worker fills
  infer, publish fills enc and hands the whole record to Timings::add.
  Verified live: [timing] format and dec/rga/infer/enc values identical
  to the 11-01/11-02 baselines; cam differs only by daylight (night
  ~46 -> day ~35, wall 20 -> ~25 FPS — the VFR follow-source pacing
  documented in Config, not a refactor effect).
- Unified path: pool 1 == ring of one. dispatch() then an immediate
  publish_oldest() that blocks on the worker's DONE — the legacy
  synchronous semantics with a single code path. The inlined
  single-context block is GONE (diffstat -168/+29); the drain loop lost
  its npool_>1 gate. pool 3 keeps publish-before-reuse unchanged.
- ASYNC VERIFIED live (11-03, pool 3): cam ~35-36 ms alongside infer
  ~45.5 ms and wall ~25 FPS — the pipeline thread does not wait on
  inference; three frames in flight, source-bound, as before.
- NEW FINDING (11-03): pool 1 with --core 7 (mask 0x7 = all three
  cores) splits the model across NPU cores -> infer 22.5-23.5 ms/frame,
  vs pool 3 where each engine is pinned to ONE core and takes ~45.5 ms.
  Both modes are camera-bound (~25 FPS wall identical); the difference
  is LATENCY: pool-1 sum ~39.6 ms (honest CFR accounting) vs the pool-3
  in-flight window ~85 ms. Best low-latency mode: --pool 1 --core 7.
  PENDING: reconcile vs the 10-30 registry row "30.0 ms (--core 7)"
  (see note below) and add a pure-figure row on the next bench pass.
- Accounting note: on the unified path pool-1 infer is measured in the
  worker thread after wake (rknn_run + postprocess, no pacing stall,
  wake-up excluded). The prod figure landed near the pure serial bench
  (22.5 ms, 10-31) instead of the old ~30 ms prod row — consistent with
  the DDR-contention model but not yet reconciled; do not cite the two
  eras against each other until the bench pass.
- Cleanup (11d6d67): dead dets_raw_ member removed (its only writer
  died with the single path); stale "single-context" comments fixed
  (Config pool note, npus_ header note, core-pinning note); duplicate
  letterbox comment collapsed. Verified by a 600-frame pool-1 run:
  steady sum 39.4-39.8 ms, viewer EIO handled cleanly mid-run
  ("viewer gone, waiting for the next one"), Ctrl-C graceful shutdown
  at frame 567 with correct TOTAL.
- Logging gotcha (not a bug, verified 11-03): stdout through a pipe is
  block-buffered; MPP deinit lines on stderr (unbuffered) can appear
  to interleave MID-log between stdout chunks (PID check: our own
  process). Direct terminal output preserves order (deinit strictly at
  the end). When order matters: run direct, or split stderr to a file.
- clang-tidy 11929 (src/mpp_decoder.cpp, started 11-02) hung >14 h
  with ~zero CPU; killed 11-03. Re-lint pending on the updated
  pipeline/mpp sources.
- CORRECTED (11-03, same day): the "SPSC queue refactor" naming in
  the first draft of this note was wrong. The publish FIFO
  (pub_slots_/pub_head_/pub_n_) has no producer/consumer thread split:
  dispatch, publish_oldest, drain and pool_discard ALL run on the
  pipeline thread, so it was never a concurrency problem — just three
  loose fields. It is now a plain PubFifo struct (fixed depth 3,
  push/front/pop, no locks by design).
- CONSIDERED AND REJECTED (11-03): lock-free slot-state handover
  (QUEUED->DONE via atomics instead of the slot mutex + cv). Rationale:
  measured contention is negligible — the worker runs rknn_run and
  postprocess WITHOUT holding the slot mutex by design, so the mutex
  is only touched on state transitions; the gain is unmeasurable while
  the correctness risk (memory barriers, slot-reuse races, interaction
  with the drop/bounded-wait/pool_discard handover) is real. Recorded
  so a future "optimizer" finds the verdict instead of re-deriving it.

## Encoder lock split, mux decoupling and viewer gating (2026-11-03, evening)

Two commits after the pipeline unification: dbc6fbc (batch B) and
6535c60 (batch C).

Batch B (dbc6fbc): publish() held mx_ across the draw-fence poll
(worst case ~200 ms under the lock) and the RGA crop. prep_frame()
now runs on the pipeline thread before the lock; the critical
section covers only encode + mux. Readiness is gated by the atomic
active_ set by serve() under mx_. A/B vs the stashed baseline on
the same scene: identical timings (pool 1 ~49.5 ms, pool 3 ~95.4
ms) - the split is semantic, not a speed change.

Batch C (6535c60):
- Mux decoupled from the pipeline thread: send_frame clones each
  received packet into a 3-deep queue (pkt_mx_/pkt_cv_, separate
  from mx_ so network wakeups never touch the encode critical
  section; drop-oldest with a rate-limited note) and serve()
  drains it into the muxer. The pipeline thread never touches the
  network. pts is assigned in send_frame BEFORE the queue, so mux
  latency cannot shift VFR timings. send_frame() no longer takes
  the muxer context.
- Viewer gating (the standing rule: output only for a connected
  viewer, recognition never gates): publish_oldest() checks
  enc_->viewer_active() (new getter over active_) and
  preview_->viewer_connected() (new getter over connected_)
  before the GPU draw pass. With no viewers the draw+fence work
  is skipped and the encoder gate drops the frame. Both checks
  are per-frame atomics; a late joiner misses at most one frame
  of boxes. hdr_ capture moved with the mux into serve().

Verification (2026-11-03 evening):
- No-viewer run: no "header captured"/"viewer connected" lines,
  enc = 0.0 in every timing block, wall recovered to ~24-25 FPS.
- Viewer kill mid-run: "write failed: EIO" from the drain loop,
  "viewer gone", timing blocks on both sides smooth; no
  "packet queue full" events. Header repeat (39 bytes)
  reproduced from the drain loop.
- Reference in LIGHT conditions, both patches in: pool 1 sum
  39.5-40.1 ms, ~25 FPS, enc 0.0 (no viewer) / <=0.8 (viewer).
  All 2026-11-03 evening numbers are on the bottle-in-scene
  composition; cross-session comparisons need the same scene AND
  the same light (see the lighting note below).

LIGHTING AS A TEST VARIABLE (2026-11-03, corrective):
- Autoexposure ties the source frame rate to scene brightness:
  in darkness the source sat at the ~20 FPS pacing level and sum
  rose to ~49.5 (pool 1) / ~95.4 (pool 3); after the lights were
  turned on the SAME scene and pipeline give sum ~39.6-40.1 and
  wall ~25 FPS. The midday "regression" hunt (the bottle was
  blamed) was wrong - lighting explains it; the bottle
  explanation is superseded.
- The infer drop ~29 -> ~22 ms with the light on is not
  explained by the scene itself (int8 inference should not
  depend on frame content); working hypothesis: DVFS/governor
  pacing with rarer camera frames in the dark. UNVERIFIED; do
  not cite the two figures against each other until understood.
- Protocol note for future sessions: record the light state per
  run (or fix exposure) before comparing timings.

TIDY PASS (0693149, 2026-11-03 evening):
- mpp_decoder: NULL -> nullptr in 7 task-meta calls
  (modernize-use-nullptr); no behavior change.
- net_encoder: serve() exceeded the tidy size limit after
  batch C. Split: drain_queue() + mux_write() own the mux side
  (wait/pop loop, per-packet write with the IDR-prepend logic);
  run_session() owns one viewer-session iteration (transport
  setup, stream setup, teardown), moved verbatim with
  continue;/break; -> return;. serve() keeps the loop, the
  tail print and the 500 ms UDP backoff: ~270 -> 37 lines,
  run_session() 193, limit 200.
- Two uniform error-path deltas (never observed live):
  stream-setup failures share one 500 ms backoff, and the
  write_header path sleeps before the teardown.
- Verified: build clean, tidy clean on both files, viewer
  smoke (full run: connect -> header captured 39 bytes ->
  ~25 FPS -> clean EOF; kill run: write failed -> viewer gone
  -> enc=0.0 -> clean EOF, pipeline unaffected after the
  disconnect).

CONSIDERED AND REJECTED - tidy size limit on the hot paths (2026-11-03):
- run() (pipeline.cpp) and yolo_postprocess() also exceed the
  200-line limit. Verdict: NOT split. Both are hot-path
  monoliths where a mechanical split risks correctness for a
  stylistic check (fifo ordering, tensor-band addressing,
  cache-local loop structure); the net_encoder split was safe
  because the moved code was self-contained per session.
  Revisit only if either grows and gains a natural seam.

TIDY INFRASTRUCTURE (2026-11-03):
- build-tidy compiles without the board's -mtune=...cortex-a55
  (clang 11 rejects the dotted core-name form; tidy is a
  diagnostics pass, not codegen - board flags stay in
  build.sh).
- tidy_run.sh runs clang-tidy per file with per-file timeouts,
  logs under /tmp/tidy/.
- Patch-scripting lesson from this session: brace/count
  asserts must COMPUTE their delta from the patch strings, not
  carry a remembered constant - a stale constant failed twice
  before the computed form passed on the first try.

READABILITY PASS (post-tidy, 2026-11-03 evening):
- The pts ternary in send_frame (CFR/VFR with a hidden ts_base_
  side effect) -> packet_pts() guard-clause ladder; the
  receive_packet drain in send_frame -> pre-call + while
  ( ret >= 0 ) + post-loop error check; for(;;) headers replaced
  with real conditions where the exits are named: drain_queue ->
  while ( !quit_ && !error_ ), worker_loop -> while ( !quit_ ).
- camera: dequeue's poll EINTR retry folded into poll_ready()
  (xioctl-style); dequeue header while ( !have ) with named
  exits; teardown index loop -> range-based for (tidy
  modernize-loop-convert; first camera tidy pass, stale
  pre-patch finding).
- Verified: build clean, tidy clean on net_encoder/camera/
  mpp_decoder, viewer smoke: header 39 bytes, sum 39.4-39.8
  ms ~25 FPS, clean EOF; pts path unchanged.
- Patch-scripting lesson, second half: brace braces '{}' of
  declarations count too - a hand-written delta missed the
  initializer and the computed check failed before write.
  Rule extended: no hand-written counters at all; every delta
  comes from counting the patch strings.


## Trail session (2026-11-04): M3.11 verification, exit Bus error, tracker notes

- GitHub incident (2026-11-04): a git push failed for ~1 h with
  "Permission denied (publickey)" on BOTH ssh 22 and ssh.github.com
  443 while the same key worked minutes earlier -- githubstatus.com
  carried "Incident with Pull Requests, Git Operations and Issues",
  Git Operations Degraded (09:49-11:00 UTC window). The local SSH
  setup was innocent; the push succeeded once the incident cleared.
  Protocol: with an unchanged key and a sudden publickey refusal,
  check githubstatus.com FIRST.
- Bus error on exit (OPEN, deferred by owner decision): one
  3000-frame run (h264 pull; viewer connected ~f60, gone ~f1020)
  printed "Bus error" AFTER "Processed 3000 frames" -- teardown
  phase, past the main loop. NOT reproduced by: a 300-frame run
  with no viewer (exit 0, clean mpp_task_queue_deinit) and a
  1200-frame run with viewer attach+detach (clean drain, EOF
  reached, exit clean). dmesg: no Mali/dma-buf faults. Hypothesis:
  phase-dependent teardown race (encoder/mux vs main teardown),
  same family as the shutdown-observations P2 item. Backlog:
  reproduce under gdb (or with a core file, ulimit -c unlimited)
  on a 3000-frame run with a viewer detach; record `echo exit: $?`
  per run.
- Tracker phantom velocities (OPEN, cosmetic): during miss streaks
  Kalman coasting lets cx/cy/vx/vy run far off-frame (observed
  v=-926.76 px/s, c=(-6.9, 479.3); id churn 1->3->5->7->9->...->21
  across the 3000-frame run -- a returning person fails the IoU
  match and gets a new id). The trail is unaffected
  (out-of-tensor points are clipped at the border check). Backlog:
  check whether Traj::push runs during coasting (grep 'traj\.|.
  push' in tracker.cpp); if yes -- push on measure only; consider
  a velocity clamp / earlier give-up after N misses.
- Trail verification (M3.11): timing unchanged (post 0.7-1.1 ms,
  sum ~86.5 ms, 25.0 FPS); slow walkers produce a near-continuous
  dashed line with stride 4 + 4 px dedup; the dark-red palette
  entry reads on white walls.
