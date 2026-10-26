# -*- coding: utf-8 -*-
import numpy as np
from PIL import Image

BAND = (140, 500)
for name in ('cal_0067', 'cal_0068', 'cal_0037'):   # 0037 — живой эталон для сравнения
    img = np.asarray(Image.open(f'dataset_night/{name}.ppm').convert('RGB'))
    b = img[BAND[0]:BAND[1]].astype(np.float32)
    lum = b.mean(axis=2)
    nz = lum[lum > 0.5]
    print(f'{name}: полоса {b.shape[1]}x{b.shape[0]}')
    print(f'  пикселей >0: {nz.size / lum.size * 100:.1f}%   '
          f'max={lum.max():.0f}  p50(незул.)={np.percentile(nz, 50) if nz.size else 0:.0f}')
    # где именно живёт ненулевой сигнал: bounding box в координатах полосы
    ys, xs = np.nonzero(lum > 0.5)
    if ys.size:
        print(f'  ненулевая зона: x=[{xs.min()},{xs.max()}]  y=[{ys.min()},{ys.max()}]'
              f'  ({ys.size} пикс., каждое 4-е субсэмплировано нет)')
    Image.fromarray(img).save(f'/tmp/{name}.png')
    print(f'  сохранено: /tmp/{name}.png')
