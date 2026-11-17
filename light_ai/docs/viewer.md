# Windows Viewer Notes (`mpv` on Legacy Hardware)

This document captures the verified client-side configuration for viewing the `light_ai` H.264/MPEG-TS TCP pull stream (`tcp://<board-ip>:1234`) on a legacy Windows laptop, originally recorded on 2026-10-31 and revised after live A/B testing on 2026-11-01.

For general server-side stream options (`--net`, `--net-h264`, `--net-port`, `--net-bitrate`, `--net-fps`) and `ffplay` commands, see [README.md](../README.md).

## Test Machine & Stream Profile

- **Client hardware:** Intel Core 2 Duo T9550, ATI Mobility Radeon HD 3400 (legacy driver `8.970.100.0`, 2013-04-24 — final release for this series), DirectX 10.1 (exposes D3D11 feature level `10_1`), UVD2 H.264 decode via DXVA2, OpenGL without `WGL_NV_DX_interop`.
- **Player:** Windows 64-bit `mpv` 0.41 (`mpv.com` console host; `mpv.exe` prints no console logs).
- **Stream:** H.264/MPEG-TS over TCP pull (`tcp://<board-ip>:1234`, confirmed by demuxer `"Found 'mpegts' at score=100"`), canonical `640x480` single-view band (`1280x960` source).
- **Cadence note:** `--net-fps` defaults to `25` (`0` = VFR follow-source), whereas USB camera pacing is a discrete firmware state `{20, 25, 30}` FPS — minor cadence differences reflect camera pacing, not a player defect.

## Recommended Launch (Product Path: Software Decode + `gpu-next`/D3D11)

No `%APPDATA%\mpv\mpv.conf` is required; pass all flags on the command line.

**Auto-reconnect batch loop (`C:\mpv64\watch.bat`):**

```bat
@echo off
:loop
echo %date% %time% >> C:\mpv64\restarts.log
mpv.com --cache=no --profile=low-latency --network-timeout=5 tcp://<board-ip>:1234
timeout /t 1 >nul
goto loop
```

**One-shot from PowerShell:**

```powershell
mpv.com --cache=no --profile=low-latency --network-timeout=5 tcp://<board-ip>:1234
```

**Fallback (CPU unload via `dxva2-copy` at higher latency):**

```powershell
mpv.com --vo=gpu --gpu-api=opengl --gpu-context=win --hwdec=dxva2-copy --profile=low-latency --network-timeout=5 --opengl-swapinterval=0 tcp://<board-ip>:1234
```

### Option Rationale

- **No `--hwdec`:** Software decode is optimal at `640x480` (negligible CPU load). `d3d11va` is unsupported by the legacy driver, zero-copy `dxva2` is unreachable, and `dxva2-copy` adds two GPU↔RAM copies per frame.
- **`--profile=low-latency --cache=no`:** Cuts demuxer/player buffering; `--cache=no` is kept explicit for self-documentation.
- **`--network-timeout=5`:** Recovers cleanly when the board restarts or drops the socket.
- **No `recv_buffer_size` trim or `opengl-swapinterval=0` on `d3d11`:** Only needed for the OpenGL fallback; Nagle is already disabled (`TCP_NODELAY`) on the board side, and trimming receive buffers hurts IDR bursts.
- **Auto-reconnect loop:** `mpv`'s built-in `reconnect*` options apply only to HTTP, not raw `tcp://`. Press `q` in `mpv` to force an immediate reconnect, or close the console / `Ctrl+C` to stop.

## Measured Latency (Timed Mode)

| Configuration | Observed Lag | Notes |
|---|---|---|
| Default `mpv` (cache enabled) | `2–3 s` | Demuxer/cache buffering |
| `--profile=low-latency --cache=no` (**recommended**) | `~1 s` | Stopwatch estimate (2026-11-01); board GOP 25 |
| `+ --hwdec=dxva2-copy` | Visibly worse | Two frame copies (`GPU -> RAM -> GPU`) |
| Photo-timestamp clock measurement | Pending | Requires burning a wall-clock timestamp into frames |

Cross-check player:

```bash
ffplay -fflags nobuffer -rtbufsize 100k tcp://<board-ip>:1234
```

## Established A/B Facts (2026-10-31 / 2026-11-01)

1. **`gpu-next` works on HD 3400:** Initializes `d3d11` at feature level `10_1` (`1002:95c4`) with flip-model presentation and SPIR-V via `shaderc`.
2. **Zero-copy decode is unreachable:** Both `win/angle/dxinterop` and `d3d11va` fail on the legacy `8.970.100.0` driver.
3. **`dxva2-copy` is slower than software decode at `640x480`:** Two slow VRAM↔system-RAM copies increase end-to-end latency and emit `INVALID_ENUM` noise at texture creation. Keep only as a CPU-unload fallback.
4. **`--hwdec=auto` rejected:** Triggers four probe device creations per start; omitting `--hwdec` makes startup deterministic.
5. **Initial `"non-existing PPS 0 referenced"`:** Normal mid-GOP join behavior; clears on the first received IDR keyframe.
6. **Startup time:** `~0.73 s` (TCP connect + wait for next IDR keyframe).
7. **Never use `--untimed` for live `tcp://` diagnostics:** `--untimed` consumes faster than real time, hits the live edge, and triggers periodic `(Buffering)` every `2–3 s`.
8. **Overlay stats (`i` key):** Absence of `(hw)` after the codec name confirms software decode; `"Context: d3d11"` indicates the VO render context.
9. **Board-side stream health:** With `TCP_NODELAY`, `2 MiB` `SO_SNDBUF`, `15 s` `SO_SNDTIMEO`, `max_delay=0`, `flush_packets=1`, and `+pat_pmt_at_frames+resend_headers` (at `--net-bitrate 3000`), timed mode runs with **zero** `(Buffering)` stalls (`0.0–1.0 s` cache).
10. **Render path A/B (`640x480` @ `20 FPS`):** `gpu-next/d3d11` (`~10.7–11.5 ms` fresh, `~1.5 ms` redraw) vs `gpu/opengl` (`~4.0–4.1 ms` fresh, `~0.15–0.26 ms` redraw); both are well within the `50 ms` budget with `0` dropped frames. `d3d11` is chosen for flip-model presentation, silent logs, and `0.0 s` cache.

## Diagnostic Cheat Sheet (PowerShell)

```powershell
# Check board TCP port reachability
Test-NetConnection <board-ip> -Port 1234

# Create log directory (mpv does not create it automatically)
New-Item -ItemType Directory -Force C:\mpv64

# Decode + network only (no video output)
mpv.com --vo=null --profile=low-latency tcp://<board-ip>:1234

# Software GDI fallback (no GPU)
mpv.com --vo=gdi --profile=low-latency tcp://<board-ip>:1234

# Measure startup timing ("playback restart complete" in log)
mpv.com --vo=null --frames=30 --profile=low-latency --log-file=C:\mpv64\log.txt tcp://<board-ip>:1234

# Verify option acceptance (empty output = accepted)
mpv.com -v --frames=1 --log-file=C:\mpv64\opt.log tcp://<board-ip>:1234
Select-String -Path C:\mpv64\opt.log -Pattern "Could not set"

# Check GPU and driver version
Get-CimInstance Win32_VideoController | Format-List Name, DriverVersion, DriverDate
```
