#!/usr/bin/env python3
"""compare_night.py — A/B comparison of light_ai logs (M2.4 night re-check).

Usage:
    python3 scripts/compare_night.py /tmp/night_int8.log /tmp/night_fp16.log

Parses per-frame detection lines ("frame N: K det(s)" + "cls=... conf=... box=..."),
[timing] lines, the final "Processed ..." line and counts mpp_buffer asserts.
Quality: detections matched by IoU (default 0.3); reports phantoms (int8-only),
misses (fp16-only), class jumps, dconf stats (alarm > 0.07), box center shift.
Timing: steady-state means with the first [timing] block treated as warmup.
"""
import re, os, sys

RE_FRAME  = re.compile(r'^frame (\d+): (\d+) det\(s\)')
RE_DET    = re.compile(r'^\s+cls=(.+?) conf=([\d.]+) box=\(([-\d.]+),([-\d.]+)\s+([-\d.]+)x([-\d.]+)\)')
RE_TIMING = re.compile(r'^\[timing\]\s+\S+\s+cam=([\d.]+)\s+dec=([\d.]+)\s+rga=([\d.]+)\s+infer=([\d.]+)\s+\|\s+sum=([\d.]+) ms -> ~([\d.]+) FPS')
RE_FINAL  = re.compile(r'^Processed (\d+) frames in ([\d.]+) s -> ([\d.]+) FPS')

def parse_log(path):
    frames, timings, mpp_asserts, final = {}, [], 0, None
    cur = None
    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            m = RE_FRAME.match(line)
            if m:
                cur = int(m.group(1)); frames[cur] = []; continue
            m = RE_DET.match(line)
            if m and cur is not None:
                frames[cur].append(dict(cls=m.group(1).strip(), conf=float(m.group(2)),
                                        x=float(m.group(3)), y=float(m.group(4)),
                                        w=float(m.group(5)), h=float(m.group(6)))); continue
            m = RE_TIMING.match(line)
            if m:
                timings.append(dict(cam=float(m.group(1)), dec=float(m.group(2)),
                                    rga=float(m.group(3)), infer=float(m.group(4)),
                                    sum=float(m.group(5)), fps=float(m.group(6)))); continue
            if 'mpp_buffer: Assertion caller failed' in line:
                mpp_asserts += 1
            m = RE_FINAL.match(line)
            if m:
                final = dict(frames=int(m.group(1)), sec=float(m.group(2)), fps=float(m.group(3)))
    return dict(frames=frames, timings=timings, mpp_asserts=mpp_asserts, final=final)

def steady(t, warmup_blocks=1):
    return t[warmup_blocks:] if len(t) > warmup_blocks else t

def mean(xs):
    return sum(xs) / len(xs) if xs else float('nan')

def iou(a, b):
    ax0, ay0, ax1, ay1 = a['x']-a['w']/2, a['y']-a['h']/2, a['x']+a['w']/2, a['y']+a['h']/2
    bx0, by0, bx1, by1 = b['x']-b['w']/2, b['y']-b['h']/2, b['x']+b['w']/2, b['y']+b['h']/2
    inter = max(0.0, min(ax1, bx1)-max(ax0, bx0)) * max(0.0, min(ay1, by1)-max(ay0, by0))
    union = a['w']*a['h'] + b['w']*b['h'] - inter
    return inter/union if union > 0 else 0.0

def compare(la, lb, name_a='A', name_b='B', iou_thr=0.3, conf_alarm=0.07):
    fa, fb = la['frames'], lb['frames']
    common = sorted(set(fa) & set(fb))
    only_a = sorted(set(fa) - set(fb)); only_b = sorted(set(fb) - set(fa))
    phantoms, misses, alarms, dconfs, shifts = [], [], [], [], []
    for f in common:
        da, db = fa[f], fb[f]
        used = set()
        for det in da:
            best = None
            for j, d2 in enumerate(db):
                if j in used: continue
                ov = iou(det, d2)
                if ov >= iou_thr and (best is None or ov > best[0]):
                    best = (ov, j, d2)
            if best is None:
                phantoms.append((f, det)); continue
            used.add(best[1]); d2 = best[2]
            if d2['cls'] != det['cls']:
                alarms.append(f'frame {f}: class jump: {name_a}={det["cls"]}({det["conf"]:.2f}) '
                              f'vs {name_b}={d2["cls"]}({d2["conf"]:.2f})')
            d = det['conf'] - d2['conf']
            dconfs.append(d)
            if abs(d) > conf_alarm:
                alarms.append(f'frame {f}: dconf {det["cls"]} = {d:+.2f} '
                              f'({name_a} {det["conf"]:.2f} / {name_b} {d2["conf"]:.2f}) > {conf_alarm}')
            shifts.append((abs(det['x']-d2['x']), abs(det['y']-d2['y'])))
        for j, det in enumerate(db):
            if j not in used: misses.append((f, det))
    ta, tb = steady(la['timings']), steady(lb['timings'])
    out = [f'=== A/B: {name_a} vs {name_b} ===',
           f'frames: A={len(fa)} B={len(fb)} common={len(common)} '
           f'(only in A: {len(only_a)}, only in B: {len(only_b)})',
           f'mpp asserts at start: A={la["mpp_asserts"]} B={lb["mpp_asserts"]}']
    if only_a: out.append(f'NOTE frames only in {name_a}: {only_a[:10]}')
    if only_b: out.append(f'NOTE frames only in {name_b}: {only_b[:10]}')
    out.append(f'-- quality (matched by IoU>={iou_thr}) --')
    out.append(f'phantoms ({name_a} only): {len(phantoms)}'
               + ('  ALARM: ' + '; '.join(f'f{f} {d["cls"]} conf={d["conf"]:.2f}' for f, d in phantoms[:5]) if phantoms else ''))
    out.append(f'misses ({name_b} only): {len(misses)}'
               + ('  ALARM: ' + '; '.join(f'f{f} {d["cls"]} conf={d["conf"]:.2f}' for f, d in misses[:5]) if misses else ''))
    out.append(f'alarms (class jumps / dconf): {len(alarms)}')
    out += ['   ' + a for a in alarms[:10]]
    if dconfs:
        out.append(f'dconf: mean={mean(dconfs):+.3f} max_abs={max(abs(d) for d in dconfs):.3f} (day reference: -0.02)')
    if shifts:
        out.append(f'box center shift: mean |dx|={mean([s[0] for s in shifts]):.1f} px, '
                   f'mean |dy|={mean([s[1] for s in shifts]):.1f} px (day ref: <=2 px)')
    out.append('-- timing (steady, warmup block excluded) --')
    if ta: out.append(f'{name_a}: infer={mean([t["infer"] for t in ta]):.1f} ms  sum={mean([t["sum"] for t in ta]):.1f} ms  '
                      f'cam={mean([t["cam"] for t in ta]):.1f} ms  ~{mean([t["fps"] for t in ta]):.1f} FPS')
    if tb: out.append(f'{name_b}: infer={mean([t["infer"] for t in tb]):.1f} ms  sum={mean([t["sum"] for t in tb]):.1f} ms  '
                      f'cam={mean([t["cam"] for t in tb]):.1f} ms  ~{mean([t["fps"] for t in tb]):.1f} FPS')
    ok = not phantoms and not misses and not alarms and (not dconfs or abs(mean(dconfs)) <= conf_alarm)
    out.append('VERDICT: MATCH — int8 night quality holds, M2.4 can be closed' if ok
               else 'VERDICT: ALARM — do NOT close M2.4 yet, inspect entries above')
    return '\n'.join(out)

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(1)
    la, lb = parse_log(sys.argv[1]), parse_log(sys.argv[2])
    print(compare(la, lb, os.path.basename(sys.argv[1]), os.path.basename(sys.argv[2])))
