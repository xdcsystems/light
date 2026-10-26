# -*- coding: utf-8 -*-
import sys
import numpy as np
from PIL import Image
from rknnlite.api import RKNNLite

MODEL = sys.argv[1] if len(sys.argv) > 1 else './yolov5s_relu_int8_v2.rknn'
FRAMES = sys.argv[2:] or ['dataset_night/cal_0067.ppm', 'dataset_night/cal_0068.ppm',
                          'dataset_night/cal_0065.ppm', 'dataset_night/cal_0037.ppm',
                          'dataset_night/cal_0149.ppm', 'dataset_night/cal_0096.ppm',
                          'dataset_night/cal_0152.ppm', 'dataset_night/cal_0163.ppm']

HEADS = [(80, 8), (40, 16), (20, 32)]

rknn = RKNNLite()
rknn.load_rknn(MODEL)
rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_0_1_2)

for path in FRAMES:
    img = np.asarray(Image.open(path).convert('RGB'), dtype=np.uint8)
    outs = rknn.inference(inputs=[img[None]])
    parts = []
    for (grid, stride), out in zip(HEADS, outs):
        a = out.reshape(3, 85, grid, grid)
        obj = a[:, 4]                      # уже пост-сигмоида
        hot10 = int((obj > 0.10).sum())
        hot50 = int((obj > 0.50).sum())
        parts.append(f'{grid}x{grid}: max_obj={obj.max():.3f} '
                     f'obj>0.1: {hot10:3d}  obj>0.5: {hot50:2d}')
    print(path.split('/')[-1], ' | '.join(parts))
