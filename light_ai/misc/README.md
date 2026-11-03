# misc/ — артефакты light_ai

Каталог `light_ai/misc/` трекается. Правило `misc/*` в корневом `.gitignore` относится только к `misc/` в корне репозитория.

`light_ai/.gitignore`:

- `*.rknn` — модели не коммитятся. Дефолт пайплайна — `./misc/yolov5s_relu_int8_mmse_v5.rknn`, путь от текущего каталога при запуске `./build/light_ai` из `light_ai/`. Файла в дереве нет: его переносят на машину отдельно. Это рабочая модель по `docs/state.md`.
- `*.whl` игнорируется, затем `!misc/*.whl` разрешает колесо в этом каталоге. Сейчас `.whl` в дереве нет.
- `misc/preds_*.csv` и `misc/boxlog_*.csv` игнорируются.
- `misc/README.md` разрешён явно (`!misc/README.md`).

## Что лежит в дереве

CSV прогонов и сверок:

- `ab_v4_day.csv`, `ab_v4_night.csv`
- `ab_v5_day.csv`, `ab_v5_night.csv`
- `board_preds.csv`, `ref_preds.csv`
- `fp16_live_night.csv`
- `gamma3_det.csv`
- `v5_live_night.csv`

История сборок INT8 (v1…v5) — в `docs/state.md`.
