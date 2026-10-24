#!/usr/bin/env python3
"""split_mjpeg.py <raw_file> <out_dir> — разделить склеенный MJPEG на JPEG."""
import sys, os
data = open(sys.argv[1], 'rb').read()
out = sys.argv[2]; os.makedirs(out, exist_ok=True)
i = n = 0
while True:
    s = data.find(b'\xff\xd8', i)
    if s < 0: break
    e = data.find(b'\xff\xd9', s)
    if e < 0: break
    open(os.path.join(out, f'frame_{n:03d}.jpg'), 'wb').write(data[s:e+2])
    n += 1; i = e + 2
print(n, 'frames ->', out)
