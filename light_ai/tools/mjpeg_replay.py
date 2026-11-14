#!/usr/bin/env python3
"""mjpeg_replay.py -- serve a recorded MJPEG clip ONCE, paced, to one HTTP client.

The layer-A replay of the reference detections (reference/README.md, issue #32):
light_ai reads the clip through its net source (--net-src http://127.0.0.1:PORT/),
and every clip frame must be decoded and inferred exactly once, in order.
The net source drops frames in three places; the server avoids each one:
  - start() drain: runs inside MediaSource::open right after the prime of
    part 0 in setup_device(), so it finds nothing to drain whatever the pacing;
  - latest-frame skip (read_latest_part): only when a SECOND complete part is
    already buffered at a dequeue. The pacing (--fps) stays well below the
    pipeline's throughput; the hold (--hold) between part 0 and part 1 covers
    the startup between the prime and the second dequeue (decoder init, the
    rest of the Pipeline ctor, run(), frame 0) -- measured with strace on the
    board: ~5 ms at --pool 3, ~31 ms at --pool 1 (reference/README.md); the
    default 0.5 s keeps a wide margin and stays far below the 2 s watchdog
    (NetMjpegSource kPollMs, SourceSet kStallMs);
  - a corrupt frame: every frame is checked here (SOI, EOI, SOF size) first.
Pacing is not trusted: tools/ref_replay.sh proves every run drop-free.
After the last part the connection stays open (no EOF, so no reconnect path)
until the client closes it. One connection only: the listening socket closes
after accept(), so a reconnect is refused and shows up as a failed run.

Clip: concatenated JPEGs as captured (ffmpeg -c copy -f mjpeg). Frames are split
by walking the JPEG segments up to SOS, then the EOI after the entropy data
(an FF D9 cannot occur inside it: FF there is always followed by 00 or RST).

Usage:
  mjpeg_replay.py CLIP --count                 # check every frame, print the count
  mjpeg_replay.py CLIP [--port 8090] [--fps 10] [--hold 0.5] [--width 1280 --height 960]
Prints "listening <port> frames <n>" when ready and "sent <n>" after the last
part (stderr, flushed). Exit 0 when every frame was sent.
"""
import argparse
import socket
import sys
import time

SOF_MARKERS = {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}


def split_frames(data, want_w, want_h):
    """Returns [(start, end)] of every JPEG in data; raises ValueError on a bad frame."""
    frames = []
    pos = data.find(b'\xff\xd8')
    while pos >= 0:
        i = pos + 2
        size = None
        while True:  # marker segments up to SOS
            if i + 4 > len(data) or data[i] != 0xFF:
                raise ValueError('frame %d at %d: broken marker segment' % (len(frames), pos))
            m = data[i + 1]
            if m == 0xFF:  # fill byte
                i += 1
                continue
            seg = int.from_bytes(data[i + 2:i + 4], 'big')
            if m in SOF_MARKERS:
                h = int.from_bytes(data[i + 5:i + 7], 'big')
                w = int.from_bytes(data[i + 7:i + 9], 'big')
                size = (w, h)
            i += 2 + seg
            if m == 0xDA:
                break
        eoi = data.find(b'\xff\xd9', i)
        if eoi < 0:
            raise ValueError('frame %d at %d: no EOI' % (len(frames), pos))
        if size != (want_w, want_h):
            raise ValueError('frame %d at %d: SOF size %s, want %dx%d' % (len(frames), pos, size, want_w, want_h))
        frames.append((pos, eoi + 2))
        pos = data.find(b'\xff\xd8', eoi + 2)
    return frames


def log(msg):
    sys.stderr.write(msg + '\n')
    sys.stderr.flush()


def serve(data, frames, port, fps, hold):
    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(('127.0.0.1', port))
    lsock.listen(1)
    log('listening %d frames %d' % (port, len(frames)))
    conn, _ = lsock.accept()
    lsock.close()  # one connection only: a reconnect is refused
    req = b''
    while b'\r\n\r\n' not in req and b'\n\n' not in req:
        chunk = conn.recv(4096)
        if not chunk:
            log('client closed before the request head')
            return 1
        req += chunk
    conn.sendall(b'HTTP/1.0 200 OK\r\n'
                 b'Content-Type: multipart/x-mixed-replace;boundary=frame\r\n\r\n')
    t0 = None
    for k, (s, e) in enumerate(frames):
        if k == 1:
            t0 = time.monotonic() + hold  # part 0 primed; part 1 after the hold
        if t0 is not None:
            delay = t0 + (k - 1) / fps - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        head = b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n' % (e - s)
        try:
            conn.sendall(head + data[s:e] + b'\r\n')
        except OSError as ex:
            log('client gone at part %d: %s' % (k, ex))
            return 1
    log('sent %d' % len(frames))
    try:
        while conn.recv(4096):  # hold the connection until the client closes it
            pass
    except OSError:
        pass
    conn.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('clip')
    ap.add_argument('--count', action='store_true', help='check every frame, print the count, exit')
    ap.add_argument('--port', type=int, default=8090)
    ap.add_argument('--fps', type=float, default=10.0)
    ap.add_argument('--hold', type=float, default=0.5, help='seconds between part 0 and part 1')
    ap.add_argument('--width', type=int, default=1280)
    ap.add_argument('--height', type=int, default=960)
    a = ap.parse_args()
    with open(a.clip, 'rb') as f:
        data = f.read()
    try:
        frames = split_frames(data, a.width, a.height)
    except ValueError as ex:
        log('bad clip: %s' % ex)
        return 2
    if a.count:
        print(len(frames))
        return 0
    return serve(data, frames, a.port, a.fps, a.hold)


if __name__ == '__main__':
    sys.exit(main())
