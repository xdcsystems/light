# Полный технический аудит проекта `light_ai` (RK3588)

**Дата аудита:** 2026-11-18  
**Целевой репозиторий:** `/mnt/nvme_data/linaro/Projects/light/light_ai` (плата RK3588 `linaro@192.168.1.10`)  
**Режим аудита:** строго read-only (без изменения и коммита существующих файлов репозитория)  
**Проверенная ветка и коммит:** `feat/callout-2x-box-stabilize` @ `bf63b2e` (4 коммита впереди `master` @ `11b6cfc`, дельта `master..HEAD`: 48 файлов, `+7409 / -4202` строк)

---

## 0. Базовая проверка состояния, сборки, замеров и карты подсистем

### 0.1 Состояние Git и результаты `./scripts/build.sh` (R1)
- **Актуальная ветка:** на плате RK3588 активна ветка **`feat/callout-2x-box-stabilize`** (`bf63b2eaa41e2ecc978cc124cad5a3902d98383d`), а не `master` (`11b6cfc`, Merge PR #86). По условиям read-only аудита переключение ветки (`git checkout`) не выполнялось; аудит проведён по текущему состоянию рабочего дерева на плате с отдельным учётом регрессий и изменений из `master..HEAD` (`a049d81`, `5d7bc24`, `db8ca8a`, `bf63b2e`).
- **Чистота рабочего дерева (`git status --short --branch`):** чисто (`0` изменённых, `0` индексированных, `0` неотслеживаемых файлов) как до, так и после всех сборок, тестов, прогонов `ref_evening` и проверок.
- **Инвентаризация репозитория (`git ls-files`):** `297` отслеживаемых файлов (`111 119` строк `wc -l`), из них `include/`: `89` файлов (`7 568` строк), `src/`: `55` файлов (`12 483` строк), `tests/`: `69` файлов (`16 371` строк).
- **Реальный запуск `./scripts/build.sh` (`rc = 0`):**
  - **Release-сборка (`build/light_ai`):** `[100%] Built target light_ai` (MPP `1.3.10`, RGA `1.10.6`, FFmpeg `libavformat 62.12.103 / libavcodec 62.28.103 / libavutil 60.26.103`, EGL `1.5`, GLES `3.2`, RKNN `2.5.0`).
  - **Unit-тесты (`./build/tests/light_ai_tests`):** **54 набора, 7 899 проверок, 0 ошибок** (`reference_chain`: 228 кадров клипа, 6 228 строк, `max |d| = 0 px, 0 px/s, 0 conf`).
  - **TSan (`./build/tests/light_ai_tests_tsan`):** **5 наборов, 22 проверки, 0 ошибок** (`1 s`).
  - **2-pass `.clang-tidy` (`scripts/tidy_run.sh 600`):**
    - В рамках `./scripts/build.sh`: `0` замечаний (216 cache hits, `16 s`).
    - Дополнительно выполнен **полный безкэшный прогон** (`TIDY_NO_CACHE=1 bash scripts/tidy_run.sh 600` по `108` TU `src/` + `tests/`, `216` запусков, `0` cache hits): **`PASS (0 findings: main + cognitive)`** за `248 s` (`main` LLVM `11.0.1`: `229 s`, `0` замечаний; `cognitive` LLVM `23.1.4`, порог `<= 20`: `19 s`, `0` замечаний; зависаний/ошибок: `0`).

### 0.2 Сверка материалов и замеры `ref_evening` / живого прогона на RK3588 (R4)
Окружение платы: все governors (`policy0`, `policy4`, `policy6`, `dmc`, `fb000000.gpu`, `fdab0000.npu`) в режиме `performance`; ОЗУ `MemTotal = 4 000 444 kB` (`3.8 GiB`), по Device Tree (`/sys/firmware/devicetree/base/memory/reg`) 256 МБ ОЗУ расположено выше 4 ГБ (`0x1_f000_0000..0x2_0000_0000`), `rga2` не имеет IOMMU, `rga3` и `rknpu` работают через IOMMU; подключена USB-камера `Canyon CNS CWC5` (`/dev/video20`).

| Прогон (временные файлы изолированы в `/tmp`) | Кадры / Время / FPS | Задержки этапов (`[timing] TOTAL`, мс) | Результат сверки и ресурсы |
|---|---|---|---|
| **A. Эталон Layer A (`tools/ref_replay.sh`, `--pool 3`, 10 FPS)** | `1063` / `106.627 s` / **`9.969 FPS`** | `cam=97.6 dec=1.5 rga=1.1 infer=24.8 post=0.1 enc=0.0 \| sum=125.1` | **PASS**, побайтово идентичен `reference/ref_evening.csv` (`sha256: 8ed90827...`), `L1..L6 -> G=1`, `0` ошибок |
| **B. Полный пайплайн (`ref_evening` @ 25 FPS + GPU overlay + H.264 + Re-ID + TCP viewer)** | `1063` / `42.969 s` / **`24.739 FPS`** | `cam=37.6 dec=1.6 rga=1.1 infer=24.9 post=0.1 enc=1.1 \| sum=66.5` | `CSV_CMP=IDENTICAL` к `reference/ref_evening.csv`, MPEG-TS `16 837 844 B`, CPU `28.1%`, `VmRSS=146 456 kB`, `Threads=14` |
| **C. Предел без троттлинга (`ref_evening` @ 120 FPS + активный TCP viewer, 15 с)** | `891` / `14.883 s` / пик **`~95.8 FPS`** | `cam=2.6 dec=2.1 rga=1.1 infer=25.8 post=0.1 enc=1.1 \| sum=31.6–33.1` | Нагрузка NPU: **`Core0 50–56%, Core1 52–54%, Core2 52–55%`**, CPU `91–108%` (~1 ядро) |
| **D. Живой прогон `/dev/video20` (12 с + GPU overlay + H.264 + TCP viewer)** | `275` / `11.577 s` / **`23.754 FPS`** (уст. `~25.2 FPS`) | `cam=39.2 dec=1.5 rga=1.1 infer=24.8 post=0.1 enc=1.1 \| sum=67.7` | MPEG-TS `4 329 452 B`, CPU `25.1%`, `VmRSS=155 240 kB`, `auto-rot 0 deg`, `0` ошибок |

**Расхождения в документации, выявленные при сверке:**
- `docs/state.md:1469` указывает устаревший размер и хэш `reference/ref_evening.csv` (`279 560 B`, `f86d3d78...`), тогда как актуальный файл (обновлён в `feat/callout-2x-box-stabilize`) имеет размер `294 222 B` (`8ed90827...`, совпадает с `reference/README.md:48-49`).
- `README.md:6,111,151` приводит устаревшие счётчики (`88 headers / 55 TUs`, `7 503 checks`) вместо фактических (`89 headers / 55 src TUs / 108 tidy TUs`, `7 899 checks`).
- `docs/state.md:1481` фиксирует в живом 2-камерном тесте перепутывание `G=1`/`G=3` после 7–23-кадровых окклюзий — причина подтверждена напрямую в коде (см. **[CH-9]**).

### 0.3 Карта проекта по 5 подсистемам (R2)

| # | Подсистема | Объём (с учётом общих заголовков) | Потоки и синхронизация | Владение памятью / `fd` / `dma-buf` | Ключевые контракты |
|---|---|---|---|---|---|
| **1** | **Вход и декодирование** (`sources*`, `net_mjpeg_source*`, `camera_v4l2*`, `mpp_decoder*`, `jpeg_probe*`, `source/*`, `http_*`, `multipart_reader*`) | 34 файла, 4 698 строк | Pipeline-поток + `BasicSourceProber` (2 с цикл) + внутр. потоки MPP; граница владения — `SourceChannel::state_` (`acquire/release`) | V4L2 MMAP (4 буфера), `MjpegSlotPool` (4x1 MiB anon `mmap`), `MppDecoder` (`pkt_buf_` с `DmaSync`+`cache_clean`+64B pad, единственный `out_frame_` DRM `dma-buf`, ленивый `aux_`) | `SourceChannel::decode()` отдаёт заимствованный `fd` до следующего `decode()`; `NetMjpegSource` работает через `ByteWindow<256 KiB>` |
| **2** | **RGA, NPU и Re-ID** (`rga_resizer*`, `rknn_engine*`, `inference_pool*`, `infer_slot*`, `reid_worker*`, `reid_queue*`, `cal_dumper*`, `yolo_*`, `box_mapper*`) | 22 файла, 2 637 строк | Pipeline-поток + `1/3` воркеров `InferencePool` (A76) + 1 поток `BasicReidQueue` (не привязан к A76); `InferSlot` (`mx_+cv_`), `PodRing` (single-producer) | `RknnEngine` владеет `in_mems_`/`out_mems_` (`rknn_create_mem`); `RgaResizer` пишет YUV->RGB888 (RGA3) и кропы Re-ID (RGA2); `CalDumper` читает через `DmaSync`+`cache_drop` | `RgaResizer::wrap()` предполагает packed `wstride=w, hstride=h`; `BoxMapper` пакует детекции, боксы треков и точки шлейфа в `GpuBoxList` (cap 128) |
| **3** | **Математика, трекинг и стабилизация** (`core/box_math*`, `box_envelope*`, `tracker*`, `box_smoother*`, `global_track*`, `trail_window*`, `callout*`, `geometry*`, `auto_orient*`, `associator*`, `kalman_axis*`, `idle_plate*`) | 24 файла, 3 055 строк | Только pipeline-поток (нулевая конкуренция потоков внутри S3; `ReidResult` приходит через очередь) | 100% zero-heap фиксированные буферы (`DetList=128`, `TrajList=8`, `BoxSmoother=32`, `TrailWindow=300`, `GlobalTrackTable=32`); `core/` без SDK/GL/OS-зависимостей | Сценовые пиксели `[0..eff_w)×[0..eff_h)` -> `LbGeom` тензора; `OverlaySink` повторно фильтрует `TrajList` через `BoxEnvelope(0.0)` |
| **4** | **Оверлей, GPU и H.264/сеть** (`overlay_sink*`, `gpu_box_drawer*`, `glyph_font*`, `bitmap_font*`, `encoder_config*`, `encode_pacer*`, `param_set_cache*`, `net/*`, `net_encoder*`, `net_preview*`, `tcp_pull_server*`) | 37 файлов, 7 078 строк | Pipeline-поток (`OverlaySink`, `GpuBoxDrawer`, `FrameComposer`, `H264Encoder`, `NetPreview::publish`) + `MuxSession::serve` + `NetPreview::serve` | Тензоры NPU -> `EGLImageKHR` (BGR R/B swap, fence `100ms`/`glFinish`) -> `FrameComposer` (`canvas_`/`idle_` `RkmppDrmPool<1>`) -> `EncoderFramePool<8>` (`AV_PIX_FMT_DRM_PRIME`) -> `PacketQueue<32>` -> `ParamSetCache` -> MPEG-TS/NUT | `SessionGate` изолирует старт/стоп сессий; `SessionPtsFilter` сдвигает epoch PTS; выход «спит» при отсутствии зрителя (`enc=0.0 ms`) |
| **5** | **Оркестрация и конфигурация** (`pipeline*`, `main*`, `config/*`, `pts_clock*`, `timings*`, `signal_waiter*`, `process_env*`, `cpu_affinity*`, `pod_*`, `CMakeLists.txt`) | 34 файла, 3 089 строк | Главный поток (A76 `cpu4-7`) + поток `SignalWaiter` (`sigtimedwait` 100 ms); деструкторы строго: `reid_worker_` -> `sink_` -> `pool_` -> `rga_` -> `srcs_` | RAII владение `Pipeline`; `OptionRegistry` (26 опций, `defaults < INI < CLI`); `PtsClock` гарантирует монотонность 90 kHz | `step()` координирует `SourceSet`, `AutoOrient`, `InferencePool`, `Tracker`/`BoxSmoother`, `ReidWorker`, `OverlaySink` |

---

## 1. Critical / High (баги, гонки, UB, утечки DMA/fd, ошибки железа RK3588)

### [CH-1] Multi-source `NetMjpegSource` отвергает валидные JPEG > ~255 КБ (регрессия `master..HEAD`)
- **Файл и строки:** `include/source/buffered_socket.hpp:27`, `include/source/mjpeg_slot_pool.hpp:30`, `src/net_mjpeg_source.cpp:114-160`, `src/source/buffered_socket.cpp:159-160`.
- **Суть и сценарий воспроизведения:** Размер окна сокета `BufferedSocket::kWindow` равен `256 KiB`, а размер слота `MjpegSlotPool::kSlotBytes` — `1 MiB`. Коммит `db8ca8a` в `feat/callout-2x-box-stabilize` удалил `sock_.full() ||` из `NetMjpegSource::part_buffered()`. В однокамерном блокирующем режиме (`timeout_ms < 0`) `wait_part_ready` пропускается, и `read_part()` потоково дочитывает >256 КБ кадр в 1 МБ слот. Но в **многокамерном режиме** (`timeout_ms >= 0`, `SourceSet::next`) `wait_part_ready()` требует `parts_.part_buffered()` == `true` до запуска `read_part()`. Любой валидный JPEG > ~255 КБ заполняет `win_`, вызывает `Fill::Full` -> `Ready::Desync` -> `drop_window()` -> `EAGAIN` x3 -> `EIO` на 4-й попытке -> бесконечный цикл переподключения камеры.
- **Способ исправления:** В неблокирующем `dequeue(timeout_ms >= 0)` передавать фрагменты в текущий слот `MjpegSlotPool` инкрементально по мере поступления заголовка/тела (как это делает `read_part`), а не требовать размещения всего JPEG в `BufferedSocket::kWindow`, либо увеличить окно с явным поточным парсером тела и добавить регрессионный тест в `tests/test_net_mjpeg_source.cpp` для JPEG ~300–900 КБ в режиме `timeout_ms >= 0`.

### [CH-2] `MppDecoder` и `RgaResizer` теряют `hor_stride`/`ver_stride` MPP (искажение 1080p и некратных 16 ширин)
- **Файл и строки:** `src/mpp_decoder.cpp:95-96, 446-451`, `include/mpp_decoder.hpp:54`, `src/rga_resizer.cpp:15-18, 84-85, 146-147`, `src/pipeline.cpp:418-419, 518-527`.
- **Суть и сценарий воспроизведения:**
  1. `MppDecoder::init()` выравнивает только высоту (`h_align = (h_ + 15) & ~15`), оставляя `w_` без выравнивания (`w_ * h_align * 2`).
  2. `MppDecoder` не запрашивает `mpp_frame_get_hor_stride(f)` / `mpp_frame_get_ver_stride(f)` и не передаёт их наружу. Прямой MPP-зонд на плате подтвердил: для `1920x1080` MPP отдаёт `hor_stride=1920, ver_stride=1088` (UV смещается на 8 строк!), для `854x480` — `hor_stride=864, ver_stride=480`.
  3. `RgaResizer::wrap()` вызывает 4-аргументный `wrapbuffer_fd(fd, w, h, fmt)` (`/usr/local/include/im2d_buffer.h:155`), жёстко задавая `wstride = w, hstride = h`. На эталоне `ref_evening` (`1280x960`) проблема скрыта, т.к. оба числа кратны 16. При подаче `1920x1080` (поддерживается подключённой `Canyon CNS CWC5`) RGA читает UV-плоскость на 8 строк выше (`1920*1080` вместо `1920*1088`), вызывая сдвиг и полосы цветности; на `854x480` строки перекашиваются по диагонали.
- **Способ исправления:** В `MppDecoder::init()` выровнять оба измерения (`align16(w_)` * `align16(h_) * 2`), в `apply_info_change()` и `decode()` считывать `hor_stride`/`ver_stride` из `MppFrame` и передавать через `SourceGeom` в `RgaResizer::resize()` / `downscale_to_aux()`, где использовать 6-аргументный `wrapbuffer_fd(fd, w, h, fmt, w_stride, h_stride)`.

### [CH-3] `MppDecoder::apply_info_change()` портит состояние при ошибке памяти и `reset_and_strip()` недочищает порты
- **Файл и строки:** `src/mpp_decoder.cpp:444-461, 507-525`, `include/source/source_channel.hpp:188-189, 269`.
- **Суть и сценарий воспроизведения:**
  1. `apply_info_change()` сначала меняет `width/height/fmt` у `out_frame_`, затем вызывает `mpp_buffer_get(out_group_, &b, bs)`. При `rg != MPP_OK` код на `:457` всё равно отправляет `MPP_DEC_SET_INFO_CHANGE_READY` и возвращает `Errc::NoMemory`. `SourceChannel` делает `detach()`, но `dec_ready_` остаётся `true`, и при следующем `attach()` декодер работает со старым меньшим буфером при новых размерах `out_frame_`.
  2. `reset_and_strip()` ограничен циклом `i < 4` и прерывается при первой задаче без meta (`if (!old_p && !old_f) break;` на `:521-522`), а после `mpi_->reset(ctx_)` (`:525`) повторная зачистка не выполняется (хотя комментарий `:163-164` требует её до и после `reset()`). При >4 задачах или «чистой» задаче перед «грязной» `drop_packet()` освободит `pkt_` при висящем `KEY_INPUT_PACKET` внутри MPP.
- **Способ исправления:** В `apply_info_change()` сначала выделить новый `MppBuffer`, только при успехе обновлять `out_frame_` и слать `MPP_DEC_SET_INFO_CHANGE_READY` (при ошибке сбрасывать `dec_ready_ = false`). В `reset_and_strip()` снять ограничение на ранний `break` до истощения `dequeue`, а также выполнить зачистку портов и до, и после `mpi_->reset(ctx_)`.

### [CH-4] `RgaResizer::crop_resize()` принудительно использует RGA2 (`< 4G` без IOMMU) на IOMMU `rknn_create_mem` dma-buf
- **Файл и строки:** `src/rga_resizer.cpp:112-135`, `include/rga_resizer.h:35-38`, `src/pipeline.cpp:629`.
- **Суть и сценарий воспроизведения:** `crop_resize()` жёстко задаёт `opt.core = IM_SCHEDULER_RGA2_CORE0` для входного тензора YOLO (`view.tensor.fd`) и входного тензора Re-ID (`dst.fd`), выделенных через `rknn_create_mem`. На RK3588 `rga2` не имеет IOMMU и не адресует физическую память `> 4 GiB`, тогда как по Device Tree даже на 4 ГБ плате 256 МБ ОЗУ лежит выше 4 ГБ (`0x1_f000_0000+`), а на 8/16 ГБ платах — большая часть ОЗУ. В результате под давлением памяти или на 8/16 ГБ RK3588 `RGA2` возвращает `RGA_MMU unsupported memory larger than 4G!`, Re-ID молча отключается, а `stderr` забивается ошибками. Аппаратный зонд на плате доказал, что `RGA3` (с IOMMU) успешно принимает чётные смещения кропов `RGB888` `(2,2)` при размерах `>= 68x68` (`st=1`).
- **Способ исправления:** Маршрутизировать `crop_resize()` на `IM_SCHEDULER_RGA3_CORE0` при `cw >= 68 && ch >= 68 && w_dst >= 68 && h_dst >= 68`; для мелких кропов (`< 68 px`) выделять I/O dma-buf Re-ID и YOLO из `< 4G` DMA32/DRM-пула (`rknn_create_mem_from_fd`) либо ограничивать минимальный кроп до `68 px` при использовании RGA3-only.

### [CH-5] Нечётная высота `l.h = band.h` в single-view `make_stream_layout()` отклоняется `h264_rkmpp`
- **Файл и строки:** `src/core/encoder_config.cpp:25-27`, `tests/test_encoder_config.cpp:31-32` (подтверждено в `/home/linaro/work/ffmpeg-rockchip/libavcodec/rkmppenc.c:800-804`).
- **Суть и сценарий воспроизведения:** При `nsrc == 1` `make_stream_layout()` присваивает `l.tile_h = band.h` и `l.h = l.tile_h` без выравнивания на 2 (в то время как `nsrc > 1` вызывает `align16(...)`). В `rkmppenc.c:800-804` проверка `drm_frame->height % 2` отклоняет любой нечётный кадр с `"Unsupported size ... not 2-aligned"` (`AVERROR(ENOMEM)`). При любой модели/аспекте с нечётным `band.h` (например, `model_h = 418 -> band.h = 313` или портрет `360x639`, который даже закреплён в `test_encoder_config.cpp:31`) каждый вызов `H264Encoder::encode()` падает, срывая сессию зрителя в бесконечном цикле.
- **Способ исправления:** Округлять `l.tile_h` вверх до чётного `(band.h + 1) & ~1` (минимум `2`) в `src/core/encoder_config.cpp:25` и обновить `tests/test_encoder_config.cpp:31-36`.

### [CH-6] Повреждение DMA-кэша соседних тайлов в `FrameComposer::fill_nv12_gray_rect()`
- **Файл и строки:** `src/net/frame_composer.cpp:52-67, 135`, `include/dma_sync.hpp:69-70`.
- **Суть и сценарий воспроизведения:** `fill_nv12_gray_rect()` вызывает `DmaSync wr(fd, true)`, затем `std::memset` по строкам прямоугольника `rc` и `dma::cache_clean(p, size)` **без предварительного `dma::cache_drop()`**. При этом `lay_.tile_w` выровнен только на 16 байт, а не на 64-байтную кэш-линию AArch64. В мультикамере с `model_w % 64 != 0` (например, `416` или `608`): `reset_canvas()` оставляет `128` в CPU-кэше на границе `[608..640)`; далее RGA пишет живое изображение ячейки 1 (`[608..1216)`) в DRAM; затем при пропадании ячейки 0 `fill_nv12_gray_rect()` делает `dc cvac` на линию `[576..640)`, перезаписывая первые 32 пикселя ячейки 1 в DRAM устаревшими значениями `128`.
- **Способ исправления:** Выполнять `dma::cache_drop(p, mpp_buffer_get_size(mb))` сразу после `DmaSync wr(..., true)` и перед `std::memset`, либо выровнять `tile_w` до 64 байт (или отказаться от CPU-заливки в пользу копирования из предзалитого DMA-буфера).

### [CH-7] `NetPreview::publish()` блокирует главный поток пайплайна до 5 с под `mx_`
- **Файл и строки:** `src/net_preview.cpp:88, 218-260`, `src/overlay_sink.cpp:354-366`.
- **Суть и сценарий воспроизведения:** В режиме сырого превью (`--net-h264 0`) `NetPreview::publish()` выполняется синхронно на потоке пайплайна, захватывает `mx_` и вызывает `av_write_frame()` + `avio_flush()` на сокет `TcpPullServer` с `SO_SNDTIMEO = 5 s`. Если зритель (`ffplay`) зависнет (`SIGSTOP`) или потеряет сеть без FIN/RST, после заполнения 2 МБ `SO_SNDBUF` (~2 кадра `640x640x3`) весь пайплайн (захват, декод, NPU, трекинг) останавливается на 5 секунд на кадр, нарушая инвариант `CONTRIBUTING.md` («Recognition never sleeps»).
- **Способ исправления:** Вынести `av_write_frame()`/`avio_flush()` на поток `NetPreview::serve()` через предвыделенный 2-слотовый кольцевой буфер с политикой drop-oldest (без heap-аллокаций на кадр).

### [CH-8] `InferencePool::discard()` может блокировать поток пайплайна до 15 с при отвале камеры
- **Файл и строки:** `include/inference_pool.hpp:54, 175-190`, `include/infer_slot.hpp:139-150`, `src/pipeline.cpp:347`.
- **Суть и сценарий воспроизведения:** При `Errc::Lost` у одной камеры `Pipeline::handle_source_failure()` вызывает `pool_.discard(f.src)`, который в цикле по до 3 слотам в `fifo_` последовательно вызывает `slot.reclaim(opts_.result_timeout)` с таймаутом `5000 ms`. Если воркер NPU застрял или заторможен, когда 3 кадра камеры 0 в полёте, один отвал камеры 0 замораживает поток пайплайна (и все живые камеры, а также реакцию на `Ctrl-C`) до `3 * 5 s = 15 секунд`.
- **Способ исправления:** В `discard()` использовать нулевой таймаут (`0ms`): готовые (`Done`) слоты освобождать сразу, а занятые (`Queued`) немедленно помечать `Abandoned` (воркер сам переведёт `Abandoned -> Free` по завершении `rknn_run`).

### [CH-9] `GlobalTrackTable` допускает привязку и переключение `global_id` по окклюзированным и обрезанным кропам
- **Файл и строки:** `src/core/global_track.cpp:277-320`.
- **Суть и сценарий воспроизведения:** В `apply_bound_result()` (`:280-294`) и `apply_unbound_result()` (`:311-319`) флаг `!res.clipped && !res.occluded` защищает только `update_ema()`, но **не блокирует** `bind_slot()` и инкремент `pending_switch_hits`. Кроме того, `apply_bound_result()` не проверяет порог отрыва `m.best_sim - m.second_sim >= kReidMargin`. Когда два человека (`G=1`, `G=3`) перекрываются > 6 кадров (`kMaxMiss=6`), один трек умирает (`active=false`, отключая `collides_on_cam`), а второй/новый трек получает 2 кропа с двумя людьми (`occluded=true`), которые срабатывают на `G=3` и переписывают `global_id` — прямое объяснение реального бага перепутывания `G=1`/`G=3`, зафиксированного в `docs/state.md:1481`.
- **Способ исправления:** Прекращать обработку (`return;`) до `bind_slot()` и `pending_switch_hits` при `res.occluded || res.clipped`; в `apply_bound_result()` проверять `(m.second_sim < -1.0f || m.best_sim - m.second_sim >= kReidMargin)`.

### [CH-10] `Tracker::spawn()` голодает из-за удержания слотов `TRACK_DEAD` до 4 с в `tracks_[8]`
- **Файл и строки:** `src/tracker.cpp:85-88, 270-274, 352-353`, `include/tracker.hpp:34, 68`.
- **Суть и сценарий воспроизведения:** Подтверждённые треки переходят в `TRACK_DEAD` при `miss > 6` (~0.24 с), но удерживают свой слот в массиве `tracks_[8]` (`kMaxTracks = 8`), пока не опустеет `TrailWindow` (`kTrailMaxAgeMs = 4000 ms`). При этом `Tracker::spawn()` ищет только `t.life == TRACK_FREE`. В насыщенной сцене или при частых кратковременных окклюзиях 3–5 «мёртвых» шлейфов выедают половину лимита, и новые/вышедшие из тени люди молча игнорируются до 4 секунд.
- **Способ исправления:** Если в `Tracker::spawn()` нет слота `TRACK_FREE`, вытеснять самый старый слот в состоянии `TRACK_DEAD` (с минимальным `ts_last_match_us`).

### [CH-11] `Tracker::anchor_clipped()` даёт скачки `cy_meas` на ~95–148 px за 1 кадр при бинарном переключении `top_anchored`
- **Файл и строки:** `src/tracker.cpp:221-227`.
- **Суть и сценарий воспроизведения:** `top_anchored = t.bh > 0.0f && (std::fabs(det_top - pred_top) + 8.0f < std::fabs(det_bot - pred_bot))` переключает измерение центра `cy_meas` скачком между `cy - shift` и `cy + shift`. Симуляция точных уравнений C++ в Python показала: при `hp=400, bh=250` сдвиг границы на `2 px` через порог `+8.0f` даёт скачок **`148.0 px`** за 1 кадр; в round-2 ассоциаторе (`update_bh=false`, `hp=200, bh=75`) при джиттере `±1 px` возле середины якорь **прыгает каждый кадр** (`334.5 -> 421.9 -> 322.6 -> 418.3`, скачки `95–100 px`, `kf_vy` мечется `+33.7..-111.1 px/s`).
- **Способ исправления:** Заменить бинарный флаг на плавную линейную интерполяцию по разнице невязок `delta = |det_bot - pred_bot| - |det_top - pred_top|` (`alpha = clamp((delta + 8.0f) / 16.0f, 0.0f, 1.0f)`, `cy_meas = cy + (1.0f - 2.0f * alpha) * shift`), либо фиксировать сторону якоря с гистерезисом на трек.

---

## 2. Medium (алгоритмические дефекты, деградация FPS/задержки, граничные случаи)

Для всех пунктов ниже оценено влияние на эталон **`ref_evening`** (1 камера `1280x960`, 10 FPS, 1 идущий человек + статичный стул, без зрителя H.264/Re-ID коллизий) и на **живой прогон**.

1. **[M-1] Несоответствие `EncodePacer::admit_idle(now_us)` и `frame.ts_us` (`t1 - t0_`) глушит живые кадры H.264 в смешанном мульти-вью** (`src/pipeline.cpp:189-199, 262, 292`, `src/core/encode_pacer.cpp:12-30`, `src/net_encoder.cpp:66, 123`).
   - *Механизм:* `idle_tick()` передаёт текущее время публикации `now_us`, а живые кадры несут время до декодирования/NPU (`t1 - t0_`, отстаёт на 15–80 мс). При 2+ источниках с >=1 отсутствующей камерой `admit()` видит `ts_us - last_ts_us_ < min_step_us` и отбрасывает триггер кодирования для живого кадра.
   - *Влияние:* **`ref_evening`**: отсутствует (`nsrc=1`, без `--net`). **Живой прогон**: ограничение FPS живой камеры частотой `idle_tick` (30 -> 25 FPS) и джиттер задержки ~40 мс при пропадании второй камеры.
   - *Фикс:* Передавать в `EncodePacer` время публикации `clk::now() - t0_` (оставив `t1 - t0_` для трекера) и защитить `admit_idle()` от перезаписи более нового `last_ts_us_`.

2. **[M-2] Повторный сброс/переподключение камеры не сбрасывает `Tracker`, `BoxSmoother`, `OverlaySink` и оставляет активными слоты `GlobalTrackTable`** (`src/pipeline.cpp:343-358, 465-472, 492-501`, `src/core/global_track.cpp:275`).
   - *Механизм:* `handle_source_failure()` не вызывает `global_tracks_.sync_cam(src, empty, ts)` и не сбрасывает `tracker`/`smoother`; при `seq != seen_attach_seq` сбрасывается только `sa.orient`; при `AutoOrient` `res.changed` не сбрасываются `OverlaySink::callout_hist_` и `track_stab_`.
   - *Влияние:* **`ref_evening`**: отсутствует. **Живой прогон**: при переподключении USB/IP-камеры новые люди на 6 кадров цепляют старые ID/скорости/шлейфы, а старые `GlobalTrack` вечно висят `active=true` в таблице на 32 слота, обходя 45-секундный TTL.
   - *Фикс:* Сбрасывать `tracker`, `smoother`, `trajs_`, `OverlaySink::reset_source(src)` и вызывать `global_tracks_.sync_cam(src, empty, now)` при `detach` и смене `attach_seq`/`res.changed`.

3. **[M-3] Медленная «лесенка» и рывки `BoxEnvelope` на джиттере (`BoxSmoother` и `OverlaySink`)** (`include/core/box_envelope.hpp:67-115`, `src/box_smoother.cpp:80`, `src/overlay_sink.cpp:267-315`).
   - *Механизм и Python-симуляция:* При медленной ходьбе (`0.2..0.8 px/кадр`) с реалистичным джиттером `±0.5..1.0 px` условие `drift` (`net >= 0.9 * npath`) срывается от смены знака джиттера. По точной Python-симуляции C++-уравнений: на `0.4–0.5 px/кадр ±1.0 px` за `119` шагов `BoxEnvelope` замораживается на `35–64` кадра (`4–6` циклов lock/unlock), затем прыгает до **`4.57–5.08 px`** (`6–16x` от реальной скорости) при лаге до `6.6 px`. При старте ходьбы из зафиксированного состояния (`0.5 px/кадр` без шума, `OverlaySink(0.0)`) рамка стоит **10 кадров (`5.5 px` лаг)**, затем прыгает на `+2.75 px` и `+3.25 px` (`6.5x`), а возврат после coasting (`kMaxMiss=6`) даёт скачок `50%/50%` (`9–15 px`).
   - *Влияние:* **`ref_evening`**: стационарный стул (`cls=58`) фиксируется 12 раз (`253/715` нулевых шагов — полезно), человек всегда идёт быстро (`mean|dx|=5.67 px`), поэтому `OverlaySink` не блокируется (`0` локов). **Живой прогон**: заметная «лесенка» и лаг рамок при медленном движении и начале шага.
   - *Фикс:* Проверять `drift` на скользящем подокне (или по знаку EMA-скорости), а в `OverlaySink` не накладывать второй `BoxEnvelope(0.0)` поверх KF-трекера (или сузить `pad` / `0.75 * max_span`).

4. **[M-4] Двойная фильтрация в `OverlaySink` (расхождение рамки и шлейфа) и рассинхрон цветов `t.id` vs `display_id()`** (`src/overlay_sink.cpp:267-315`, `src/box_mapper.cpp:98, 209-218`, `src/core/callout.cpp:226-235, 278`).
   - *Механизм:* `OverlaySink` прогоняет боксы через `BoxEnvelope(0.0)`, а `BoxMapper::trail_points()` читает сырые постерноры KF. Кроме того, `trail_color()` берёт локальный `t.id & 7`, а `Callout` — `display_id() & 7`, а `CalloutHistory` индексируется по `display_id()` (теряя гистерезис в момент привязки `global_id`).
   - *Влияние:* **`ref_evening`**: расхождение `0.000 px` (человек не стоит 8 кадров; Re-ID не активен). **Живой прогон**: на стоящем человеке при джиттере ±2..4 px рамка и шлейф расходятся до `2.0–3.8 px` (до `10.5 px` при выходе из лока), цвет шлейфа отличается от цвета рамки/выноски (`docs/state.md:1481`), выноска моргает при получении `G=N`.
   - *Фикс:* Использовать `t.display_id()` для цвета шлейфа (или `t.id` в обоих местах), индексировать `CalloutPrevSlot` по `(id, gen)` и согласовать источник координат рамки и шлейфа.

5. **[M-5] Устаревание `OverlaySink::track_stab_` и `callout_hist_` при отключении/подключении зрителя** (`src/overlay_sink.cpp:267-300, 348-355`).
   - *Влияние:* **`ref_evening`**: отсутствует. **Живой прогон**: когда зритель отключается и подключается вновь, выжившие `(id, gen)` треки на 1 кадр рисуются в середине между позицией до отключения и текущей (`eff_a=0.5f`).
   - *Фикс:* Очищать `track_stab_` и `callout_hist_` при `!enc_live && !prev_live` или на переходе `idle -> live`.

6. **[M-6] `BoxMapper::fill()` вытесняет живые боксы людей точками шлейфа при лимите 128 боксов** (`src/box_mapper.cpp:12-25`, `include/box_mapper.h:20`).
   - *Влияние:* **`ref_evening`**: отсутствует (1 человек). **Живой прогон**: при `>= 6–7` людях (у каждого до 20 точек шлейфа + `TRACK_DEAD`) 128-элементный `GpuBoxList` забивается шлейфами первых треков, и рамки последних людей полностью пропадают с экрана (нарушение контракта `box_mapper.h:20`).
   - *Фикс:* Разделить `BoxMapper::fill()` на 3 последовательных прохода: (1) `det_boxes`, (2) все `track_box` (`TENT`/`CONF`), (3) `trail_points`.

7. **[M-7] Сдвиг на 1 px в `Callout::map_track_box()` при нечётном `LbGeom` и `static_cast<int>(NaN)` UB** (`src/core/callout.cpp:34-54`).
   - *Влияние:* **`ref_evening`**: отсутствует (`1280x960 -> 640x640` имеет чётный `lb=(0,640,80,480)`). **Живой прогон**: на разрешениях вроде `848x480` (`lb.top=139`) или `720x1080` (`lb.left=107`) выноска и её коннектор съезжают на `-1 px` относительно `BoxMapper`.
   - *Фикс:* Считать координаты из `lb.left, lb.top`, используя `crop` (`& ~1`) только для обрезки границ, и добавить проверку `std::isfinite`.

8. **[M-8] `AutoOrient`: буст EXIF `+0.35f` может зафиксировать поворот на шуме `0.25`, а `NaN` обнуляет кадр** (`src/core/auto_orient.cpp:135, 233-238`).
   - *Влияние:* **`ref_evening`**: отсутствует. **Живой прогон**: при `!raw_portrait_` и `adj[ei] == 0` два кадра случайного ложного срабатывания `0.25` (`0.35 + 0.35 = 0.70 >= 0.45`) навсегда фиксируют поворот; один `NaN` в `d.conf` зануляет весь скор кадра.
   - *Фикс:* Начислять `kExifTieBoost` только при `adj[ei] > 0.0f` и отфильтровывать `!std::isfinite(d.conf)`.

9. **[M-9] Нехватка проверок RGA3 `>= 68 px`, чётности `aux_fd` и границ `>8x` в `RgaResizer` и `SourceChannel`** (`include/source/source_channel.hpp:291-293`, `src/rga_resizer.cpp:58-149`, `src/pipeline.cpp:527, 629`).
   - *Механизм:* `aux_fd` выделяется как `(w/2)*(h/2)*2` (усечение нечётных, `w_aux = raw_w/2` может быть нечётным для `NV12`); аппаратный зонд подтвердил падение `RGA3` на `< 68 px` (`64x64 st=0`); `crop_resize` вызывается без `aux_fd` и без проверок pass-2 `> 8x`, молча возвращая ошибки без лога.
   - *Влияние:* **`ref_evening`**: отсутствует (`1280x960`). **Живой прогон**: сбои RGA на нестандартных разрешениях, экстремальных аспектах (<68 px полосы) или больших Re-ID моделях (`>8x`).
   - *Фикс:* Выровнять `w_aux/h_aux` до чётных `>= 68`, проверять `>= 68 px` и пределы `8x` в обеих функциях `RgaResizer`, передавать `aux_fd` в `crop_resize` при необходимости.

10. **[M-10] `GpuBoxDrawer`: двойное альфа-наложение углов рамок, отсутствие offscreen-cull, переполнение вершин выноски (`192 < 228`), отсутствие `destroy_sync` и `glFinish()` на таймауте** (`include/gpu_box_drawer.hpp:46, 68`, `src/gpu_box_drawer.cpp:77, 115-161, 407, 647-668`).
    - *Влияние:* **`ref_evening`**: отсутствует (нет `--net`). **Живой прогон**: полупрозрачные точки шлейфа на GPU имеют яркие двойные углы в отличие от `--cpu-draw`; текст > 25 глифов (`kLabelCap=32` допускает 31 -> 228 вершин) молча обрезается на отметке 192; при зависании GPU `glFinish()` блокирует пайплайн без таймаута; `P_ClientWaitSync` объявлен как `EGLBoolean` вместо `EGLint`.
    - *Фикс:* Усечь вертикальные полоски `append_boxes_quads` на `[y0+t, y1-t]`, добавить offscreen-cull, поднять `kVertsPerCallout` до `228`, добавить `&& st_.destroy_sync` и тип `EGLint`.

11. **[M-11] Неучтённые `stride_bytes` в `RknnEngine` (порядок `setup_io`), `CalDumper`, `FrameComposer` и `NetPreview`** (`src/rknn_engine.cpp:160-200`, `src/cal_dumper.cpp:45, 53`, `src/net/frame_composer.cpp:141`, `src/net_preview.cpp:240`).
    - *Влияние:* **`ref_evening` и текущие модели `640x640`/`128x256`**: скрыто (`w_stride == 0` по зонду). **Живой прогон с иной моделью**: при ширине входа, требующей выравнивания `w_stride`, `in_mems_` окажутся недовыделены, а `CalDumper`, `FrameComposer` и `NetPreview` исказят строки диагональным сдвигом.
    - *Фикс:* Пересчитывать/перевыделять `in_mems_` после `rknn_set_io_mem`, передавать `stride_bytes` во все 3 потребителя.

12. **[M-12] Глобальный сброс таймеров простоя `reset_stall_clocks` маскирует зависание соседних камер** (`src/pipeline.cpp:253, 267`, `include/source/source_set.hpp:622-626`).
    - *Влияние:* **`ref_evening`**: отсутствует (1 источник). **Живой мульти-кам прогон**: если камера `s1` падает каждые < 2 с, `reset_stall_clocks()` обнуляет таймер простоя у `s0`, и повисшая `s0` никогда не перезапустится.
    - *Фикс:* Не сбрасывать `last_ok_` у исправных соседних каналов (максимум компенсировать реальное время блокировки).

13. **[M-13] Конкуренция ядер NPU между `InferencePool` и `ReidWorker` + отсутствие A76 affinity у `BasicReidQueue`** (`include/inference_pool.hpp:195-212`, `src/reid_worker.cpp:19-20`, `include/core/reid_queue.hpp:33, 53-77`).
    - *Влияние:* **`ref_evening`**: ничтожно (~6.1 мс Re-ID при 10 FPS). **Живой прогон**: в `--pool 1` Re-ID на `core 2` конкурирует с YOLO при `--core 0/7`; в `--pool 3` с толпой (до 60 Re-ID/с) `core_mask=0` тормозит один из 3 YOLO-воркеров и стопорит строгий FIFO `collect()`; поток Re-ID может планироваться на слабые A55.
    - *Фикс:* Привязать поток Re-ID к A76 и согласовать маски ядер с `--pool`.

14. **[M-14] `yolo_head_shape` пропускает `hd.ch < 15` (OOB-чтение) и нехватка `isfinite`/`w>0` в S2/S3** (`src/core/yolo_decoder.cpp:133-138, 176`, `src/box_mapper.cpp:134-151`, `include/core/box_envelope.hpp:33, 67`, `src/tracker.cpp:428-458`, `src/box_smoother.cpp:148-167`).
    - *Влияние:* **`ref_evening` и штатный пайплайн**: защищено проверкой `255` каналов в `Pipeline` и фильтрацией `YoloDecoder::emit()`, но опасно как баг памяти при прямом вызове `core/` API или смене числа классов.
    - *Фикс:* Требовать `hd.ch >= 18 && hd.h > 0` в `yolo_head_shape` и добавить проверки `std::isfinite`/`w>0, h>0` в `BoxMapper`, `BoxEnvelope`, `Tracker`, `Associator`, `BoxSmoother`.

15. **[M-15] Ошибки конфигурации, `enc_lb` 4:3, `handle_source_failure`, `CameraV4L2`, медленный HTTP, `H264Encoder` `EAGAIN` и `ParamSetCache`**:
    - `src/config/cli.cpp:166-170`, `src/config/option_registry.cpp:181-193`: `--src-rot` с CLI не может переопределить источник из INI/по умолчанию и печатает вводящую в заблуждение ошибку `(expected 0|90|180|270)`.
    - `src/pipeline.cpp:120-121`: жёсткий `enc_lb` 4:3 сжимает портретный single-view поток до `270x480` (-58% ширины) и добавляет полосы к 16:9.
    - `src/pipeline.cpp:266`: `static_cast<void>(handle_source_failure(df))` проглатывает не-`Lost` фатальные ошибки декодера как `Step::Skipped`.
    - `src/camera_v4l2.cpp:85-86, 257-269`, `include/media_source.hpp:64-72`: `width_/height_` «залипают» при провале `VIDIOC_STREAMON` или `trial()`.
    - `src/net_mjpeg_source.cpp:94-111, 185-204`, `src/source/buffered_socket.cpp:28-38`: slow-drip HTTP-сервер сбрасывает 2-секундный watchdog бесконечно; `getaddrinfo()` блокирует `SourceProber` и остановку.
    - `src/mpp_decoder.cpp:241-245`, `include/source/source_channel.hpp:152-153, 192-193`: умирающая V4L2-камера с битыми кадрами (без SOI/EOI и без `V4L2_BUF_FLAG_ERROR`) возвращает `fd=-1` и никогда не делает `detach()`.
    - `src/net/h264_encoder.cpp:230-232`: при `EAGAIN`-дренировании старому пакету передаётся `fpts` нового кадра.
    - `src/core/param_set_cache.cpp:47-60, 123-141`: `annexb::is_idr` пропускает только NAL `6..9`, а `idr_buf_` может сделать heap-реаллокацию на кадрах >256 КБ.
    - `include/dma_sync.hpp:39, 48`: `DMA_BUF_IOCTL_SYNC` не повторяется при `EINTR`.

*(Отдельно подтверждено как **ложные тревоги / чистые зоны** после прямой проверки, замеров и зондов: потеря кэша выхода YOLO из-за отсутствия `sync_output()` опровергнута зондом при `flag=0`; `NC1HWC2` несоответствие в `YoloDecode` опровергнуто — RKNN сам конвертирует в `NCHW`; утечка слотов `ReidWorker` в `run_reid_stage` опровергнута — `try_acquire_slot()` не блокирует слот; гонка `set_prober_quit` и stale `geom_` при `exif_rot_` опровергнуты; `KalmanAxis` устойчив в `float32` на 10k шагов `min_det >= 49.4`; `glyph_font` `144x154` NPOT с `highp float` точен до `7.87e-6` текселя; `EncoderFramePool<8>` достаточен для `H26X_ASYNC_FRAMES=4` + `LOW_DELAY`; `TcpPullServer` `abort_client`/`close_client` защищены `cfd_mx_`.)*

---

## 3. Low / Чистота кода (стандарты `CONTRIBUTING.md`, STL, комментарии, тесты)

1. **Нарушения правил заголовков и стиля `CONTRIBUTING.md`:**
   - Порядок `#include` (`<...>` до `"..."`): `include/rkmpp_drm_pool.hpp:15,21` (`"core/status.hpp"` до `<rockchip/rk_mpi.h>`), `src/signal_waiter.cpp:1-7`, `src/process_env.cpp:1-6`, `src/cpu_affinity.cpp:1-3`.
   - Секции видимости в классах (методы до `private:`-полей, свежая метка перед данными): `include/rkmpp_drm_pool.hpp:69-89` (`public:` поля `group`, `buf` после `private:`), `include/core/log.hpp:101-108` (`line()` и поля под одним `private:`), `include/signal_waiter.hpp:59-68`, `include/tcp_pull_server.hpp:21-23`.
   - Длина строк > 140 символов: `include/dma_sync.hpp:81,110` (`146` и `147` симв.); хвостовой пробел `include/coco_names.h:4`.
   - Константность и `noexcept` на hot-path: `include/rga_resizer.h:33` (`resize()` не `const`); отсутствие `noexcept` в `SourceChannel::{poll,detach}`, `SourceSet::{next,next_blocking}`, `MediaSource`, `CameraV4L2`, `NetMjpegSource`, `MppDecoder::decode`, `GpuBoxDrawer::{draw_slot,sync_cpu}`, `NetPreview::publish`; вызовы `std::ostream`-логирования (`Log`) внутри `noexcept`-функций (`InferencePool`, `RgaResizer`, `YoloDecoder`).
   - Вся папка `core/` подтверждена **чистой** от SDK/GL/OS-зависимостей; во всём проекте подтверждены **zero virtuals**, **zero manual heap (`new`/`delete`/`unique_ptr`/`shared_ptr`)**, **zero `static`**.

2. **Мелкие дефекты, `CMake`, логика статистики и устаревшие комментарии/документация:**
   - `include/source/source_prober.hpp:56, 62-72`: `prober_quit_` не сбрасывается в `nullptr` в `stop()`/`~BasicSourceProber()` (висячий указатель на стек `Pipeline::run()`).
   - `src/camera_v4l2.cpp:235-247`: `close(fd_)` вызывается до `munmap()`.
   - `src/pipeline.cpp:317-323`: при `Step::Fatal` пропускаются `pool_` drain и итоговые сводки `Timings`/`ConsoleReporter`.
   - `src/timings.cpp:33-36`, `src/pipeline.cpp:601-603`: кадры с `!npu_ok` портят среднее время `Timings`, а при `pool_ > 1` с `run_.n == 1` возможно деление на `0` (`inf` FPS).
   - `src/net/h264_encoder.cpp:198`: `frames_sent_` не сбрасывается между сессиями зрителя; `src/config/cli.cpp:160`, `config_file.cpp:74-75`: `--config ""` и пустая секция `[]` тихо игнорируются; `CMakeLists.txt:318` форсирует `-O3` для любых `CMAKE_BUILD_TYPE`.
   - `src/core/yolo_decoder.cpp:223-229`: `+Inf` в канале класса побеждает конечные значения; `src/rknn_engine.cpp:100-111`: `tensor_elem_bytes()` возвращает `1` в `default:`; `src/reid_worker.cpp:25-27`: формат выхода Re-ID не валидируется на старте.
   - `include/box_smoother.h:75-77`: `const float` поля мешают копирующему присваиванию; `include/tracker.hpp:60`: `kVClampPxs = 500` зависит от разрешения; `src/core/callout.cpp:304-307`: порядок выносок зависит от переиспользования слотов `tracks_[8]`.
   - Устаревшие комментарии и документация: `include/net_encoder.h:82` (`"4 NV12 dma-bufs"` вместо `8`), `docs/state.md:1469` и `README.md:6,111,151` (устаревшие контрольные суммы `ref_evening.csv` и счётчики файлов/тестов).

3. **Недостающие тесты в `tests/`:**
   - Нет тестов на >256 КБ JPEG в многокамерном неблокирующем `NetMjpegSource` и slow-drip таймауты.
   - Нет unit-тестов для `RgaResizer`, `YoloDecode` (`yolo_postprocess`), `DmaSync`, `TcpPullServer` (передача/обрыв/`abort_client`), `OverlaySink`, `FrameComposer`, `H264Encoder`, `NetPreview`, `Pipeline` (`test_pipeline.cpp`), `cpu_affinity`, `throw_on`, `fixed_fmt.hpp`, `line_scan.hpp`.
   - Пробелы в существующих тестах: `test_box_mapper` (приоритет боксов людей над шлейфами при 128 слотах, `NaN`), `test_box_smoother` / `BoxEnvelope` (джиттер 0.4–0.8 px/кадр, `NaN`, coast jump), `test_global_track` (32-слотовое насыщение и запрет переключения на `occluded`/`clipped`), `test_auto_orient` (`NaN` conf и EXIF-буст на пустом кадре), `test_yolo_decoder` (`hd.ch < 15`), `test_inference_pool` (быстрый `discard()` на зависших слотах), `test_config` (`--src-rot` с INI, `--config ""`).

---

## 4. План исправлений (3 небольших PR по приоритету)

### PR 1 (`Critical/High — Железо RK3588, декодер MPP, сеть MJPEG и неблокирующий пайплайн`)
**Цель:** Устранить аппаратные сбои RK3588, десинхронизацию DMA-кэша, потерю `stride`, блокировки главного потока и регрессию >255 КБ MJPEG.
1. `NetMjpegSource` (`[CH-1]`): потоковое чтение фрагментов >256 КБ в `MjpegSlotPool` в многокамерном режиме (`timeout_ms >= 0`) + регрессионный тест.
2. `MppDecoder` + `RgaResizer` (`[CH-2]`, `[CH-3]`): выравнивание ширины и передача `hor_stride`/`ver_stride` в 6-аргументный `wrapbuffer_fd()`, исправление отката `apply_info_change()` и полной до/после зачистки `reset_and_strip()`.
3. `RgaResizer::crop_resize` (`[CH-4]`): перевод кропов `>= 68x68` на `IM_SCHEDULER_RGA3_CORE0` (IOMMU) и безопасный DMA32/`<4G` путь для мелких кропов `< 68 px`.
4. `encoder_config.cpp` (`[CH-5]`): чётное округление `l.tile_h` в single-view + обновление `test_encoder_config.cpp`.
5. `FrameComposer::fill_nv12_gray_rect` (`[CH-6]`): добавить `dma::cache_drop()` до `memset` (или 64-байтное выравнивание `tile_w`).
6. Неблокирующий hot-path (`[CH-7]`, `[CH-8]`): `InferencePool::discard()` с таймаутом `0 ms` + вынос `av_write_frame()`/`avio_flush()` в `NetPreview` на поток `serve()` через 2-слотовый кольцевой буфер.

### PR 2 (`High/Medium — Качество трекинга, Re-ID, стабилизация боксов/оверлея и оркестрация`)
**Цель:** Устранить перепутывания Re-ID (`G=1/G=3`), голодание слотов трекера, скачки `anchor_clipped` и рассинхроны мульти-камеры.
1. `GlobalTrackTable` (`[CH-9]`): запрет `bind_slot()` и `pending_switch_hits` при `occluded || clipped`, добавление `kReidMargin` в `apply_bound_result()`, + очистка при отключении камеры (`[M-2]`).
2. `Tracker` (`[CH-10]`, `[CH-11]`): вытеснение самого старого `TRACK_DEAD` в `spawn()` при нехватке `TRACK_FREE`; плавная линейная интерполяция в `anchor_clipped()` вместо скачка на `+8.0f`.
3. Оверлей без смены фильтра (`[M-4]`, `[M-5]`, `[M-6]`, `[M-7]`): сброс `OverlaySink` при отвале зрителя и смене ориентации/переподключении; единый цвет по `display_id()` + гистерезис `CalloutPrevSlot` по `(id, gen)`; 3-проходный `BoxMapper::fill()`; фикс 1-px сдвига в `Callout::map_track_box()`. *(Примечание: доработка `[M-3]` старого `BoxEnvelope` снята во избежание двойной работы и перенесена в PR 3 в виде перехода на единый адаптивный фильтр отображения).*
4. Оркестрация, конфиги и периодические подвисания трансляции (`[M-1]`, `[M-8]`, `[M-12]`, `[M-13]`, `[M-15]` ч.1): публикационное время для `EncodePacer`, по-канальный учёт stall-таймеров, проверка возврата `handle_source_failure()`, фикс `AutoOrient` EXIF boost + `NaN`, переопределение `--src-rot` с CLI, привязка потока `BasicReidQueue` к A76, а также профилирование и устранение периодических подвисаний живой трансляции (связка `EncodePacer` / `NetEncoder` / `NetPreview` / ожидание кадров источников, чтобы не делать двойную работу с `[M-1]` и `[M-8]`).

### PR 3 (`Medium-Defense / Low — Защита граничных случаев, синхронизация GPU/H.264, чистота кода и тесты`)
**Цель:** Закрыть латентные баги граничных входов, привести код к 100% соответствию `CONTRIBUTING.md` и закрыть пробелы в unit-тестах.
1. Проверки границ и драйверов (`[M-9]`, `[M-10]`, `[M-11]`, `[M-14]`, `[M-15]` ч.2): проверки `>= 68 px`/чётности/`8x` в `RgaResizer`, `in_size`/`stride_bytes` в `RknnEngine`/`CalDumper`/`FrameComposer`/`NetPreview`, `hd.ch >= 18` в `YoloDecoder`, `isfinite`/`w>0` в `BoxMapper`/S3, `EINTR` в `DmaSync`, `destroy_sync`/`EGLint`/двойное альфа-наложение/`kVertsPerCallout=228` в `GpuBoxDrawer`, откат `width_/height_` и `munmap`-до-`close` в `CameraV4L2`, wall-clock таймауты в `NetMjpegSource`, `EAGAIN` `fpts` в `H264Encoder`, `ParamSetCache` NAL-парсер.
2. Чистота кода и документация (`Low §1–2`): исправление порядка `#include` и `private:`-меток (`rkmpp_drm_pool.hpp`, `log.hpp`, `signal_waiter.*`, `process_env.cpp`, `cpu_affinity.cpp`, `tcp_pull_server.hpp`), длина строк в `dma_sync.hpp`, `noexcept`, обнуление `prober_quit_`, `Timings` (`dur > 0` и разделение `!npu_ok`), обновление `docs/state.md` и `README.md`.
3. Тесты (`Low §3`): добавление недостающих модульных тестов для `RgaResizer`, `YoloDecode`, `BoxMapper`, единого фильтра отображения (взамен `BoxEnvelope`), `GlobalTrackTable`, `AutoOrient`, `TcpPullServer`, `Pipeline`, `fixed_fmt`, `line_scan`.
4. **Единый фильтр отображения рамок (замена `BoxEnvelope` / двойного сглаживания, вместо `[M-3]`):**
   - *Цель и место:* Исследовать и заменить связку `OverlaySink::BoxEnvelope` (поверх `KalmanAxis` трекера) и сглаживание отображения в `BoxSmoother` на единый адаптивный фильтр (например, One-Euro с гистерезисной мёртвой зоной для статики), без изменения внутренней ассоциации/Re-ID `Tracker` (сохраняя побайтовую идентичность `ref_evening.csv`) и с согласованием рамки и шлейфа в `OverlaySink`.
5. **Подписи к рамкам без фоновых плашек с пропорциональным шрифтом (`Callout` / `GpuBoxDrawer` / `glyph_font`):**
   - *Фоновая плашка убирается полностью:* никаких сплошных или полупрозрачных подложек/прямоугольников под текстом, перекрывающих видеопоток.
   - *Отказ от моноширинности:* снятие плашки убирает привязку к моноширинному шрифту — переход на нормальный пропорциональный шрифт (Roboto/Inter/DejaVu, 14–16 pt) с constexpr таблицей авансов и смещений (advance_x, offset_x/y).
   - *Стиль подписи:* чёткий белый шрифт с тёмной тенью/обводкой (drop-shadow / outline / halo) для идеальной контрастности на любом фоне (светлый/тёмный/пёстрый) без размытия NV12/H.264.
   - *Масштаб:* компенсация коэффициента сжатия place_tile между камерами (4:3 USB vs портретный телефон), чтобы физический размер букв был одинаковым на всех каналах.

---
## Статус реализации плана исправлений (раздел 4)

**Обновлено:** 2026-11-20

| Этап | Состояние | Дата | Issue / ветка / PR / коммит |
|---|---|---|---|
| PR 1 (Critical/High) | Завершён | 2026-11-18 | без issue → `fix/audit-p2-pr1` → PR #88 → `3117d76` влит в `fix/audit-p2` |
| PR 2 (High/Medium) | Завершён | 2026-11-19 | без issue → `fix/audit-p2-pr2` → PR #89 → `8c6bf24` влит в `fix/audit-p2` |
| PR 3 (Medium-Defense/Low) | Завершён | 2026-11-19 | issue #91 → `fix/audit-p2-pr3` → PR #92 → `1a0ee5f` влит в `fix/audit-p2` (этапы 3a и 3b) |
| Доп.: инструменты (clangd 22, `TIDY_FILES`, лимиты размера функций в `src/`, правила проекта в гейте) | Завершён | 2026-11-20 | issue #93 → `fix/audit-p2-tooling` → PR #95 → `12fdf24` влит в `fix/audit-p2` |
| Доп.: гейт `misc-const-correctness` в проходе `rules` | Завершён | 2026-11-20 | issue #94 → `fix/audit-p2-const` → PR #98 → `1cb2ab4` влит в `fix/audit-p2` |
| Доп.: заметки о SDK платы для агентов (`CLAUDE.md`) | Завершён | 2026-11-20 | без issue → `docs/agent-sdk-notes` → PR #99 → `62d06a2` влит в `fix/audit-p2` |
| Доп.: `noexcept` для закрепления потока на больших ядрах при старте; `#error` для неизвестной архитектуры в операциях DMA-кэша | Завершён | 2026-11-20 | issue #96 → `fix/audit-p2-noexcept-pin` → PR #100 → `6f738d8` влит в `fix/audit-p2` |
| Доп.: доработки по ревью PR #92 и замечания ревью PR #98–#100 (обводка подписей, `sizeof(Pipeline)`, наборы `BoxFilter`, дрожание рамки person, tidy-probe, документация) | Завершён | 2026-11-20 | issue #97 → `fix/audit-p2-followups` → PR #101 → `69c8eb8` влит в `fix/audit-p2` |
