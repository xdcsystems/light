import glob, os, sys, time
import numpy as np
from PIL import Image
from rknnlite.api import RKNNLite
sys.path.insert(0, 'tools')
from postprocess import decode_outputs

IMAGES = sorted(glob.glob('dataset_mix/*.ppm'))[:100]   # первые 20 кадров
MODEL = 'yolov5s_relu_int8_v2.rknn'

rk = RKNNLite()
assert rk.load_rknn(MODEL) == 0, 'load_rknn failed'
assert rk.init_runtime() == 0, 'init_runtime failed'

n_det, n_empty, t_all = 0, 0, 0.0
for path in IMAGES:
    arr = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0
    img = np.expand_dims((arr ** 2.0 * 255).astype(np.uint8), 0)
    t0 = time.time()
    outs = rk.inference(inputs=[img])
    t_all += time.time() - t0
    dets = decode_outputs([o[0] for o in outs])
    name = os.path.basename(path)
    if len(dets) == 0:
        n_empty += 1
        print(f'{name}: нет детекций')
    else:
        n_det += len(dets)
        tops = ', '.join(f'cls{int(c)}:{s:.2f}' for _, _, _, _, s, c in dets[:3])
        print(f'{name}: {len(dets)} дет. [{tops}]')

print(f'\nитого: {n_det} детекций на {len(IMAGES)} кадрах, '
      f'{n_empty} кадров без детекций, '
      f'{t_all/len(IMAGES)*1000:.1f} мс/кадр (инференс)')
