# План: light_sensor + light_control + LuCI

Документ фиксирует исходную задачу и **что осталось**. Подробно, что уже сделано и почему — в [`DONE.md`](DONE.md) (роутер отдельно, ESP32 отдельно).

Целевое железо контроллера: TP-Link TL-WDR4300 v1, OpenWrt 23.05, LuCI, ubus.
Сенсор: ESP32 / Linux / тестовая заглушка без железа.

**Железа на руках нет** (роутер, ESP32, BH1750, лента — у клиента; плата **не приедет**, пишем только софт). Клиент прислал модели: **BH1750FVI** и **ESP32-WROOM-32 30-pin USB CH340C** (чип ESP32-D0WD-V3 rev 3.1, 40 МГц, MAC `68:fe:71:f9:fd:90`). Установка ipk, проверка LuCI в браузере, NTP на устройстве и живой выход на ленту **не выполняются**. Шаг 5 закрыт на хосте: WSL + selftest + ipk **1.0.23** без установки. Шаг 6 (сенсор) закрыт в исходниках и **собран** под ESP-IDF v5.5.5; на плату не прошивалось.

| Шаг | Суть | Статус |
|---|---|---|
| 1 | UCI libuci + пакет LuCI (Status/Settings) | Сделано в исходниках → [DONE §1.2](DONE.md) |
| 2 | SDK + ipk `mips_24kc` | Сборка **сделана**. На диске актуальные **1.0.23**. **Установка на роутер заблокирована** — устройства нет |
| 3 | Протокол 3 байта lux, кривая на контроллере | Сделано; selftest → [DONE §1.4](DONE.md) |
| 4 | Сцены утро/день/ночь + лог-заглушка диммера | Сделано (исходники, selftest ok). ipk 1.0.22 не пересобран, сразу 1.0.23 → [DONE §1.5](DONE.md) |
| 5 часть | ubus `get_status` + страница Status | Сделано в исходниках → [DONE §1.6](DONE.md) |
| 5 остаток | `reload` / `set_brightness` / `get_config` | **Сделано** в исходниках **1.0.23** (selftest ok). ipk пересобран, на роутер не ставился → [DONE §1.7](DONE.md) |
| 6 сенсор | ESP-IDF + I2C BH1750FVI + Wi-Fi/UDP | **Собрано** (IDF v5.5.5), не прошивалось → [DONE §2.8](DONE.md) |
| 6 остаток | Драйвер ленты, bind на `interface` | Схема ленты неизвестна; роутера нет |
| 7 | Синхронизация ESP32, нейросеть | Дальний горизонт |

---

## Исходная задача

Два приложения должны работать и без реальных устройств: сенсор шлёт фейковые данные, контроллер логирует.

На OpenWrt уже есть ubus и GUI LuCI. На WDR4300 (8 MB flash) python3 практически не влезает — GUI это JS-приложение LuCI поверх C++ ubus-объекта `light_control`. Алгоритм яркости и сцены — **на контроллере**; сенсор только шлёт lux. Кривая, сцены, лог-заглушка диммера и ubus `reload` / `set_brightness` / `get_config` уже в коде (ipk **1.0.23** на диске). Дальше без железа — только опциональная localhost-петля.

---

## Как устроено сейчас

```
  ESP32 (BH1750FVI) / Linux Stub     OpenWrt (роутер)
  ┌──────────────────────────┐       ┌─────────────────────────────────┐
  │ light_sensor             │       │ light_control                   │
  │ lux only                 │ UDP   │ UdpServer → parse 3 байта       │
  │ [id][lux_be]  :5005 ─────┼──────►│ → SceneScheduler (local time)   │
  │                          │       │ → BrightnessPolicy.apply(lux)   │
  │ ESP32: I2C + Wi-Fi STA   │       │ → BrightnessOverride.apply      │
  │ Linux/TEST: Stub 220 lux │       │ → Dimmer.set_brightness (лог)   │
  └──────────────────────────┘       │ → syslog + StatsCollector       │
                                     │ → ubus object light_control     │
                                     │      get_stats / get_status     │
                                     │      get_config / reload        │
                                     │      set_brightness             │
                                     └──────────────┬──────────────────┘
                                                    │
                                     LuCI JS ───────┘  UCI /etc/config/light_control
                                                       (service + map + scene)
```

Протокол пакета: ровно 3 байта `[device_id][lux_hi][lux_lo]`, lux = `uint16` big-endian. Совместимость со старым 2-байтным `[id][0–100]` **сломана**. Это не кадр I2C BH1750 (там 2 байта raw). Сцены в stub сенсора (`Morning` 450 / `Evening` 220 / `Night` 50) — фейковые люксы, не расписание контроллера.

Диммер на контроллере — syslog-заглушка. Реальный драйвер ленты и bind UDP на UCI-`interface` — остаток шага 6.

Жёстко в коде vs конфиг — таблицы в [DONE §1.8](DONE.md#18-что-на-роутере-в-коде-vs-конфиг) и [DONE §2.6](DONE.md#26-что-на-сенсоре-в-коде-vs-конфиг).

---

## Что осталось

Железа у нас нет — **не** ставим ipk и **не** прошиваем ESP32. Шаг 5 закрыт в исходниках. Сенсорная часть шага 6 собрана (ESP-IDF v5.5.5); чеклист клиента — ниже.

**Перед прошивкой клиент обязан заменить `WIFI_SSID` / `WIFI_PASS` (и при необходимости `CONTROLLER_IP`) в [`light_sensor/include/config.hpp`](light_sensor/include/config.hpp) и пересобрать.** Плейсхолдеры `YOUR_SSID` / `YOUR_PASSWORD` специально не поднимают Wi-Fi. Подробности — [DONE §2.8](DONE.md#28-сборка-прошивки-esp32-wsl--esp-idf).

Опционально без железа: локальная петля `light_sensor` LINUX → `127.0.0.1:5005` → хостовой `light_control`, смотреть syslog (`Device … scene=…`, `dimmer set_brightness`). Для этого в [`config.hpp`](light_sensor/include/config.hpp) временно `CONTROLLER_IP = "127.0.0.1"`.

Хостовые бинарники и selftest после шагов 5–6 пересобраны; selftest прошёл. Повторить:

```bash
export MAMBA_ROOT_PREFIX=$HOME/micromamba
~/bin/micromamba run -n cpp cmake --build /mnt/c/Dev/light_control/light/light_control/build_wsl
~/bin/micromamba run -n cpp cmake --build /mnt/c/Dev/light_control/light/light_sensor/build_test
~/bin/micromamba run -n cpp cmake --build /mnt/c/Dev/light_control/light/light_sensor/build_linux
/mnt/c/Dev/light_control/light/light_control/build_wsl/light_control_selftest
```

Пересборка ipk (без установки на роутер) из WSL:

```bash
export PATH="$HOME/openwrt/host-tools/bin:/usr/bin:/bin"
export PYTHONPATH="$HOME/openwrt/pyshim"
/mnt/c/Dev/light_control/light/light_control/openwrt_light_control_build.sh
```

Повторная сборка ESP32 из WSL (IDF уже стоит в `~/esp/esp-idf`):

```bash
/mnt/c/Dev/light_control/light/light_sensor/esp32/build_wsl.sh
```

### Заблокировано на стороне клиента (плата / роутер не у нас)

#### Шаг 2 (хвост). Поставить ipk на роутер

Скрипты и чеклист готовы. Когда у клиента будет WDR4300 в LAN:

- Поставить оба ipk **1.0.23** из [`ipk/`](ipk/), `/etc/init.d/rpcd restart` (или `./openwrt_light_control_deploy.sh`).
- `ubus call light_control get_stats` / `get_status` / `get_config`; `set_brightness` / `reload`; LuCI Services → Light Control.
- Сенсор — только 3-байтный протокол, иначе `Invalid length` и рост `errors`.
- NTP на роутере, иначе сцены от 1970.

```sh
scp light/ipk/*.ipk root@192.168.1.1:/tmp/
ssh root@192.168.1.1
opkg install --force-reinstall /tmp/light_control_1.0.23-1_mips_24kc.ipk /tmp/luci-app-light-control_1.0.23-1_all.ipk
/etc/init.d/light_control restart
/etc/init.d/rpcd restart
```

Проверка на роутере:

```sh
ubus list | grep light_control
ubus call light_control get_stats
ubus call light_control get_status
ubus call light_control get_config
ubus call light_control set_brightness '{"value":40}'
ubus call light_control set_brightness '{"release":true}'
ubus call light_control reload
/etc/init.d/light_control restart
logread | grep light_control | tail -n 30
```

Ожидаемый `get_stats` сразу после старта: все счётчики 0. Ожидаемый `get_status`: `has_packet: false`, `override: false`. После `set_brightness '{"value":40}'` — `override: true`, `brightness: 40`, даже если пакета ещё не было.

В LuCI: Services → Light Control → Status (счётчики + lux/яркость/сцена/override + live config + Hold/Release/Reload UCI) и Settings (enabled/port/interface + таблица порогов + сцены).

После пакета от нового сенсора в syslog: `Device N lux=... brightness=... scene=...` и при смене яркости `dimmer set_brightness N`. Если включён hold — суффикс ` override`. Старый 2-байтный пакет → `Protocol error ... Invalid length`. После `reload` — строка `reload: uci ok, port …`.

#### Шаг 6. Прошивка сенсора + остаток без схемы

Прошивку льёт **клиент**. Образ у нас собран (IDF v5.5.5), но **с плейсхолдерами Wi-Fi — лить его нельзя**. Сначала `config.hpp`, потом пересборка, потом `idf.py -p /dev/ttyUSB0 flash monitor`.

Порядок. Пункты 1–5 — сенсор; 6–7 — если на том же стенде уже есть WDR4300.

1. **ESP-IDF 5.2+** (у нас 5.5.5; у клиента бывает **6.0.2** — в `esp32/CMakeLists.txt` уже есть `-D_GNU_SOURCE`). `idf.py set-target esp32`. IDF 4.x / Arduino / PlatformIO этот `CMakeLists` не возьмут (`esp_driver_i2c`).
2. **Разводка:** VCC→3V3, GND, SDA GPIO21, SCL GPIO22, ADDR на GND (`0x23`). Только 3.3 V.
3. **Прописать** в [`include/config.hpp`](light_sensor/include/config.hpp) **до** `flash` (без этого Wi-Fi не поднимется):
   - `WIFI_SSID` / `WIFI_PASS` сети, куда ходит роутер (открытая: `WIFI_PASS = ""`);
   - при необходимости `CONTROLLER_IP`, пины, `BH1750_I2C_ADDR`.
   Оставить `YOUR_SSID` / `YOUR_PASSWORD` — в мониторе: `Set WIFI_SSID / WIFI_PASS …` и вечный delay. Затем **пересобрать**.
4. **Собрать и прошить** с той машины, где уже работал `esptool` (`/dev/ttyUSB0`, RTS):

```bash
cd light_sensor/esp32
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

5. **Что ждать в serial (115200)** после удачного старта:
   - `node start id=1 -> 192.168.1.1:5005`
   - `got ip a.b.c.d`
   - `UDP -> 192.168.1.1:5005`
   - `BH1750FVI ready at 0x23`
   - раз в секунду `device_id=1 lux=…` (число меняется от света).
6. Если роутер в той же LAN: поставить ipk **1.0.23**, `rpcd restart`. В syslog: `Device 1 lux=… brightness=… scene=…`. В LuCI Status — `has_packet`, lux, сцена.
7. Прислать лог `monitor`, если что-то падает. Типичное:
   - `BH1750 not found at 0x23` — ADDR/пины/питание/другой адрес `0x5C`;
   - `Wi-Fi connect timeout` — SSID/пароль, 2.4 ГГц (ESP32 без 5 ГГц), AP не WPA3-only;
   - `UDP send failed` при живом lux — нет маршрута до `192.168.1.1:5005` (роутер, firewall, демон не слушает).

Не путать с шагом 4: лог-заглушка диммера уже пишет без ленты. После живого сенсора в софте ещё:

- Драйвер ленты, когда будет схема выхода (GPIO PWM / UART / ESP32 / MQTT). У WDR4300 готового разъёма под ленту нет.
- Bind UDP на выбранный `interface` (поле в UCI/GUI есть, слушается `0.0.0.0`).

#### Шаг 7. Дальний горизонт

- Синхронизация состояния с ESP32.
- Локальная нейросеть для адаптивного управления — не блокирует шаг 6.
