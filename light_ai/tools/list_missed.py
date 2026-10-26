# -*- coding: utf-8 -*-
import csv, os

def load(p):
    with open(p) as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        r['name'] = os.path.splitext(r['name'])[0]
    return rows

def iou(a, b):
    ax1, ay1, ax2, ay2 = map(float, (a['x1'], a['y1'], a['x2'], a['y2']))
    bx1, by1, bx2, by2 = map(float, (b['x1'], b['y1'], b['x2'], b['y2']))
    ix = max(0.0, min(ax2, bx2) - max(ax1, bx1))
    iy = max(0.0, min(ay2, by2) - max(ay1, by1))
    inter = ix * iy
    union = (ax2-ax1)*(ay2-ay1) + (bx2-bx1)*(by2-by1) - inter
    return inter / union if union > 0 else 0.0

ref = load('ref_preds.csv')
brd = load('board_preds.csv')
print('пропущенные на плате (FP32-скор → ближайший INT8-скор того же класса):')
for r in ref:
    cands = [b for b in brd if b['name'] == r['name'] and b['cls'] == r['cls']]
    if not cands or max(iou(r, b) for b in cands) <= 0.5:
        near = max(cands, key=lambda b: iou(r, b)) if cands else None
        s_ref = float(r['score'])
        s_brd = float(near['score']) if near else float('nan')
        print(f"  {r['name']} cls{r['cls']} score={s_ref:.4f} "
              f"box=({r['x1']},{r['y1']},{r['x2']},{r['y2']}) "
              f"INT8: score={s_brd if cands else '—'} "
              f"iou={iou(r, near) if near else 0:.2f}")
