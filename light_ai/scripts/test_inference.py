#!/usr/bin/env python3
"""First inference of yolov5s_relu.rknn on librknnrt 2.5.0 via RKNNLite."""
import os
import sys

import numpy as np
from rknnlite.api import RKNNLite

MODEL = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'misc', 'yolov5s_relu.rknn'))
EXPECTED_SHAPES = {(1, 255, 80, 80), (1, 255, 40, 40), (1, 255, 20, 20)}


def main() -> int:
    if not os.path.isfile(MODEL):
        print(f'model not found: {MODEL}', file=sys.stderr)
        return 1

    rknn = RKNNLite()
    if rknn.load_rknn(MODEL) != 0:
        print('load_rknn failed', file=sys.stderr)
        return 1
    if rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_0) != 0:
        print('init_runtime failed', file=sys.stderr)
        return 1

    rng = np.random.RandomState(42)
    x = rng.randint(0, 256, size=(1, 640, 640, 3), dtype=np.uint8)

    outs = rknn.inference(inputs=[x], data_format='nhwc')
    if outs is None:
        print('inference returned None', file=sys.stderr)
        return 1

    ok = True
    shapes = set()
    for i, o in enumerate(outs):
        omin = float(o.min())
        omax = float(o.max())
        relu_ok = omin >= 0.0
        alive = omax > 0.0
        shapes.add(tuple(o.shape))
        print(f'out[{i}]: shape={tuple(o.shape)} min={omin:.6f} max={omax:.6f} '
              f'relu_ok={relu_ok} alive={alive}')
        ok = ok and relu_ok and alive

    if shapes != EXPECTED_SHAPES:
        print(f'shape mismatch: got {sorted(shapes)}, expected {sorted(EXPECTED_SHAPES)}',
              file=sys.stderr)
        ok = False

    rknn.release()
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
