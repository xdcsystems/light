#!/usr/bin/env python3
import numpy as np
from rknnlite.api import RKNNLite

rng = np.random.RandomState(42)
NOISE = rng.randint(0, 256, size=(1, 640, 640, 3), dtype=np.uint8)


def dump(name, outs):
    print('==', name)
    for i, o in enumerate(outs):
        print(f'  out[{i}]: shape={o.shape} dtype={o.dtype} '
              f'min={o.min()} max={o.max()} mean={float(o.mean()):.4f} '
              f'nonzero={int(np.count_nonzero(o))}')


rk = RKNNLite()
assert rk.load_rknn('yolov5s_relu.rknn') == 0
assert rk.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0

for name, x in [
    ('yolo noise', NOISE),
    ('yolo zeros', np.zeros((1, 640, 640, 3), np.uint8)),
    ('yolo gray128', np.full((1, 640, 640, 3), 128, np.uint8)),
]:
    outs = rk.inference(inputs=[x], data_format='nhwc')
    if outs is None:
        print('==', name, '-> None')
    else:
        dump(name, outs)
rk.release()

rk2 = RKNNLite()
assert rk2.load_rknn('/usr/share/model/RK3588/mobilenet_v1.rknn') == 0
assert rk2.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0
o = rk2.inference(inputs=[rng.randint(0, 256, size=(1, 224, 224, 3), dtype=np.uint8)],
                  data_format='nhwc')
if o is None:
    print('== mobilenet noise 224 -> None')
else:
    dump('mobilenet noise 224', o)
rk2.release()
