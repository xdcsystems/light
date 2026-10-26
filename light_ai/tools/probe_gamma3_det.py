# -*- coding: utf-8 -*-
# Детекционный A/B условной гаммы 3.0: домен x вариант {orig, g3} x порог {0.18, 0.25}.
# Декод+NMS общий, из tools/postprocess.py (как в batch_eval).
#
# Трюк с порогами: декод один раз на 0.18, порог 0.25 — фильтрацией по score.
# Корректно: NMS жадный, бокс со score>=0.25 может подавить только бокс с
# большим score, который и на 0.25 выживает.
#
# Запуск:  python tools/probe_gamma3_det.py [модель.rknn]
# Выход:   gamma3_det.csv (корень) + агрегат в stdout.

import glob
import sys
import csv
import numpy as np
from PIL import Image
from rknnlite.api import RKNNLite

sys.path.insert(0, 'tools')
from postprocess import decode_outputs

MODEL = sys.argv[1] if len(sys.argv) > 1 else 'yolov5s_relu_int8_v2.rknn'
BRIGHT_FROM = 155          # cal_0155..0199 — светлый домен (как в probe_gamma3_full)
THRESHOLDS = (0.18, 0.25)
LB_TOP, LB_H = 140, 360    # видимая полоса 640x640 (s=1/2), как в пайплайне
OUT_CSV = 'gamma3_det.csv'

rk = RKNNLite()
assert rk.load_rknn(MODEL) == 0, f'load_rknn failed: {MODEL}'
assert rk.init_runtime(core_mask=RKNNLite.NPU_CORE_0_1_2) == 0, 'init_runtime failed'

def in_band(dets):
    # Центр по y вне [140, 500) — вылет на серые бары, как в пайплайне: фильтруем.
    if len(dets) == 0:
        return dets
    yc = (dets[:, 1] + dets[:, 3]) / 2
    return dets[(yc >= LB_TOP) & (yc < LB_TOP + LB_H)]

frames = sorted(glob.glob('dataset_night/cal_*.ppm'))
assert frames, 'нет кадров dataset_night/cal_*.ppm'

rows = []                      # (dom, idx, tag, thr, n_det, top5)
conf_store = {}                # (dom, tag, thr) -> список score
for path in frames:
    name = path.split('/')[-1]
    idx = int(name.split('_')[1].split('.')[0])
    dom = 'dark' if idx < BRIGHT_FROM else 'bright'
    img = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    for tag, x in (('orig', img),
                   ('g3', (img / 255.0) ** 3.0 * 255.0)):   # гамма 3.0 поверх запечённой базовой 2.0
        outs = rk.inference(inputs=[x[None].astype(np.uint8)])
        dets18 = in_band(decode_outputs([o[0] for o in outs], conf_thres=0.18, iou_thres=0.45))
        for thr in THRESHOLDS:
            d = dets18[dets18[:, 4] >= thr]
            top = ';'.join(f'{int(c)}:{s:.2f}' for _, _, _, _, s, c in d[:5])
            rows.append((dom, idx, tag, thr, len(d), top))
            conf_store.setdefault((dom, tag, thr), []).extend(d[:, 4].tolist())

with open(OUT_CSV, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['domain', 'frame', 'variant', 'conf_thr', 'n_dets', 'top5'])
    w.writerows(rows)

print(f'\n{"домен":8}{"вариант":8}{"порог":7}{"дет.":>7}{"кадров>=1":>12}{"ср.conf":>10}')
for dom in ('dark', 'bright'):
    for tag in ('orig', 'g3'):
        for thr in THRESHOLDS:
            sel = [r for r in rows if r[0] == dom and r[2] == tag and r[3] == thr]
            tot = sum(r[4] for r in sel)
            with_det = sum(1 for r in sel if r[4] > 0)
            cs = conf_store[(dom, tag, thr)]
            mean_c = np.mean(cs) if cs else 0.0
            print(f'{dom:8}{tag:8}{thr:<7}{tot:>7}{with_det:>12}{mean_c:>10.3f}')

# Покомпонентная дельта g3 против orig на пороге 0.18 — где гамма меняет картину
print('\nдельта g3-orig (порог 0.18), только изменившиеся кадры:')
m = {(r[0], r[1], r[2]): r[4] for r in rows if r[3] == 0.18}
for dom in ('dark', 'bright'):
    changed = 0
    for idx in sorted({r[1] for r in rows if r[0] == dom}):
        d = m.get((dom, idx, 'g3'), 0) - m.get((dom, idx, 'orig'), 0)
        if d != 0:
            changed += 1
            print(f'  {dom} cal_{idx:04d}: orig {m.get((dom, idx, "orig"), 0)} '
                  f'-> g3 {m.get((dom, idx, "g3"), 0)} ({d:+d})')
    if changed == 0:
        print(f'  {dom}: без изменений')
