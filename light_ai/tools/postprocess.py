import numpy as np

# Якоря YOLOv5s по умолчанию, в порядке масштабов 80/40/20
ANCHORS = np.array([
    [[10, 13], [16, 30], [33, 23]],          # 80x80, stride 8
    [[30, 61], [62, 45], [59, 119]],         # 40x40, stride 16
    [[116, 90], [156, 198], [373, 326]],     # 20x20, stride 32
], dtype=np.float32)
STRIDES = np.array([8.0, 16.0, 32.0], dtype=np.float32)
NUM_CLASSES = 80   # COCO; поправьте, если свой набор

def decode_outputs(outputs, conf_thres=0.25, iou_thres=0.45):
    """outputs: список из 3 карт (255,H,W) без батча.
       Возвращает (N,6): x1,y1,x2,y2,score,cls"""
    all_pred = []
    for out, anchors, stride in zip(outputs, ANCHORS, STRIDES):
        _, h, w = out.shape
        out = out.reshape(3, 5 + NUM_CLASSES, h, w)   # 3 якоря × 85
        gy, gx = np.meshgrid(np.arange(h), np.arange(w), indexing='ij')
        grid = np.stack([gx, gy], axis=-1).astype(np.float32)  # (h,w,2)
        grid = grid[:, :, None, :]                    # (h,w,1,2) — для якорей

        # выходы уже после sigmoid: 0..1
        # xy = (t*2 - 0.5 + grid) * stride ; wh = (t*2)^2 * anchor
        # из (3,2,h,w) -> (h,w,3,2)
        xy = (np.transpose(out[:, 0:2], (2, 3, 0, 1)) * 2 - 0.5 + grid) * stride
        wh = (np.transpose(out[:, 2:4], (2, 3, 0, 1)) * 2) ** 2 * anchors[None, None]

        obj = np.transpose(out[:, 4], (1, 2, 0))          # (3,h,w) -> (h,w,3)
        cls = np.transpose(out[:, 5:], (2, 3, 0, 1))      # (3,nc,h,w) -> (h,w,3,nc)
        conf = obj * cls.max(axis=-1)                     # (h,w,3)

        m = conf > conf_thres
        if not m.any():
            continue
        yy, xx, aa = np.where(m)
        cxy = xy[yy, xx, aa]
        cwh = wh[yy, xx, aa]
        score = conf[yy, xx, aa]
        label = cls.argmax(axis=-1)[yy, xx, aa]

        x1 = cxy[:, 0] - cwh[:, 0] / 2
        y1 = cxy[:, 1] - cwh[:, 1] / 2
        x2 = cxy[:, 0] + cwh[:, 0] / 2
        y2 = cxy[:, 1] + cwh[:, 1] / 2
        all_pred.append(np.stack([x1, y1, x2, y2, score, label], axis=1))

    if not all_pred:
        return np.empty((0, 6))
    pred = np.concatenate(all_pred, axis=0)
    return nms(pred, iou_thres)

def nms(pred, iou_thres):
    keep = []
    order = pred[:, 4].argsort()[::-1]
    while order.size:
        i = order[0]
        keep.append(i)
        if order.size == 1:
            break
        rest = order[1:]
        xx1 = np.maximum(pred[i, 0], pred[rest, 0])
        yy1 = np.maximum(pred[i, 1], pred[rest, 1])
        xx2 = np.minimum(pred[i, 2], pred[rest, 2])
        yy2 = np.minimum(pred[i, 3], pred[rest, 3])
        inter = np.maximum(0, xx2 - xx1) * np.maximum(0, yy2 - yy1)
        area_i = (pred[i, 2] - pred[i, 0]) * (pred[i, 3] - pred[i, 1])
        area_r = (pred[rest, 2] - pred[rest, 0]) * (pred[rest, 3] - pred[rest, 1])
        iou = inter / (area_i + area_r - inter + 1e-9)
        order = rest[iou < iou_thres]
    return pred[keep]
