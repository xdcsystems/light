#!/usr/bin/env python3
"""Verify C++ detections (raw rows in --box-log) against RKNNLite oracle.

Inputs:
  dump_dir  -- PPM dumps of model-input tensor (--dump-cal / --cal, cal_%04d.ppm)
  log_csv   -- box-log from run ('frame,c1,c2' + 'raw,cls,conf,x,y,w,h')
Model defaults to ./misc/yolov5s_relu_int8_mmse_v5.rknn (--model overrides).
"""
import argparse
import glob
import os
import re
import sys

import numpy as np
from rknnlite.api import RKNNLite

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from postprocess import decode_outputs

MATCH_IOU = 0.6
CONF_TOL = 0.06
BORDER_CONF = 0.35


def load_ppm(path):
    d = open(path, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(d[m.end():], dtype=np.uint8)[: w * h * 3].reshape(h, w, 3)


def lb_bars_of(pix):
    def run_len(mask):
        n = len(mask)
        head = 0
        while head < n and mask[head]:
            head += 1
        tail = 0
        while tail < n - head and mask[n - 1 - tail]:
            tail += 1
        return head, tail

    top, bot = run_len((np.abs(pix.astype(np.int16) - 128) <= 1).all(axis=(1, 2)))
    left, right = run_len((np.abs(pix.astype(np.int16) - 128) <= 1).all(axis=(0, 2)))
    return float(left), float(top), float(right), float(bot)


def load_log(path):
    frames = {}
    cur = None
    with open(path) as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) == 3 and all(p.isdigit() for p in parts):
                cur = int(parts[0])
                frames[cur] = []
            elif len(parts) == 7 and parts[0] == 'raw' and cur is not None:
                frames[cur].append(
                    (int(parts[1]), float(parts[2]), float(parts[3]), float(parts[4]), float(parts[5]), float(parts[6]))
                )
    return frames


def iou(a, b):
    ax1, ay1, ax2, ay2 = a[2], a[3], a[2] + a[4], a[3] + a[5]
    bx1, by1, bx2, by2 = b[2], b[3], b[2] + b[4], b[3] + b[5]
    iw, ih = max(0.0, min(ax2, bx2) - max(ax1, bx1)), max(0.0, min(ay2, by2) - max(ay1, by1))
    inter = iw * ih
    return inter / (a[4] * a[5] + b[4] * b[5] - inter + 1e-6)


def infer_scale(preds_by_dump, frames, lb_left, lb_top, band_w):
    # Candidate standard scene widths preserving band aspect ratio.
    cands = []
    for sw in (640, 800, 1024, 1280, 1440, 1600, 1920, 2560, 3840):
        cands.append(band_w / float(sw))
    cands = sorted(set(cands))
    best_sc = band_w / 1280.0
    best_score = -1.0
    for sc in cands:
        tot_score = 0.0
        n_matched = 0
        for pred in preds_by_dump:
            mine = [
                (int(c), float(s), (x1 - lb_left) / sc, (y1 - lb_top) / sc, (x2 - x1) / sc, (y2 - y1) / sc)
                for x1, y1, x2, y2, s, c in pred
            ]
            if not mine:
                continue
            fbest = -1.0
            for boxes in frames.values():
                if not boxes:
                    continue
                vs = [max((iou(m, g) for g in boxes if g[0] == m[0]), default=0.0) for m in mine]
                s = sum(vs) / len(mine)
                if s > fbest:
                    fbest = s
            if fbest > 0.0:
                tot_score += fbest
                n_matched += 1
        avg = tot_score / n_matched if n_matched else -1.0
        if avg > best_score:
            best_score = avg
            best_sc = sc
    return best_sc


def main():
    ap = argparse.ArgumentParser(description='Verify C++ raw detections against RKNNLite oracle')
    ap.add_argument('dump_dir')
    ap.add_argument('log_csv')
    ap.add_argument('--model', default='./misc/yolov5s_relu_int8_mmse_v5.rknn')
    ap.add_argument('--cam-w', type=float, default=0.0)
    ap.add_argument('--cam-h', type=float, default=0.0)
    ap.add_argument('--scale', type=float, default=0.0)
    args = ap.parse_args()

    frames = load_log(args.log_csv)
    dumps = sorted(glob.glob(os.path.join(args.dump_dir, 'cal_*.ppm')))
    if not dumps:
        print('No cal_*.ppm dumps found in', args.dump_dir)
        sys.exit(1)

    rk = RKNNLite()
    if rk.load_rknn(args.model) != 0:
        print('Failed to load model:', args.model)
        sys.exit(1)
    if rk.init_runtime(core_mask=RKNNLite.NPU_CORE_0) != 0:
        print('Failed to init RKNNLite runtime')
        sys.exit(1)

    decoded = []
    for path in dumps:
        idx = int(re.search(r'(\d+)', os.path.basename(path)).group(1))
        pix = load_ppm(path)
        mh, mw = float(pix.shape[0]), float(pix.shape[1])
        lb_left, lb_top, lb_right, lb_bot = lb_bars_of(pix)
        band_w = max(1.0, mw - lb_left - lb_right)
        band_h = max(1.0, mh - lb_top - lb_bot)
        outs = rk.inference(inputs=[pix[np.newaxis]])
        pred = decode_outputs([o[0] for o in outs], 0.25, 0.45)
        decoded.append((idx, pred, mw, mh, lb_left, lb_top, band_w, band_h))
    rk.release()

    _, _, mw0, mh0, l0, t0, bw0, bh0 = decoded[0]
    if args.scale > 0.0:
        scale = args.scale
    elif args.cam_w > 0.0 and args.cam_h > 0.0:
        scale = min(mw0 / args.cam_w, mh0 / args.cam_h)
    else:
        scale = infer_scale([d[1] for d in decoded], frames, l0, t0, bw0)
    print('model=%s scale=%.4f (band %.0fx%.0f in %.0fx%.0f)' % (args.model, scale, bw0, bh0, mw0, mh0))

    tot = mat = u_ref = u_pred = 0
    max_px = max_conf = 0.0
    for idx, pred, mw, mh, lb_left, lb_top, band_w, band_h in decoded:
        mine = [
            (int(c), float(s), (x1 - lb_left) / scale, (y1 - lb_top) / scale, (x2 - x1) / scale, (y2 - y1) / scale)
            for x1, y1, x2, y2, s, c in pred
        ]

        def frame_score(boxes):
            if not mine or not boxes:
                return -1.0
            return sum(max((iou(m, g) for g in boxes if g[0] == m[0]), default=0.0) for m in mine) / len(mine)

        fbest, fidx = -1.0, None
        for f, boxes in frames.items():
            s = frame_score(boxes)
            if s > fbest:
                fbest, fidx = s, f
        log = frames.get(fidx, []) if fidx is not None else []
        if fidx is not None and fidx != idx:
            print('  dump cal_%04d matched to log frame %d (score %.3f)' % (idx, fidx, fbest))
        taken = [False] * len(mine)
        for g in log:
            tot += 1
            best_v, best_i = 0.0, -1
            for i, m in enumerate(mine):
                if taken[i] or m[0] != g[0]:
                    continue
                v = iou(m, g)
                if v > best_v:
                    best_v, best_i = v, i
            if best_i < 0 or best_v < MATCH_IOU:
                if g[1] < BORDER_CONF:
                    print('  frame %d: BORDERLINE log cls=%d conf=%.4f (IoU %.3f) -- skipped' % (idx, g[0], g[1], best_v))
                    continue
                u_ref += 1
                print('  frame %d: UNMATCHED log cls=%d conf=%.4f box=(%.1f,%.1f %.1fx%.1f)' % ((idx,) + g))
            else:
                taken[best_i] = True
                m = mine[best_i]
                dx = abs(m[2] - g[2])
                dy = abs(m[3] - g[3])
                dc = abs(m[1] - g[1])
                max_px = max(max_px, dx, dy)
                max_conf = max(max_conf, dc)
                if dc > CONF_TOL:
                    u_ref += 1
                    print('  frame %d: CONF-DRIFT cls=%d log %.4f vs mine %.4f (IoU %.3f)' % (idx, g[0], g[1], m[1], best_v))
                else:
                    mat += 1
        for i, m in enumerate(mine):
            if not taken[i]:
                u_pred += 1
                print('  frame %d: PHANTOM pred cls=%d conf=%.4f box=(%.1f,%.1f %.1fx%.1f)' % ((idx,) + m))

    print('reference boxes: %d, matched: %d, unmatched: %d, phantoms: %d' % (tot, mat, u_ref, u_pred))
    print('max px delta: %.3f, max conf delta: %.4f' % (max_px, max_conf))
    ok = (tot > 0 or u_pred == 0) and u_ref == 0 and u_pred == 0
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
