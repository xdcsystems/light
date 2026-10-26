# -*- coding: utf-8 -*-
import glob
import numpy as np
from PIL import Image

BAND_TOP, BAND_BOT = 140, 500   # lb_top=140, lb_h=360 из ридми

stats = []
for path in sorted(glob.glob('dataset_night/cal_*.ppm')):
    a = np.asarray(Image.open(path).convert('L'), dtype=np.float32)[BAND_TOP:BAND_BOT]
    g = a[::2, ::2]
    stats.append((path.split('/')[-1], g.mean(),
                  np.percentile(g, 1), np.percentile(g, 50), np.percentile(g, 99)))

stats.sort(key=lambda s: s[1])
print('имя         средняя  p1   p50  p99   (видимая полоса, 0..255)')
for name, mean, p1, p50, p99 in stats:
    print(f'{name}  {mean:6.1f}  {p1:4.0f}  {p50:4.0f}  {p99:4.0f}')

m = np.array([s[1] for s in stats])
print()
print(f'средняя по полосе: {m.mean():.1f}, min {m.min():.1f}, max {m.max():.1f}')
print('самые тёмные 10:', [s[0] for s in stats[:10]])
print('самые светлые 5:', [s[0] for s in stats[-5:]])
