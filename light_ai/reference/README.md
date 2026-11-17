# Reference detections (issue #32)

The safety net of stage 6 (YoloDecoder, BoxMapper, KalmanAxis, Associator): the
detection output of today's code on a fixed clip, in two layers.

- **Layer A (board): the NPU path.** `ref_evening.csv` is the `--box-log` of a
  replay of the clip (decode -> RGA -> NPU -> the YOLO postprocess (`YoloDecoder` since stage 6a) -> `BoxSmoother`:
  per frame a `frame,n_raw,n_sm` line, then the `raw,...` and `sm,...` boxes).
  It covers YoloDecoder (and BoxSmoother). No tracker: the tracker runs on the
  dequeue time, which differs from run to run -- that is layer B.
- **Layer B (host): the time-dependent part.** `tests/test_reference_chain.cpp`
  feeds a slice of the `raw` lines of `ref_evening.csv` with fixed timestamps
  into BoxSmoother + Tracker + `BoxMapper` (before stage 6c: `fill_gpu_boxes`) and compares with a golden file
  (tests/data/README). It runs in `scripts/test.sh`, so on every gate.

## What "identical" means in layer A
`--box-log` writes floats through `operator<<` with the default stream precision:
**6 significant digits** (`include/core/detection.hpp`), about 0.01 px at x \~ 1000.
`cmp` proves the CSV equal to 6 significant digits, not the floats bit-equal.
A stage 6 rewrite may, in rare cases, flip a last printed digit when a value sits
at a rounding boundary: a rounding artifact to explain (show the two values),
not a behavior change. Anything else that differs is a behavior change.

## The clip
| | |
|---|---|
| file | `misc/ref_evening.mjpeg` (git-ignored, `misc/*.mjpeg`) |
| size | 131715466 bytes |
| frames | 1063, 1280x960 baseline MJPEG as captured (stream copy), \~21.3 FPS: 50 s |
| sha256 | `9d0d22a8c94f83b86337cefe94d9bd25afd9e0f27a92b658bdf8cd7ef29aa7b0` |
| recorded | 2026-11-15, the USB camera (Canyon CNS CWC5), room light in the evening |
| content | one person: walks across, enters and leaves, stands still, half out of frame, close up |

Between the scenes the room is empty. No two people crossing: one person was available.

Recorded with:

    ffmpeg -f v4l2 -input_format mjpeg -video_size 1280x960 -framerate 30 -i /dev/video21 \
        -t 50 -c copy -f mjpeg misc/ref_evening.mjpeg

The frame count agrees two ways: `tools/mjpeg_replay.py misc/ref_evening.mjpeg --count`
(the server's own split: segments up to SOS, then EOI; every frame checked for
SOF 1280x960) and `ffprobe -v error -count_packets -show_entries stream=nb_read_packets misc/ref_evening.mjpeg`.

## The reference run
| | |
|---|---|
| CSV | `reference/ref_evening.csv`, 294319 bytes, 1063 frame blocks |
| CSV sha256 | `83a6b6192328a415fab45f008117e53c58ed337767dceb44020a331082d2a20a` |
| commit | `feat/callout-2x-box-stabilize` (`BoxSmoother` interval-envelope stabilizer + confirmed coasting & hysteresis; `raw` lines unchanged vs Stage 4) |
| model | `misc/yolov5s_relu_int8_mmse_v5.rknn` (the working model, the default `--model`) |
| model sha256 | `8fe20029429248ce07556c28820a46b7d342445bd6b2eb9a47e6fd089861b085` |
| thresholds | `--conf 0.25 --iou 0.45` (the defaults) |
| librknnrt | `2.5.0 (74ea630ce8@2026-09-30T09:41:17)` |
| pool | `--pool 3` and `--pool 1`: `cmp`-identical |
| viewer | none (no `--net`): recognition only, the output side asleep |

## Rerun layer A

    bash tools/ref_replay.sh misc/ref_evening.mjpeg build/ref_p3.csv 3 reference/ref_evening.csv
    bash tools/ref_replay.sh misc/ref_evening.mjpeg build/ref_p1.csv 1 reference/ref_evening.csv

Each run takes \~107 s (the clip paced at 10 FPS) and prints `PASS: 1063 frames in,
1063 frame blocks out, ...` or `FAIL: <why>`. A different model, threshold or
librknnrt version gives a different CSV by design: compare against the table above
first. Build the binary first (`./scripts/build.sh`); no heavy job in parallel.

## How the replay is drop-free
The net source (`--net-src http://127.0.0.1:8090/`, `tools/mjpeg_replay.py`)
loses frames in three places; the replay avoids each, and every run proves it:
- the `start()` drain runs inside `MediaSource::open` right after the prime of
  part 0 in `setup_device()`: nothing is buffered yet;
- the latest-frame skip (`read_latest_part`) fires only when a second complete
  part is buffered at a dequeue. The server holds part 1 for 0.5 s after part 0,
  then paces at 10 FPS, well below the pipeline (\~30 ms per frame at `--pool 1`).
  The startup between the prime and the second dequeue, measured with `strace -tt`
  on the socket (2026-11-15): \~5 ms at `--pool 3`, \~31 ms at `--pool 1` (frame 0 is
  processed synchronously there). 0.5 s keeps a wide margin and stays far below
  the 2 s watchdog (`kPollMs`, `kStallMs`);
- a corrupt frame: the server checks every frame before serving.
Decode warm-up does not apply: the source is up at startup (no hot attach).
`tools/ref_replay.sh` fails a run unless light_ai exits 0 after `--frames 1063`
(a lost frame leaves it waiting: the watchdog, a refused reconnect -- the server
takes one connection -- then the timer), the log has no source-trouble line
(`net-src: corrupt`, `detached`, `dequeue failed`, `no frame in`), the server sent
every frame, and the CSV holds 1063 frame blocks numbered 0..1062 in order.

Stage 6 Re-ID check on the same clip (`--reid-model ./misc/osnet_x1_0_imagenet.rknn --pool 3`):
leaves detector/tracker local geometry unchanged while mapping all 6 re-entries (`L1..L6`) of the subject to `global_id = 1` (`29.615 FPS`, `1063` frames, `0` errors).
