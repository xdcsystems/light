# -*- coding: utf-8 -*-
import csv, os
from collections import defaultdict

def load(p):
    with open(p) as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        r['name'] = os.path.splitext(r['name'])[0]   # n1_cal_0000.png и .ppm → одно имя
    return rows

def iou(a, b):
    ax1, ay1, ax2, ay2 = map(float, (a['x1'], a['y1'], a['x2'], a['y2']))
    bx1, by1, bx2, by2 = map(float, (b['x1'], b['y1'], b['x2'], b['y2']))
    ix = max(0.0, min(ax2, bx2) - max(ax1, bx1))
    iy = max(0.0, min(ay2, by2) - max(ay1, by1))
    inter = ix * iy
    union = (ax2-ax1)*(ay2-ay1) + (bx2-bx1)*(by2-by1) - inter
    return inter / union if union > 0 else 0.0

ref = load('misc/ref_preds.csv')
brd = load('misc/board_preds.csv')
frames = sorted(set(r['name'] for r in ref) | set(r['name'] for r in brd))

matched, missed, extra, dsum = 0, 0, 0, 0.0
per_cls = defaultdict(lambda: [0, 0])
for name in frames:
    R = [r for r in ref if r['name'] == name]
    B = [b for b in brd if b['name'] == name]
    used = set()
    for r in R:
        per_cls[r['cls']][1] += 1
        best, best_iou = None, 0.5
        for j, b in enumerate(B):
            if j in used or b['cls'] != r['cls']:
                continue
            v = iou(r, b)
            if v > best_iou:
                best, best_iou = j, v
        if best is None:
            missed += 1
        else:
            used.add(best)
            matched += 1
            per_cls[r['cls']][0] += 1
            dsum += abs(float(r['score']) - float(B[best]['score']))
    extra += len(B) - len(used)

print(f'эталон: {len(ref)} детекций, плата: {len(brd)}')
print(f'совпало (IoU>0.5): {matched}, пропущено: {missed}, лишних: {extra}')
print(f'средняя |Δscore|: {dsum/matched if matched else 0:.3f}')
print('по классам (совпало/всего):')
for c in sorted(per_cls, key=lambda x: -per_cls[x][1]):
    m, t = per_cls[c]
    print(f'  cls{c}: {m}/{t}')
