# -*- coding: utf-8 -*-
import numpy as np
from PIL import Image
from rknnlite.api import RKNNLite

BAND_TOP, BAND_BOT = 140, 500
TARGET = 55.0        # целевая медиана полосы (после базовой гаммы)
GAIN_CAP = 6.0       # потолок адаптивного усиления

FRAMES = ['dataset_night/cal_0067.ppm', 'dataset_night/cal_0068.ppm',
          'dataset_night/cal_0096.ppm', 'dataset_night/cal_0152.ppm',
          'dataset_night/cal_0037.ppm', 'dataset_night/cal_0149.ppm',
          'dataset_night/cal_0030.ppm',  # средний тёмный
          'dataset_night/cal_0163.ppm']  # светлый контроль

rknn = RKNNLite()
rknn.load_rknn('./yolov5s_relu_int8_v2.rknn')
rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_0_1_2)

def report(tag, x):
    outs = rknn.inference(inputs=[x[None].astype(np.uint8)])
    line = []
    tot = 0
    for out, grid in zip(outs, (80, 40, 20)):
        a = out.reshape(3, 85, grid, grid)
        obj = a[:, 4]
        hot = int((obj > 0.1).sum())
        if hot:
            ai, y, xi = np.unravel_index(obj.argmax(), obj.shape)
            cls = a[ai, 5:, y, xi].argmax()
            line.append(f'{grid}: max={obj[ai,y,xi]:.2f} hot={hot} cls={cls}')
            tot += hot
        else:
            line.append(f'{grid}: max={obj.max():.2f} hot=0')
    print(f'  {tag}: ' + ' | '.join(line) + f'  [sum hot={tot}]')

for path in FRAMES:
    img = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    band = img[BAND_TOP:BAND_BOT]
    med = np.median(band[::2, ::2, :])
    name = path.split('/')[-1]
    print(name)
    report('orig        ', img)
    k = min(GAIN_CAP, TARGET / max(med, 1.0))
    x = np.minimum(255.0, img * k)
    x = (x / 255.0) ** 2.0 * 255.0
    report(f'adaptive k={k:4.1f}', x)
    x3 = (img / 255.0) ** 3.0 * 255.0
    report('gamma3      ', x3)
