# -*- coding: utf-8 -*-
import numpy as np
from PIL import Image

for name in ('cal_0067', 'cal_0068', 'cal_0037'):
    img = np.asarray(Image.open(f'dataset_night/{name}.ppm').convert('RGB'))
    lum = img[140:500].mean(axis=2).ravel()
    tot = lum.size
    b = [(lum == 0).mean(), ((lum > 0) & (lum < 10)).mean(),
         ((lum >= 10) & (lum < 50)).mean(), ((lum >= 50) & (lum < 100)).mean(),
         ((lum >= 100) & (lum < 200)).mean(), (lum >= 200).mean()]
    print(f"{name}: 0: {b[0]*100:4.1f}%  1-9: {b[1]*100:4.1f}%  10-49: {b[2]*100:4.1f}%  "
          f"50-99: {b[3]*100:4.1f}%  100-199: {b[4]*100:4.1f}%  >=200: {b[5]*100:4.1f}%")
    # bounding box засвета (>=200): компактная лужа или размазана по кадру
    ys, xs = np.nonzero(lum.reshape(360, 640) >= 200)
    if ys.size:
        print(f"   >=200: {ys.size} пикс., x=[{xs.min()},{xs.max()}] y=[{ys.min()},{ys.max()}]")
    else:
        print('   >=200: нет')
