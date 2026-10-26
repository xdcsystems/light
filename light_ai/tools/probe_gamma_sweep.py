# -*- coding: utf-8 -*-
import numpy as np
from PIL import Image
from rknnlite.api import RKNNLite

FRAMES = ['dataset_night/cal_0067.ppm', 'dataset_night/cal_0068.ppm',
          'dataset_night/cal_0096.ppm', 'dataset_night/cal_0152.ppm',
          'dataset_night/cal_0037.ppm',   # живой контроль
          'dataset_night/cal_0149.ppm']   # пограничный

# файлы уже с гаммой 2.0 из пайплайна; здесь — ДОПОЛНИТЕЛЬНЫЕ преобразования
def gam(x, g):  return (x / 255.0) ** g * 255.0
def gain(x, k): return np.minimum(255.0, x * k)
TF = [
    ('orig ',      lambda x: x),
    ('g+2 ',       lambda x: gam(x, 2.0)),
    ('g+3 ',       lambda x: gam(x, 3.0)),
    ('x2  ',       lambda x: gain(x, 2.0)),
    ('x4  ',       lambda x: gain(x, 4.0)),
    ('x2g2',       lambda x: gam(gain(x, 2.0), 2.0)),
    ('x4g2',       lambda x: gam(gain(x, 4.0), 2.0)),
]

rknn = RKNNLite()
rknn.load_rknn('./yolov5s_relu_int8_v2.rknn')
rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_0_1_2)

for path in FRAMES:
    img = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    print(path.split('/')[-1])
    for name, tf in TF:
        x = tf(img).astype(np.uint8)
        outs = rknn.inference(inputs=[x[None]])
        m = max(out.reshape(3, 85, g, g)[:, 4].max()
                for out, g in zip(outs, (80, 40, 20)))
        print(f'  {name}: max_obj={m:.3f}')
