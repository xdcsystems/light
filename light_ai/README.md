# light_ai — zero-copy видеоаналитика на RK3588

Пайплайн: V4L2 (MJPEG) -> MPP (декод в NV12, dma-buf) -> RGA (im2d-ресайз
прямо в dma-buf тензора NPU) -> RKNN (zero-copy) -> CPU-постобработка.

## Правила проекта
- GStreamer запрещён (фиксация от владельца проекта).
- Кадр существует только как dma-buf; копирование пикселей через CPU запрещено.
  Единственный допустимый memcpy - сжатый MJPEG-кадр (~25 КБ) в packet декодера.

## Слои
- CameraV4L2   : захват MJPEG, mmap-буферы, DQBUF/QBUF.
- MppDecoder   : декод MJPEG -> NV12, внешний буферный пул dma-buf, отдаёт fd.
- RgaResizer   : im2d-ресайз по fd (wrapbuffer_fd + imresize).
- RknnEngine   : rknn_create_mem + rknn_set_io_mem; fd входного тензора
                 отдаётся RGA как приёмник (ресайз пишет прямо в тензор NPU).

## Сборка
    ./scripts/build.sh

## Запуск
    sudo ./build/light_ai --device /dev/video20 --frames 60

## Фазы
- M1: базовый прогон с mobilenet_v1, замер честного FPS.
- M2: конвертация yolov8n.onnx -> .rknn (rknn-toolkit2 на x86), вход 640x640.
- M3: вход модели NV12 - RGA без конвертации формата, CPU-квантование исчезает.
- M4: конвейеризация (async RGA), governor NPU/DDR = performance.
