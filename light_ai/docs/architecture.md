# `light_ai` — Architecture, Modules & Platform Constraints

Deep technical reference for [`light_ai`](../README.md): module/header layout, error & exit-code contract, and verified RK3588 hardware/SDK constraints.

## Modules & Headers

The codebase contains **104 headers** under [`include/`](../include/) and **62 translation units** under [`src/`](../src/) + [`externals/inih/ini.c`](../externals/inih/ini.c).

### Pipeline & Hardware Modules (`include/<folder>/`, `src/<folder>/`)

Every header lives in a domain folder (Stage E, #112); a header's `.cpp` sits in the same folder under `src/`, and only `src/main.cpp` stays at the `src/` root. Includes spell the path from `include/` (`#include "media/rga_resizer.hpp"`). Folder dependencies point one way:

```
core  <-  platform, config, media  <-  sys  <-  source, npu, net  <-  render  <-  app  <-  main
```

| Folder | Header | Responsibility |
|---|---|---|
| `app/` | `pipeline.hpp` | Top-level orchestration (`next_frame`, `step`, `publish_oldest`, A76 pinning, `mlockall`) |
| `app/` | `source_supervisor.hpp`, `preprocessor.hpp`, `analytics_stage.hpp`, `stream_output.hpp` | Stage owners extracted from `Pipeline` (capture/prober, RGA+NPU dispatch, tracker/Re-ID, overlay/net publish) |
| `app/` | `console_reporter.hpp` | Throttled detection/track reporter (`id=G(L...)` when Re-ID remaps) |
| `source/` | `sources.hpp` | Umbrella convenience include for `source/` headers |
| `source/` | `media_source.hpp` | Concrete `std::variant` owner of `CameraV4L2` and `NetMjpegSource` (static dispatch) |
| `source/` | `camera_v4l2.hpp` | USB MJPEG V4L2 capture (`S_FMT`/`G_FMT`, mmap `DQBUF`/`QBUF`, hot-replug recovery) |
| `source/` | `net_mjpeg_source.hpp` | HTTP `multipart/x-mixed-replace` MJPEG pull client |
| `media/` | `mpp_decoder.hpp`, `rkmpp_drm_pool.hpp` | Rockchip MPP MJPEG → YUV422SP/YUV420SP decoder and external DRM `dma-buf` pool |
| `media/` | `rga_resizer.hpp`, `dma_sync.hpp` | RGA3 `im2d` letterbox/rotation (`--src-rot`), per-call RGA2 `crop_resize` (RGB888 Re-ID crops), and `DMA_BUF_IOCTL_SYNC` guard |
| `npu/` | `rknn_engine.hpp` | RKNN zero-copy NPU engine (multi-slot `setup_io`, `bind_input_slot`, `sync_output`, native or INT8→FP16 outputs, move-only) |
| `npu/` | `inference_pool.hpp`, `infer_slot.hpp` | Multi-core NPU worker pool (`--pool 1` / `3`), ordered FIFO, bounded `5 s` wait & `Abandoned` slot handoff |
| `npu/` | `reid_worker.hpp`, `reid_queue.hpp` | Asynchronous Re-ID embedding runner (`RknnEngine` with 4 zero-copy slots + `BasicReidQueue` thread) |
| `npu/` | `yolo_postprocess.hpp`, `coco_names.hpp` | Worker-local `YoloDecode` wrapper and 80-class COCO label lookup table |
| `npu/` | `cal_dumper.hpp` | `--dump-cal` / `--cal` PPM writer |
| `render/` | `box_smoother.hpp` | 32-slot matcher / coaster for non-person boxes (`cls != 0`, display-filtered by `BoxFilter`) |
| `render/` | `box_mapper.hpp`, `overlay_sink.hpp` | Maps tracks, trails, EN/RU callouts, and non-person boxes; owns GPU/CPU draw parity + network output |
| `render/` | `gpu_box_drawer.hpp` | Mali G610 EGL/GBM + GLES2/3 batched box, trail & `GL_LUMINANCE_ALPHA` callout label renderer (outline + fill pass; CPU fallback in `core/overlay_raster.hpp`) |
| `net/` | `net_encoder.hpp`, `net_preview.hpp` | `h264_rkmpp` MPEG-TS encoder facade (`= default` dtor) and raw RGB24 NUT preview server |
| `net/` | `tcp_avio.hpp`, `av_common.hpp`, `muxer_teardown.hpp` | FFmpeg AVIO binding over `TcpPullServer`, FFmpeg helpers, RAII muxer teardown |
| `platform/` | `joining_thread.hpp`, `buffered_socket.hpp`, `tcp_pull_server.hpp`, `log_sink.hpp`, `pi_mutex.hpp` | POSIX/C++17 OS helpers: RAII thread, non-blocking buffered socket, single-viewer TCP pull transport (`TCP_NODELAY`, `TCP_NOTSENT_LOWAT`, `SO_SNDBUF`, `POLLOUT` send loop in `src/platform/tcp_send.cpp`), lock-free `AsyncLogSink`, `PiMutex` / `PiCondVar` |
| `sys/` | `cpu_affinity.hpp`, `process_env.hpp`, `signal_waiter.hpp`, `sd_watchdog.hpp` | Cortex-A76 affinity pinning & RT setup, signal mask + `SIGPIPE` setup, `signalfd` + `eventfd` → `StopFlag` bridge, systemd `NOTIFY_SOCKET` watchdog client |

### SDK-Free Pure-Logic Core (`include/core/`, `src/core/`)

`core/` depends only on the C++17 standard library (and `core/*`), allowing all pure logic to build and run on any host without Rockchip or FFmpeg headers:

- **Primitives & Errors:** `status.hpp` (`Status`, `Errc`, `Sys`, `ErrnoText`), `log.hpp` (`Log`, thread-safe RAII `Line`), `stop_flag.hpp` (`StopFlag`, `sleep_interruptible`), `fixed_fmt.hpp`, `line_scan.hpp`, `perf_clock.hpp` (`ms_between`, `MaxMs`, `CounterDelta`, `Lap`), `perf_reporter.hpp` (`PerfReporter`: the source-side window and the `[perf] src`, `[perf] enc`, `[perf] rt`, and `[perf] reid` lines; byte-pinned format), `net_perf.hpp` (`SourcePerfWindow`, `EncPerfWindow`, `ReidPerfWindow`: the zero-heap 5 s windows behind those lines).
- **Geometry & Detection:** `geometry.hpp` (`LbGeom`, `band_geom`), `rga_limits.hpp` (the RK3588 RGA limits as pure functions: 68 px minimum side, 8× per pass, 16-aligned RGB888 stride; `needs_two_pass` and the aux sizing shared by `RgaResizer`, `SourceChannel`, `Pipeline` and `RknnEngine`), `detection.hpp` (`Detection`, `DetList`), `frame_view.hpp` (`TensorRef` with `stride_bytes`, `TensorSet`, `FrameView`), `fp16.hpp` (`fp16_to_float`), `box_math.hpp` (`box_iou`, `box_iou_corners`, `greedy_iou_match`), `yolo_decoder.hpp` (`YoloDecoder`, `yolo_head_shape`, `Box::seq` NMS ordering).
- **Tracking, Re-ID & Overlay:** `track.hpp` (`TrackLife`, `TrajPoint`, `TrackState`, `TrajList`), `kalman_axis.hpp`, `associator.hpp`, `trail_window.hpp`, `box_filter.hpp` (`BoxFilter` One-Euro + backlash dead zone, `TrackBoxFilters`: the one display filter of every drawn box), `gpu_box.hpp` (`GpuBox`, `GpuBoxList`), `glyph_font.hpp` (proportional DejaVu Sans, 147 glyphs: digits, Latin, Cyrillic, label punctuation; a `256×296` atlas of two 8-bit planes, fill and the baked 2 px dark outline (`151,552` bytes), baked at 22 px by `tools/gen_glyph_font.py`, sampled bilinearly; `for_each_glyph` / `raster_text`: the one glyph walk of every renderer; zero-heap UTF-8), `glyph_font_table.hpp` (the atlas and glyph table behind `glyph_font.hpp`, generated by `tools/gen_glyph_font.py`: do not edit), `callout.hpp` (`compute_callouts` with per-camera `CalloutHistory` hysteresis; plate-free white labels with a dark outline; `callout_text_scale` gives every source the same letter size on its output tile), `overlay_raster.hpp` (`draw_boxes_cpu`, `draw_callouts_cpu`: the CPU twin of `GpuBoxDrawer`, the `--cpu-draw` / no-GPU fallback), `auto_orient.hpp` (`AutoOrient` EXIF + 4-angle scoring), `global_track.hpp` (`GlobalTrackTable`, crop scheduler).
- **Containers, Tracker & Timing:** `pod_list.hpp`, `pod_ring.hpp` (zero-heap fixed-capacity `PodList<T, N>`, `PodRing<T, N>`), `throw_on.hpp` (startup `Status` → exception bridge), `tracker.hpp` (16-slot Kalman + greedy IoU tracker: `kMaxTracks = 16`, `kMaxMiss = 6`, `global_id`, `gen`), `timings.hpp`, `frame_trace.hpp` (per-stage `avg/60` + `TOTAL` timing accumulator).
- **HTTP/MJPEG Parsing:** `byte_window.hpp`, `http_url.hpp`, `http_head.hpp`, `multipart_reader.hpp`, `jpeg_probe.hpp` (JPEG `SOFn` + EXIF `Orientation` probe).
- **Encoder/Stream State:** `encoder_config.hpp`, `param_set_cache.hpp`, `pts_clock.hpp` (`PtsClock`, `SessionPtsFilter`), `encode_pacer.hpp` (4-level transport backpressure hysteresis, Stage 10), `session_warmup.hpp`, `idle_plate.hpp`, `packet_queue.hpp`, `session_gate.hpp`, `preview_ring.hpp` (`PreviewRing`: the 2-slot drop-oldest hand-off from the pipeline thread to the preview serve thread, sized once at startup).

### Configuration, Source & Network Building Blocks

- **`include/config/`, `src/config/`:** `config.hpp` (`Config`, `InferConfig`, `CalConfig`, `StreamConfig`, `LabelLang`), `option_registry.hpp` (28-entry `std::array` registry), `cli.hpp`, `config_file.hpp` (`inih`-backed INI loader).
- **`include/source/`, `src/source/`:** `source_types.hpp` (`SourceKind`, `SourceEndpoint` with `rot_deg`/`rot_auto`, `SourceGeom`), `source_channel.hpp` (`NeverAttached` / `Detached` / `Live`, `quarter_turn`, `needs_aux`), `source_set.hpp` (round-robin + `::poll(2)` `wait_any()` + `2 s` stall detection), `source_prober.hpp` (`2 s` background re-attach), `usb_node_locator.hpp` (USB + `device_caps` capture filtering), `mjpeg_slot_pool.hpp`.
- **`include/net/`, `src/net/`:** `network_init.hpp`, `encoder_frame_pool.hpp` (4-slot NV12 DRM ring), `frame_composer.hpp` (RGA tile/band compositor), `h264_encoder.hpp` (`h264_rkmpp` wrapper), `av_packet.hpp`, `av_packet_queue.hpp`, `mux_session.hpp`.

## Error Model & Exit Codes

- **Recoverable errors (`Status`):** Modules return [`Status`](../include/core/status.hpp) (`Errc`: `Ok`, `Absent`, `Lost`, `Busy`, `Timeout`, `Unsupported`, `NoMemory`, `Invalid`, `Failed`, plus string-literal reason and optional `errno`/SDK code) and write outputs to reference parameters. Multi-branch outcomes (`FrameStatus`, `SourceChannel::Attach`, `HttpHead::Parse`) embed `Status` in their failure variant.
- **Startup-only exceptions:** [`throw_on`](../include/core/throw_on.hpp) converts non-OK `Status` into `std::runtime_error` only during initialization (constructors and `Pipeline` setup).
- **`noexcept` frame path & thread bodies:** No exceptions are thrown or caught on the frame path (`bugprone-exception-escape` clean).
- **Thread-safe logging (`Log`):** `const Log&` is injected from `main.cpp` (info → `stdout`, warn/error → `stderr`); in production each line is formatted into a fixed stack buffer and passed lock-free to `AsyncLogSink` (zero heap, zero mutex on the frame path), with `std::recursive_mutex` used only in tests and early CLI parsing.

| Exit Code | Meaning | Source |
|---|---|---|
| `0` | Clean exit (`--help`/`-h`, completed `--frames N`, or `SIGINT`/`SIGTERM` graceful drain) | [`src/main.cpp`](../src/main.cpp) |
| `1` | Runtime fatal error (`Pipeline::run()` returned a non-OK `Status`) | [`src/main.cpp`](../src/main.cpp) |
| `2` | Invalid CLI flag, config file error, or bad test-runner option | [`src/main.cpp`](../src/main.cpp), [`tests/suite_runner.hpp`](../tests/suite_runner.hpp) |
| `3` | Startup failure (`std::exception` caught in `main()`: missing model, bad YOLOv5 head shape, hardware init failure) | [`src/main.cpp`](../src/main.cpp) |

## Verified Hardware & SDK Constraints

- **VFR encoder pitfall:** Passing `framerate = 0/1` to `h264_rkmpp` triggers MPP's `65535` auto marker and collapses CBR bit allocation (`~83 kbps`). Follow-source (`--net-fps 0`) keeps nominal `25` fps on the encoder rate controller while stamping per-dequeue `pts` (`1/90000` ticks).
- **MPP alignment & logging:** Decoder output buffer heights must be aligned to `16` (`1080 -> 1088`), and `mpp_buffer_group_get` must receive a non-null caller string (`__FUNCTION__`).
- **RGA3 vs. RGA2 & >4 GB DMA-heap:** On `>= 8 GB` boards, DMA allocations use `/dev/dma_heap/system-dma32` because RGA2 cannot map physical addresses above 4 GB (`"RGA_MMU unsupported memory larger than 4G!"`). RGA3 scales at most `1/8` per axis (checked by multiplication; `1920×1080 -> 224×224` and `90°`/`270°` rotated portrait sources downscaled along a diagonal axis use 2-pass via `aux_fd`). Decoder NV12/letterbox/rotation jobs alternate across both RGA3 cores (`IM_SCHEDULER_RGA3_CORE0` / `IM_SCHEDULER_RGA3_CORE1` via `RgaResizer::next_rga3_core()`). By contrast, `RgaResizer::crop_resize()` (RGB888 NPU tensor sub-rect → `128×256` Re-ID tensor) uses `IM_SCHEDULER_RGA3_CORE0` when within RGA3 size limits (`can_use_rga3`) and falls back to `IM_SCHEDULER_RGA2_CORE0` for small crops.
- **RGA fill prohibited:** RGA hardware rectangle fill is unsupported on RGA3/RGA2E RGB888 and an `imfill` probe crashed the kernel (`Oops: 96000005`). Box/trail drawing uses Mali G610 (`GpuBoxDrawer`) or CPU fallback (`--cpu-draw` / invalid tensor pitch `0x3003` fallback).
- **USB2 camera firmware pacing:** Tested Canyon CNS-CWC5 (`1bcf:2283`, USB2 `480M`) delivers discrete `{20, 25, 30}` FPS firmware states unaffected by UVC `exposure_absolute` or `get-parm`; treat `20 FPS` as the reliable camera baseline.
- **NPU multi-core behavior:** A single RKNN context does not parallelize layers across NPU cores (`--core 7` vs `1` buys `<= 2.5 ms`). Multi-core throughput comes from parallel contexts via `--pool 3` (one engine per core).
- **YOLOv5s RKNN semantics:** `yolov5s_relu` outputs (`255` channels across `80×80`, `40×40`, `20×20` heads) are already post-sigmoid (`0..1`). INT8 models (`v2`–`v5`) rewrite output `attr` to `FLOAT16` via `rknn_set_io_mem` for runtime per-channel dequantization.
- **Zero-copy H.264 input:** `h264_rkmpp` must receive `DRM_PRIME` descriptors with `16`-aligned vertical stride in both RGA destination and descriptor; system-memory frames force `av_hwframe_transfer_data` copies and cause `enc` latency drift (`1.8 -> 15 ms`).
