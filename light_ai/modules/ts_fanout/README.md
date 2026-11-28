# `ts_fanout` — On-Demand MPEG-TS TCP Fan-Out Relay

Lightweight single-file C++17 `epoll` relay that fans out a single upstream MPEG-TS TCP pull stream (such as `light_ai` on `127.0.0.1:1234`) to multiple concurrent viewers on `0.0.0.0:1236` with `< 1–2 ms` added latency, keeping the `light_ai` single-viewer core and RT cores untouched.

## Architecture & Guarantees

- **On-demand upstream:** While `0` clients are connected, the upstream socket stays closed so `light_ai` keeps its H.264 encoder and GPU overlay asleep. When the first client connects, `ts_fanout` opens a non-blocking TCP connection to upstream (retrying every `250 ms` if unavailable). After the last client disconnects, upstream remains open for `--idle-grace-ms` (`1500 ms` by default) to absorb quick reconnects before closing.
- **Sub-millisecond / `< 1–2 ms` relay latency:** Sets `TCP_NODELAY=1`, `SO_SNDBUF=65536`, `TCP_NOTSENT_LOWAT=16384`, and `SOCK_NONBLOCK` on upstream and client sockets; forwards raw chunks up to `16 KiB` immediately with zero demuxing or re-encoding.
- **188-byte MPEG-TS packet alignment:** New clients joining mid-stream start receiving from the first `0x47` sync byte verified at `buf[i] == 0x47 && buf[i + 188] == 0x47`, avoiding player resynchronization delays.
- **Zero HoL-blocking:** Each client has a dedicated `256 KiB` ring buffer. Slow consumers that overflow are dropped individually without stalling upstream reads or other viewers.
- **`accept4` errors:** `ECONNABORTED` / `EPROTO` are skipped; on `EMFILE` / `ENFILE` / `ENOBUFS` / `ENOMEM` the listener leaves `epoll` (no busy-spin) and returns after `100 ms` or as soon as a client is dropped.
- **CPU isolation:** Intended to run on efficiency cores (`AllowedCPUs=0-3`, `Nice=5`) with no `mlockall` or `SCHED_FIFO`.

## Build & Install

```bash
cd light_ai/modules/ts_fanout
make
sudo make install   # installs /usr/local/bin/light-ai-fanout
```

## CLI Options

| Flag | Default | Description |
|---|---|---|
| `--upstream <ip:port>` | `127.0.0.1:1234` | Upstream MPEG-TS TCP pull endpoint |
| `--listen <ip:port>` | `0.0.0.0:1236` | Listen address for viewer connections |
| `--max-clients <N>` | `8` | Maximum concurrent clients |
| `--idle-grace-ms <ms>` | `1500` | Grace period before closing idle upstream |
| `-h`, `--help` | — | Print usage |

## Systemd Setup

```bash
sudo cp light-ai-fanout.service.example /etc/systemd/system/light-ai-fanout.service
sudo systemctl daemon-reload
sudo systemctl enable --now light-ai-fanout.service
sudo systemctl status light-ai-fanout.service
```

Connect viewers to `tcp://<board-ip>:1236`:

```bash
ffplay -fflags nobuffer tcp://<board-ip>:1236
```
