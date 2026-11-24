# Windows Viewer Notes (`mpv` on Legacy Hardware)

This document captures the verified client-side configuration for viewing the `light_ai` H.264/MPEG-TS TCP pull stream (`tcp://<board-ip>:1234`) on a legacy Windows laptop, originally recorded on 2026-10-31 and updated on 2026-11-24 for multi-view streaming (`2000 kbps / gop 50`), zero-readahead `mpv` tuning, and 2.4 GHz Wi-Fi power-saving mitigation.

For general server-side stream options (`--net`, `--net-h264`, `--net-port`, `--net-bitrate`, `--net-fps`, `--net-gop`) and `ffplay` commands, see [README.md](../README.md).

## Test Machine & Stream Profile

- **Client hardware:** Intel Core 2 Duo T9550, ATI Mobility Radeon HD 3400 (legacy driver `8.970.100.0`, 2013-04-24 — final release for this series), DirectX 10.1 (exposes D3D11 feature level `10_1`), UVD2 H.264 decode via DXVA2, OpenGL without `WGL_NV_DX_interop`.
- **Player:** Windows `mpv` (`v0.37+` / `0.41`, 32-bit or 64-bit; use `mpv.exe` for GUI/scheduled launch or `mpv.com` console host when interactive stdout/stderr logs are needed).
- **Stream:** H.264/MPEG-TS over TCP pull (`tcp://<board-ip>:1234?timeout=5000000`, confirmed by demuxer `"Found 'mpegts' at score=100"`), across canonical output canvases: `640x480` (1-source), `1280x480` (2-source side-by-side), and `1280x960` (3–4-source 2×2 grid).
- **Cadence & INI defaults:** Production defaults in `light_ai/scripts/light_ai.ini.example` set `net_fps = 0` (VFR follow-source), `net_bitrate = 2000` kbps, and `net_gop = 50`. At `~20 fps` VFR multi-view (`1280x480`), `net_bitrate = 2000` yields `~1600 kbps` elementary stream (`~1.7–2.0 Mbps` on wire), and `net_gop = 50` halves periodic IDR bursts (`~2.5 s` interval vs `1 s` auto) to keep 2.4 GHz Wi-Fi queue spikes bounded. USB camera pacing is a discrete firmware state `{20, 25, 30}` FPS — minor cadence differences reflect camera pacing, not a player defect.

## Recommended Launch (Product Path: Software Decode + `direct3d` / Low-Latency `mpv`)

No `%APPDATA%\mpv\mpv.conf` is required (and `--no-config` isolates the viewer from any existing user config); pass all flags on the command line.

**Verified low-latency one-shot (GUI / headless launch):**

```powershell
mpv.exe --no-config --no-terminal --force-window=no --idle=no --save-position-on-quit=no --no-resume-playback --no-audio --vo=direct3d --hwdec=no --cache=no --profile=low-latency --demuxer-readahead-secs=0 --demuxer-lavf-buffersize=4096 --framedrop=vo --keep-open=no "tcp://<board-ip>:1234?timeout=5000000"
```

*(Substitute `mpv.com` for `mpv.exe` when running interactively in a console window to see stdout/stderr output, or add `--log-file=C:\mpv64\mpv.log`.)*

**Auto-reconnect batch loop (`C:\mpv64\watch.bat`):**

```bat
@echo off
:loop
echo %date% %time% >> C:\mpv64\restarts.log
mpv.com --no-config --no-terminal --force-window=no --idle=no --save-position-on-quit=no --no-resume-playback --no-audio --vo=direct3d --hwdec=no --cache=no --profile=low-latency --demuxer-readahead-secs=0 --demuxer-lavf-buffersize=4096 --framedrop=vo --keep-open=no "tcp://<board-ip>:1234?timeout=5000000"
timeout /t 2 >nul
goto loop
```

**Optional alternatives:**

- **`gpu-next`/D3D11 (`mpv` 0.41+ on D3D10.1+ drivers):** Replace `--vo=direct3d` with `--vo=gpu-next` if you specifically want flip-model D3D11 presentation. `--vo=direct3d --hwdec=no` remains the verified low-overhead default across legacy Windows GPUs and 32-bit/64-bit `mpv` builds (no shader-compilation overhead or driver quirks).
- **OpenGL + `dxva2-copy` fallback (CPU unload at higher latency):**

```powershell
mpv.com --no-config --vo=gpu --gpu-api=opengl --gpu-context=win --hwdec=dxva2-copy --cache=no --profile=low-latency --demuxer-readahead-secs=0 --demuxer-lavf-buffersize=4096 --framedrop=vo --network-timeout=5 --opengl-swapinterval=0 tcp://<board-ip>:1234
```

### Option Rationale

- **`--demuxer-readahead-secs=0 --demuxer-lavf-buffersize=4096`:** Eliminates demuxer pre-buffering (`--profile=low-latency` alone can still accumulate readahead after transient Wi-Fi stalls) and caps `libavformat`'s AVIO read buffer to `4 KiB` so incoming TS packets reach the decoder immediately (`Total Cache` stays at `~8–11 KiB` / `0.0 s`).
- **`--framedrop=vo` (without `--untimed`):** Keeps `mpv` in timed PTS mode while permitting video-output frame drops to catch up cleanly after a brief network stall without building multi-second queue lag. **Never use `--untimed`**: it drains faster than real time, hits the live edge, and triggers `(Buffering)` every `2–3 s`.
- **`--no-config --save-position-on-quit=no --no-resume-playback --idle=no --keep-open=no --force-window=no --no-terminal --no-audio`:** Isolates playback from stale `%APPDATA%\mpv\mpv.conf` or `watch_later/` state, skips audio/window-idle overhead, and exits cleanly on socket drop so an external reconnect loop restarts immediately.
- **`?timeout=5000000` (or `--network-timeout=5`):** Sets a 5 s TCP socket timeout in `libavformat` (microseconds in the URL query parameter) so the player exits and reconnects cleanly when the board restarts or drops the socket.
- **`--vo=direct3d --hwdec=no`:** Deterministic software H.264 decode at `640x480` / `1280x480` (negligible CPU load) with minimal D3D9 presentation overhead and zero `dxva2-copy` GPU↔RAM bounce or `--hwdec=auto` probe delays. `d3d11va` is unsupported by the legacy HD 3400 driver and zero-copy `dxva2` is unreachable.
- **No `recv_buffer_size` trim or `opengl-swapinterval=0` on `direct3d`/`d3d11`:** Only needed for the OpenGL fallback; Nagle is already disabled (`TCP_NODELAY`) on the board side, and trimming receive buffers hurts IDR bursts.
- **Auto-reconnect loop:** `mpv`'s built-in `reconnect*` options apply only to HTTP, not raw `tcp://`. Press `q` in `mpv` to force an immediate reconnect, or close the console / `Ctrl+C` to stop.

## Receiver Wi-Fi & Power-Saving Tuning (2.4 GHz)

On 2.4 GHz (`802.11g/n`) links, default Windows power-saving settings introduce periodic `100–300+ ms` RTT spikes that trigger board-side `BasicPacketQueue` overflow flushes (`qflush`) and visible viewer stutter:

- **Disable WLAN Power Saving Mode (`Maximum Performance` / CAM = `0x0`):** Set Wireless Adapter Settings -> Power Saving Mode to `Maximum Performance` (`0x0`) on both AC and DC in the active power scheme (`powercfg` subgroup `19cbb8fa-5279-450e-9fac-8a3d5fedd0c1`, setting `12bbebe6-58d6-4636-95bb-3217ef867c1a`). This prevents 802.11 PSP sleep/beacon polling stalls.
- **Disable PCIe Link State Power Management (`ASPM = Off`, `0x0`):** Set PCI Express -> Link State Power Management to `Off` (`0x0`, subgroup `501a4d13-42af-4429-9fd1-a8218c268e20`, setting `ee12f906-d277-404b-b6da-e5fa1a576df5`) on AC and DC so the Wi-Fi NIC does not enter L0s/L1 sleep between packet bursts.
- **Disable AC display/video idle timeout:** Set `VIDEOIDLE = 0` on AC (`powercfg /SETACVALUEINDEX SCHEME_CURRENT SUB_VIDEO VIDEOIDLE 0`) for unattended monitoring.
- **Link hygiene:** Prefer wired Ethernet or 5 GHz when available; on 2.4 GHz, keep strong RSSI on a clear channel and avoid competing bulk traffic on the same link.

## Measured Latency (Timed Mode)

| Configuration | Observed Lag / Latency | Notes |
|---|---|---|
| Default `mpv` (cache enabled) | `2–3 s` | Demuxer/cache buffering |
| Legacy `--profile=low-latency --cache=no` (2026-11-01) | `~1 s` | Stopwatch estimate; board GOP 25 |
| `+ --hwdec=dxva2-copy` | Visibly worse | Two frame copies (`GPU -> RAM -> GPU`) |
| Board pipeline loopback (`640x480` 1-source, `2000 kbps / gop 50`) | `111.7 ms` median (`p90 112.0 ms`, `110.7–139.7 ms`) | Raw tagged MJPEG -> H.264/MPEG-TS TCP pull (`0` `qflush`/`clamp`) |
| Board pipeline loopback (`1280x480` 2-source, `2000 kbps / gop 50`) | `125.8 ms` median (`p90 127.7 ms`, `124.2–149.0 ms`) | Raw tagged MJPEG -> H.264/MPEG-TS TCP pull (`0` `qflush`/`clamp`) |
| Live 2.4 GHz soak with recommended `mpv` flags (`1280x480`, `2000 kbps / gop 50`) | `0` `(Buffering)`, `0` drops, `~8–11 KiB` (`0.0 s`) cache | Verified across `3h 48m` and continuous `16m+` runs |

Cross-check player:

```bash
ffplay -fflags nobuffer -rtbufsize 100k tcp://<board-ip>:1234
```

## Established A/B Facts

1. **`gpu-next` works on HD 3400 (`mpv` 0.41):** Initializes `d3d11` at feature level `10_1` (`1002:95c4`) with flip-model presentation and SPIR-V via `shaderc`.
2. **Zero-copy decode is unreachable:** Both `win/angle/dxinterop` and `d3d11va` fail on the legacy `8.970.100.0` driver.
3. **`dxva2-copy` is slower than software decode at `640x480`:** Two slow VRAM↔system-RAM copies increase end-to-end latency and emit `INVALID_ENUM` noise at texture creation. Keep only as a CPU-unload fallback.
4. **`--hwdec=auto` rejected:** Triggers four probe device creations per start; `--hwdec=no` makes startup deterministic.
5. **Initial `"non-existing PPS 0 referenced"`:** Normal mid-GOP join behavior; clears on the first received IDR keyframe.
6. **Startup time:** `~0.73 s` (TCP connect + wait for next IDR keyframe).
7. **Never use `--untimed` for live `tcp://` diagnostics:** `--untimed` consumes faster than real time, hits the live edge, and triggers periodic `(Buffering)` every `2–3 s`.
8. **Overlay stats (`i` or `Shift+I`):** Absence of `(hw)` after the codec name confirms software decode; `"Context: direct3d"` or `"Context: d3d11"` indicates the VO render context, and `Total Cache` / `Dropped Frames` confirm zero-backlog streaming.
9. **Board-side stream health (`master`):** With `TCP_NODELAY`, `64 KiB` `SO_SNDBUF`, `16 KiB` `TCP_NOTSENT_LOWAT`, `5 s` send timeout, `max_delay=0`, `flush_packets=1`, `+pat_pmt_at_frames+resend_headers`, `300 ms` `BasicPacketQueue` with `SessionPtsFilter` muxer-space flush rebase, and `2000 kbps / gop 50` multi-view defaults, timed mode with `--demuxer-readahead-secs=0 --demuxer-lavf-buffersize=4096 --framedrop=vo` runs with **zero** `(Buffering)` stalls (`~8–11 KiB` / `0.0 s` cache) and **zero** dropped frames.
10. **Render path A/B (`mpv` 0.41, `640x480` @ `20 FPS`):** `gpu-next/d3d11` (`~10.7–11.5 ms` fresh, `~1.5 ms` redraw) vs `gpu/opengl` (`~4.0–4.1 ms` fresh, `~0.15–0.26 ms` redraw); both are well within the `50 ms` budget with `0` dropped frames, while `--vo=direct3d` provides the lowest-overhead default across 32-bit/64-bit `mpv` builds.

## Diagnostic Cheat Sheet (PowerShell)

```powershell
# Check board TCP port reachability
Test-NetConnection <board-ip> -Port 1234

# Inspect active power scheme WLAN Power Saving Mode and PCIe ASPM (expect 0x0)
powercfg /query SCHEME_CURRENT 19cbb8fa-5279-450e-9fac-8a3d5fedd0c1 12bbebe6-58d6-4636-95bb-3217ef867c1a
powercfg /query SCHEME_CURRENT 501a4d13-42af-4429-9fd1-a8218c268e20 ee12f906-d277-404b-b6da-e5fa1a576df5

# Create log directory (mpv does not create it automatically)
New-Item -ItemType Directory -Force C:\mpv64

# Decode + network only (no video output)
mpv.com --no-config --vo=null --profile=low-latency --demuxer-readahead-secs=0 --demuxer-lavf-buffersize=4096 tcp://<board-ip>:1234

# Software GDI fallback (no GPU)
mpv.com --no-config --vo=gdi --profile=low-latency --demuxer-readahead-secs=0 --demuxer-lavf-buffersize=4096 tcp://<board-ip>:1234

# Measure startup timing ("playback restart complete" in log)
mpv.com --no-config --vo=null --frames=30 --profile=low-latency --log-file=C:\mpv64\log.txt tcp://<board-ip>:1234

# Verify option acceptance (empty output = accepted)
mpv.com --no-config -v --frames=1 --log-file=C:\mpv64\opt.log tcp://<board-ip>:1234
Select-String -Path C:\mpv64\opt.log -Pattern "Could not set"

# Toggle mpv OSD stats during live playback: press i or Shift+I
# Check GPU and driver version
Get-CimInstance Win32_VideoController | Format-List Name, DriverVersion, DriverDate
```
