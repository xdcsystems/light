# -*- coding: utf-8 -*-
import glob
import numpy as np
from PIL import Image

stats = []
for path in sorted(glob.glob('dataset_night/cal_*.ppm')):
    # читаем с прореживанием: каждая 4-я строка/столбец, точности хватает
    a = np.asarray(Image.open(path).convert('L'), dtype=np.float32)
    g = a[::4, ::4]
    stats.append((path.split('/')[-1], g.mean(),
                  np.percentile(g, 1), np.percentile(g, 50), np.percentile(g, 99)))

stats.sort(key=lambda s: s[1])
print('имя         средняя  p1   p50  p99   (яркость 0..255)')
for name, mean, p1, p50, p99 in stats:
    print(f'{name}  {mean:6.1f}  {p1:4.0f}  {p50:4.0f}  {p99:4.0f}')

m = np.array([s[1] for s in stats])
print()
print(f'средняя по набору: {m.mean():.1f}, min {m.min():.1f}, max {m.max():.1f}')
print('самые тёмные 10:', [s[0] for s in stats[:10]])
print('самые светлые 5:', [s[0] for s in stats[-5:]])
# для сравнения: с каким средним значением модель уже справлялась на калибровке
