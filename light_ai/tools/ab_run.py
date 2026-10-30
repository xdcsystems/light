# -*- coding: utf-8 -*-
import glob, csv, sys, argparse
import numpy as np
from PIL import Image
sys.path.insert(0, 'tools')
from postprocess import decode_outputs
from rknnlite.api import RKNNLite

p = argparse.ArgumentParser()
p.add_argument('--model', required=True)
p.add_argument('--pattern', required=True, help='glob дневных кадров, через запятую если несколько')
p.add_argument('--out', required=True)
p.add_argument('--conf', type=float, default=0.18)
p.add_argument('--gamma', type=float, default=1.0, help='1.0 для дня; 2.0 было для ночных серий')
a = p.parse_args()

rk = RKNNLite()
rk.load_rknn(a.model)
rk.init_runtime()
paths = sorted(sum([glob.glob(g) for g in a.pattern.split(',')], []))
assert paths, 'под паттерн ничего не нашлось: ' + a.pattern
rows = []
for path in paths:
    arr = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0
    img = np.expand_dims((arr ** a.gamma * 255).astype(np.uint8), 0)
    outs = rk.inference(inputs=[img])
    for x1, y1, x2, y2, s, c in decode_outputs([o[0] for o in outs], conf_thres=a.conf):
        rows.append([path.split('/')[-1], int(c), round(s, 4),
                     round(x1, 1), round(y1, 1), round(x2, 1), round(y2, 1)])
with open(a.out, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['name', 'cls', 'score', 'x1', 'y1', 'x2', 'y2'])
    w.writerows(rows)
print(a.model.split('/')[-1], '->', a.out, '|', len(paths), 'кадров |', len(rows), 'детекций')
