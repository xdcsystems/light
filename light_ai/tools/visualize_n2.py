from PIL import Image, ImageDraw
import sys, numpy as np
sys.path.insert(0, 'tools')
from postprocess import decode_outputs

IMG = 'dataset_mix/n2_cal_0040.ppm'
img = Image.open(IMG).convert('RGB')
arr = np.expand_dims(np.array(img), 0)
from rknnlite.api import RKNNLite
rk = RKNNLite(); rk.load_rknn('yolov5s_relu_int8_v2.rknn'); rk.init_runtime()
outs = rk.inference(inputs=[arr])
dets = decode_outputs([o[0] for o in outs])
d = ImageDraw.Draw(img)
for x1, y1, x2, y2, s, c in dets:
    d.rectangle([x1, y1, x2, y2], outline='red', width=3)
    d.text((x1, max(0, y1 - 14)), f'cls {int(c)} {s:.2f}', fill='red')
img.save('pred_n2_cal_0040.jpg', quality=90)
print('сохранено: pred_n2_cal_0040.jpg, детекций:', len(dets))
