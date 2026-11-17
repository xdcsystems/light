#!/usr/bin/env python3
"""Probe yolov5s_relu output semantics on a real model-input frame.

usage: probe_yolo.py [PPM]   (default: build/cal.ppm of the project, the --dump-cal output)
"""
import os
import sys
import numpy as np

from rknnlite.api import RKNNLite

PROJ = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
ppm = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJ, 'build', 'cal.ppm')
with open(ppm, 'rb') as f:
    d = f.read()
i = d.index(b'255\n') + 4
w, h = 640, 640
img = np.frombuffer(d[i:], np.uint8).reshape(h, w, 3)

rk = RKNNLite()
assert rk.load_rknn(os.path.join(PROJ, 'misc', 'yolov5s_relu.rknn')) == 0
assert rk.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0
outs = rk.inference(inputs=[img[None]], data_format='nhwc')

# layout NCHW: (1, 255, H, W); 255 = 3 anchors * 85 (4 box + 1 obj + 80 cls)
for gi, o in enumerate(outs):
    a = o[0]  # (255, H, W)
    _, H, W = a.shape
    print(f'== scale {gi}: grid {H}x{W}')
    for label, ch in [('box', a[0:4]), ('obj', a[4:5]), ('cls', a[5:85])]:
        print(f'   {label}: min={ch.min():.4f} max={ch.max():.4f} '
              f'frac>0.5={float((ch > 0.5).mean()):.5f}')
    obj = a[4]
    hot = int((obj > 0.5).sum())
    print(f'   cells with obj>0.5: {hot} of {H*W*3}')

rk.release()
