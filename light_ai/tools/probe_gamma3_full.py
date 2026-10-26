# -*- coding: utf-8 -*-
import glob
import numpy as np
from PIL import Image
from rknnlite.api import RKNNLite

BRIGHT_FROM = 155   # cal_0155..0199 — светлый домен

rknn = RKNNLite()
rknn.load_rknn('./yolov5s_relu_int8_v2.rknn')
rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_0_1_2)

def measure(x):
    outs = rknn.inference(inputs=[x[None].astype(np.uint8)])
    h10 = h50 = 0
    for out, g in zip(outs, (80, 40, 20)):
        obj = out.reshape(3, 85, g, g)[:, 4]
        h10 += int((obj > 0.1).sum())
        h50 += int((obj > 0.5).sum())
    return h10, h50

dom = {'dark': {'orig': [0,0,0,0], 'g3': [0,0,0,0]},
       'bright': {'orig': [0,0,0,0], 'g3': [0,0,0,0]}}
improved, regressed = [], []

for path in sorted(glob.glob('dataset_night/cal_*.ppm')):
    name = path.split('/')[-1]
    idx = int(name.split('_')[1].split('.')[0])
    domname = 'dark' if idx < BRIGHT_FROM else 'bright'
    img = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    for tag, x in (('orig', img),
                   ('g3', (img / 255.0) ** 3.0 * 255.0)):
        h10, h50 = measure(x)
        dom[domname][tag][0] += h10
        dom[domname][tag][1] += h50
        dom[domname][tag][2] += 1 if h10 > 0 else 0   # кадры с активностью
        dom[domname][tag][3] += 1 if h50 > 0 else 0
    if domname == 'dark':
        o10, _ = measure(img)  # orig уже посчитан выше; считаем через кэш ниже
print()
print(f"{'домен':8} {'вариант':8} hot>0.1  hot>0.5  кадров: акт.10/акт.50")
for d in dom:
    for t in ('orig', 'g3'):
        a = dom[d][t]
        print(f"{d:8} {t:8} {a[0]:7d} {a[1]:8d} {a[2]:6d}/{a[3]}")
