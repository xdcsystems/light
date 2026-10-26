import time
import numpy as np
from PIL import Image
from rknnlite.api import RKNNLite

MODEL = 'yolov5s_relu_int8_v2.rknn'
IMG = '/home/linaro/Projects/light/light_ai/dataset_mix/n1_cal_0000.ppm'


rknn = RKNNLite()
ret = rknn.load_rknn(MODEL)
assert ret == 0, f'load_rknn вернул {ret}'

ret = rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_0)
assert ret == 0, 'init_runtime failed'

img = np.array(Image.open(IMG).convert('RGB'))
img = np.expand_dims(img, 0)   # (1, 640, 640, 3) — NHWC с батч-размерностью

outputs = rknn.inference(inputs=[img])
for i, o in enumerate(outputs):
    print(f'output{i}: shape={o.shape} dtype={o.dtype} '
          f'min={o.min():.3f} max={o.max():.3f} mean={o.mean():.4f}')

t0 = time.time()
N = 50
for _ in range(N):
    rknn.inference(inputs=[img])
dt = (time.time() - t0) / N * 1000
print(f'{dt:.1f} ms/кадр -> ~{1000/dt:.1f} FPS')
rknn.release()


from postprocess import decode_outputs
dets = decode_outputs([o[0] for o in outputs])   # снимаем батч-размерность
print(f'детекций после NMS: {len(dets)}')
for d in dets[:10]:
    x1, y1, x2, y2, score, cls = d
    print(f'  cls={int(cls):2d} score={score:.3f} box=({x1:.0f},{y1:.0f},{x2:.0f},{y2:.0f})')
