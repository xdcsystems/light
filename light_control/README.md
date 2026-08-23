# light_control

OpenWrt daemon for a TP-Link TL-WDR4300 v1 (ath79 / `mips_24kc`, OpenWrt 23.05). Package version **1.0.23**.

It listens for UDP lux readings from `light_sensor`, maps lux and local time to a brightness value, and exposes the result over ubus and LuCI. The dimmer is a syslog stub (`dimmer set_brightness N`), not a GPIO/LED-strip driver.

The sensor only measures and sends lux. Brightness, scenes, and manual hold live on this node. ESP32 firmware and Kconfig: see [`../light_sensor/README.md`](../light_sensor/README.md).

## Features

- UDP server, default port `5005`, bind address `0.0.0.0` (UCI `interface` is stored, not used for bind).
- 3-byte lux protocol (incompatible with the old 2-byte `[id][0–100]` payload).
- UCI via libuci: enable flag, port, brightness map, named map sets, scenes.
- ubus object `light_control`: `get_stats`, `get_status`, `get_config`, `reload`, `set_brightness`.
- LuCI app `luci-app-light-control`: Services → Light Control → Status / Settings.
- Graceful stop: atomic `stop_flag`, `uloop_run_timeout(200)`, then `uloop_done()` so procd does not need `SIGKILL`.
- No heap in core logic. Fixed buffers, `std::array` / `std::sort`. State lives in class fields (ubus C callbacks recover `this` with `offsetof`).

## UDP protocol

Exactly **3 bytes**: `[device_id][lux_hi][lux_lo]`. Lux is `uint16` big-endian (0–65535). This is not the BH1750 I2C frame (that is 2 bytes of raw data).

Any other length is rejected (`Invalid length`) and increments `errors`. Update sensor and controller together.

After a valid packet:

```
parse 3 bytes
  → scene from local clock
  → brightness map (optional named set)
  → optional min/max clamp
  → manual hold, if active
  → Dimmer.set_brightness (syslog)
  → LastStatus + get_stats counters
```

Scenes use `localtime_r`. Without NTP the clock starts at 1970 and night/morning will be wrong.

## UCI (`/etc/config/light_control`)

Shipped file (`openwrt_pkg/light_control/files/light_control.config`):

- `option enabled '1'`
- default map: lux &lt; 200 → 100, &lt; 400 → 60, else 30
- scenes `morning` 06:00–11:00, `day` 11:00–18:00, `night` 18:00–06:00 (night clamp 10–40)

Code defaults if a `config light_control` section is missing: `enabled=1`, `port=5005`, `interface=lan`. LuCI Settings writes that service section (`enabled` / `port` / `interface`).

`enabled=0` prevents start (init script and daemon). `reload` does not kill an already running process. Changing the UDP port still needs `/etc/init.d/light_control restart` (the socket is not rebound).

## ubus API

```sh
ubus list | grep light_control

ubus call light_control get_stats
ubus call light_control get_status
ubus call light_control get_config
ubus call light_control reload
ubus call light_control set_brightness '{"value":40}'
ubus call light_control set_brightness '{"release":true}'
```

| Method | Role |
|---|---|
| `get_stats` | `packets_received`, `bytes_received`, `errors` |
| `get_status` | last valid packet: `has_packet`, `device_id`, `lux`, `brightness` (after hold), `override`, `scene`, `source_ip`, `source_port`, `unix_time` |
| `get_config` | live snapshot the daemon is using: `port` vs `bound_port`, `interface`, maps, scenes, hold |
| `reload` | re-read UCI maps/scenes/`enabled`/`port`/`interface` without restarting. Does not rebind, clear stats, or drop hold |
| `set_brightness` | runtime hold. `release: true` wins even next to `value`. `0` is valid (off). Values &gt; 100 clamp to 100. Empty call → `UBUS_STATUS_INVALID_ARGUMENT`. Not stored in UCI |

Right after start, `get_stats` is all zeros and `get_status` has `has_packet: false`. Hold can be set before the first UDP packet.

## LuCI

Install **both** ipk packages, then restart rpcd:

```sh
/etc/init.d/rpcd restart
```

Menu: **Services → Light Control**

- **Status** — stats, last output, live config (poll ~3 s), Hold / Release / Reload UCI
- **Settings** — service flag/port/interface, map table, scenes. Save & Apply restarts the daemon via procd. Map/scene edits can also be picked up with Reload UCI (new UDP port still needs a restart)

## Repository structure

```
light_control/
├── CMakeLists.txt          # host + OpenWrt; single source list
├── src/                    # application, udp_server, ubus_exporter, uci_settings, …
├── include/
├── tests/selftest.cpp      # host selftest (no ubus)
├── openwrt_light_control_build.sh
├── openwrt_light_control_deploy.sh
├── openwrt_pkg/
│   ├── light_control/      # Makefile, init.d, default UCI
│   └── luci-app-light-control/
├── externals/openwrt       # OpenWrt SDK (gitignored)
└── README.md
```

The package Makefile copies sources from `PKG_SOURCE_PATH:=../../../light_control` (relative to an SDK checked out as `externals/openwrt`). Do not copy the tree into the SDK. Do not remove the CMake `install` target.

## Dependencies

OpenWrt package `DEPENDS`: `libuci`, `libubox`, `libubus`, `libstdcpp`, `libatomic`, `libc`, `libpthread`.

Toolchain: OpenWrt SDK for `ath79/generic`, GCC 12.3 + musl. `uloop` comes from `libubox`, not a separate `libuloop` package.

## OpenWrt SDK

Clone or unpack the **23.05** SDK for `ath79/generic` into `light_control/externals/openwrt`. That directory is gitignored and must not be pushed.

Point a feed at `openwrt_pkg` (for example `src-link light …/openwrt_pkg` in `feeds.conf`). Fresh SDKs do not ship target `libuci` / `libubox` / `libubus`; build those from the `base` feed or the daemon will not link.

Confirm `.config` has:

```
CONFIG_TARGET_ath79=y
CONFIG_TARGET_ath79_generic=y
```

The package uses OpenWrt `cmake.mk` plus this repo’s `CMakeLists.txt` (`-DOPENWRT_BUILD=ON -DENABLE_UBUS=ON`).

## Building the packages

Default SDK path used by the scripts (override with `OPENWRT_SDK`):

```
$HOME/Projects/light/light_control/externals/openwrt
```

From `light_control/`:

```sh
./openwrt_light_control_build.sh
```

The script compiles `light_control`, then packs **luci-app-light-control** itself: it copies the JS views, menu, and rpcd ACL into a noarch ipk via the SDK `scripts/ipkg-build`. That avoids `luci.mk` / `luci-base` host tools (lemon, po2lmo), which this JS app does not need. Override the tree with `LIGHT_CONTROL_SRC` if the script is not run from `light_control/`.

Or from the SDK (daemon only — LuCI still needs the script’s step 3):

```sh
cd externals/openwrt
make package/index
make package/light_control/compile V=s
```

Output lives under `bin/packages/mips_24kc/light_control/` (not `bin/targets/ath79/generic/`):

```
bin/packages/mips_24kc/light_control/light_control_1.0.23-1_mips_24kc.ipk
bin/packages/mips_24kc/light_control/luci-app-light-control_1.0.23-1_all.ipk
```

A copy is also written to `../ipk/` when that directory can be created. Deploy installs **both** ipk files.

## Installation on the router

```sh
scp light_control_1.0.23-1_mips_24kc.ipk luci-app-light-control_1.0.23-1_all.ipk root@<router_ip>:/tmp/
ssh root@<router_ip>
opkg install --force-reinstall /tmp/light_control_1.0.23-1_mips_24kc.ipk /tmp/luci-app-light-control_1.0.23-1_all.ipk
/etc/init.d/light_control restart
/etc/init.d/rpcd restart
```

Or from `light_control/` (default router `192.168.1.1`, same SDK path as the build script):

```sh
./openwrt_light_control_deploy.sh
```

## Start / stop

```sh
/etc/init.d/light_control start
/etc/init.d/light_control stop
/etc/init.d/light_control restart
```

## Verifying operation

```sh
ps | grep light_control
ubus list | grep light_control
ubus call light_control get_stats
ubus call light_control get_status
ubus call light_control get_config
logread | grep light_control | tail -n 30
```

After a 3-byte packet: syslog `Device N lux=… brightness=… scene=…` (suffix ` override` while hold is on) and `dimmer set_brightness N` when the value changes. Repeats of the same brightness are not logged.

Confirm stop without `SIGKILL`:

```sh
logread | grep -E "SIGKILL|not stopped"
```

## Host build (no router)

`ENABLE_UBUS` defaults to OFF. From `light_control/`:

```sh
cmake -S . -B build
cmake --build build
./build/light_control_selftest
```

Host `reload` has no libuci and loads code defaults. The ubus methods exist only in the OpenWrt build.

## Remaining work

- Real LED-strip driver (GPIO / UART / MQTT) when the output schematic is known. WDR4300 has no strip connector.
- Bind UDP to the UCI `interface` instead of `0.0.0.0`.
