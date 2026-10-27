# -*- coding: utf-8 -*-
import glob, csv, sys
import numpy as np
from PIL import Image
sys.path.insert(0, 'tools')
from postprocess import decode_outputs
from rknnlite.api import RKNNLite

rk = RKNNLite()
rk.load_rknn('yolov5s_relu_int8_v2.rknn')
rk.init_runtime()
rows = []
for path in sorted(glob.glob('dataset_mix/n1_cal_*.ppm') +
                   glob.glob('dataset_mix/n2_cal_*.ppm')):
    arr = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0
    img = np.expand_dims((arr ** 2.0 * 255).astype(np.uint8), 0)   # гамма 2.0
    outs = rk.inference(inputs=[img])
    for x1, y1, x2, y2, s, c in decode_outputs([o[0] for o in outs], conf_thres=0.18):
        rows.append([path.split('/')[-1], int(c), round(s, 4),
                     round(x1, 1), round(y1, 1), round(x2, 1), round(y2, 1)])
with open('misc/board_preds.csv', 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['name', 'cls', 'score', 'x1', 'y1', 'x2', 'y2'])
    w.writerows(rows)
print('сохранено: misc/board_preds.csv,', len(rows), 'детекций')
