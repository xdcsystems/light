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
- RgaResizer : im2d resize by fd (wrapbuffer_fd + imresize).
- RknnEngine : rknn_create_mem + rknn_set_io_mem; the input tensor fd is
               handed to RGA as the destination (resize writes directly into
               the NPU tensor). Output buffer sized exactly (out_attr_.size).
- Pipeline   : model input geometry is read from the model file itself
               (input_attr), not from CLI flags.

## Measured performance (640x480 MJPEG, 30 FPS source)

| Stage  | Average, ms |
|--------|-------------|
| cam    | 30.2        |
| decode | 0.7         |
| resize | 0.4         |
| infer  | 2.0         |
| total  | 33.3        |

Steady-state throughput: 30.0 FPS — the pipeline is camera-bound. The sensor
offers 30 FPS as its maximum in every mode and every format
(v4l2-ctl --list-formats-ext), so the pipeline already runs at the rate of
the source. Stability: 5000-frame continuous run, zero frame loss
(ok = 5000/5000), average 29.9 FPS; RSS flat at ~19.9 MB (no leaks).

## Build
    ./scripts/build.sh

## Run
    sudo LD_LIBRARY_PATH=/usr/local/lib ./build/light_ai --device /dev/video20 --frames 300

Options: --device, --model, --frames, --w, --h. Numeric values are strictly
validated (garbage is an error, not a silent zero).

## Roadmap
- [x] M1: baseline run with mobilenet_v1, honest FPS measurement.
- [x] M1.5: per-stage timing (cam / decode / resize / infer) — bottleneck
      identified as the camera source.
- [x] M1.6: hardening — model dims from the model file, G_FMT verification,
      strict CLI parsing, error-path cleanup in the decoder, exact output
      buffer sizing. FPS unchanged (camera-bound, as expected).
- [ ] M2: convert yolov8n.onnx -> .rknn (rknn-toolkit2 on x86), 640x640 input.
- [ ] M3: NV12-family model input — RGA without format conversion, CPU-side
      quantization disappears.
- [ ] M4: pipelining (async RGA), NPU/DDR governor = performance. Only pays
      off once the source delivers more than 30 FPS (infer headroom analysis:
      even a 10-15 ms yolov8n@640x640 fits the 33.3 ms frame budget without
      pipelining).
