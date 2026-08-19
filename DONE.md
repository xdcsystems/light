# DONE: что сделано и почему

Документ фиксирует **закрытую работу** по двум узлам: роутер (`light_control` + LuCI) и ESP32 (`light_sensor`). Что ещё ждать железа или схемы — в [`PLAN.md`](PLAN.md).

Железа на руках нет (роутер, ESP32, BH1750, лента — у клиента; плата **не приедет**, пишем только софт). Клиент прислал модели: **BH1750FVI** и **ESP32-WROOM-32 30-pin USB CH340C** (ESP32-D0WD-V3 rev 3.1, 40 МГц, MAC `68:fe:71:f9:fd:90`). Установка ipk, проверка LuCI в браузере, NTP на устройстве, прошивка платы и живой выход на ленту **не выполнялись**.

Актуальная версия контроллера: исходники и ipk **1.0.23**. Сенсор: исходники + **собранный** образ ESP-IDF **v5.5.5** (на плату не лили).

> **Перед прошивкой ESP32 обязательно замени данные в [`light_sensor/include/config.hpp`](light_sensor/include/config.hpp).**
> Сейчас там плейсхолдеры `WIFI_SSID = "YOUR_SSID"` / `WIFI_PASS = "YOUR_PASSWORD"` и `CONTROLLER_IP = "192.168.1.1"`.
> С этими значениями прошивка **намеренно не поднимает Wi‑Fi** (в мониторе: `Set WIFI_SSID / WIFI_PASS …` и вечный delay).
> Готовый `.bin` с плейсхолдерами **лить нельзя** — сначала SSID/пароль (и IP роутера, если не `192.168.1.1`), затем пересборка, затем `flash`. Открытая сеть: `WIFI_PASS = ""`.

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

Протокол: ровно 3 байта `[device_id][lux_hi][lux_lo]`, lux = `uint16` big-endian. Совместимость со старым 2-байтным `[id][0–100]` **сломана** — сенсор и контроллер обновляются вместе. Это не кадр I2C BH1750 (там 2 байта raw): UDP — отдельный протокол между прошивкой и демоном.

---

# Часть 1. Роутер (OpenWrt / `light_control`)

Целевое железо: TP-Link TL-WDR4300 v1, OpenWrt 23.05, LuCI, ubus. Архитектура ath79 / `mips_24kc`. На устройстве **не ставилось** — закрыто сборкой ipk, хостовым демоном и `light_control_selftest`.

## 1.1 Зачем такой стек, а не другой

На OpenWrt уже есть ubus и GUI LuCI. Модуль *можно* писать на Python через rpcd, но у WDR4300 **8 MB flash**: python3 практически не влезает. Демон уже регистрирует C++ ubus-объект `light_control`, поэтому GUI — JS-приложение LuCI поверх существующего объекта (браузер → uhttpd → rpcd + ACL → ubusd → демон).

Алгоритм яркости живёт **на контроллере**, не на сенсоре. Иначе GUI на роутере не мог бы задавать пороги: сенсор слал бы уже готовую яркость 0–100. BH1750 даёт до десятков тысяч lux — в один байт 0–100 это не умещается. Сенсор только измеряет и шлёт lux; кривая, сцены, hold и выход — ответственность роутера.

Память: без heap в ядре логики. Кривая — до 8 порогов, сцены — до 8, именованные карты — до 4. Фиксированные буферы на стеке. JSON/TLV для UDP не брали по той же причине: фиксированный 3-байтный кадр.

Два приложения должны работать и без реальных устройств: сенсор шлёт фейковые данные, контроллер логирует. Пока нет железа, проверка — WSL + selftest + кросс-ipk.

## 1.2 UCI через libuci и пакет LuCI

**Почему.** Раньше порт читался через `fopen` + `sscanf("option port %hu")`. Строка `option port '5005'` из-за кавычек не парсилась, порт всегда оставался 5005. Поля `enabled` и `interface` игнорировались. Без нормального UCI нельзя ни выключить сервис из GUI, ни править пороги без пересборки.

**Что сделано.**

| Файл | Назначение |
|---|---|
| [`light_control/include/uci_settings.h`](light_control/include/uci_settings.h) | `UciSettings`: `enabled`, `port`, `iface`, `BrightnessPolicy policy` |
| [`light_control/src/uci_settings.cpp`](light_control/src/uci_settings.cpp) | `load_uci_settings()` через libuci при `OPENWRT_BUILD`, иначе дефолты |
| [`light_control/src/application.cpp`](light_control/src/application.cpp) | `enabled=0` — не стартовать (код 1). Порт из UCI. Bind пока `0.0.0.0`. `interface` только логируется |
| [`light_control/src/main.cpp`](light_control/src/main.cpp) | `init() < 0` → ошибка; `init() > 0` → штатно выключен |
| [`light_control/openwrt_pkg/light_control/files/light_control.init`](light_control/openwrt_pkg/light_control/files/light_control.init) | Не стартует процесс при `enabled=0`; `procd_add_reload_trigger "light_control"` — Save & Apply в LuCI перезапускает сервис |

Дефолты без UCI: `enabled=1`, `port=5005`, `interface=lan`, кривая 200→100 / 400→60 / 65535→30. `enabled` понимает `0/1`, `on/off`, `true/false`, `yes/no`, `enabled/disabled`.

Пакет [`luci-app-light-control`](light_control/openwrt_pkg/luci-app-light-control/): меню Services → Light Control → Status / Settings, ACL на ubus и UCI, JS-страницы. Настройки: LuCI пишет UCI → `reload_config` / procd trigger → рестарт демона. Живая подмена карты без рестарта — отдельная кнопка Reload UCI (см. §1.7).

Цепочка GUI: браузер → uhttpd → rpcd (ACL) → ubusd → демон.

## 1.3 OpenWrt SDK и ipk `mips_24kc`

**Почему.** Нужен `.ipk` под ath79, не desktop-бинарь. Полный git OpenWrt для этого избыточен: официальный SDK, как в [Using the SDK](https://openwrt.org/docs/guide-developer/toolchain/using_the_sdk). На `C:` было мало места и NTFS/`/mnt/c` для OpenWrt медленный — SDK живёт на диске WSL.

**Что сделано.** SDK 23.05.5 ath79-generic (gcc 12.3, musl), SHA256 совпал с `sha256sums`. Пути: `$HOME/openwrt/sdk` (symlink) и `light_control/externals/openwrt`. Архив в git не коммитится.

Host без root: `apt` в WSL требует sudo, toolchain через micromamba (`g++` 16, cmake, ninja). Для SDK — узкий `$HOME/openwrt/host-tools/bin` (не весь `cpp/bin`: туда попал python 3.14 без distutils, а SDK host gcc через `/usr/bin/env bash` давал «Too many levels of symbolic links»). Фейковый `distutils` в `$HOME/openwrt/pyshim` для системного python3.10. `PATH` = host-tools + `/usr/bin`. Кросс-компилятор — из самого SDK.

Feeds: мелкий `src-git base` (не `src-git-full`), luci pin, `src-link light` на `openwrt_pkg`. Свежий SDK **не содержит** target-библиотек: `libuci` / `libubox` / `libubus` собираются из feed `base`, иначе демон не линкуется.

Путь к исходникам в Makefile пакета: `PKG_SOURCE_PATH` через `LIGHT_CONTROL_SRC` или абсолютный путь. Относительный `../../../light_control` работал только если SDK лежал внутри репозитория.

`luci.mk` **не прошёл**: host-сборка `luci-base` компилирует старый `lemon.c` (K&R) хостовым gcc 16 → `conflicting types for 'FindRulePrecedences'`. Для JS-приложения minify/`po2lmo` не обязательны — noarch ipk собирается вручную через `scripts/ipkg-build`.

Скрипты: [`openwrt_light_control_build.sh`](light_control/openwrt_light_control_build.sh) (compile + ручная упаковка luci-app + копия в `light/ipk/`), [`openwrt_light_control_deploy.sh`](light_control/openwrt_light_control_deploy.sh) (scp + opkg — **не запускался**, нет роутера).

| Пакет | Куда |
|---|---|
| `light_control_1.0.23-1_mips_24kc.ipk` | [`ipk/`](ipk/) и `$HOME/openwrt/sdk/bin/packages/mips_24kc/light/` |
| `luci-app-light-control_1.0.23-1_all.ipk` | туда же |

История версий: 1.0.20 — первая успешная сборка; 1.0.21 — `get_status`; 1.0.22 — сцены/диммер (ipk не выпускался); **1.0.23** — `reload` / `set_brightness` / `get_config`. Рядом на диске остаются 1.0.20 / 1.0.21 (не ставить).

## 1.4 Протокол 3 байта и кривая lux→яркость

**Почему.** Старый пакет `[id][dim 0–100]` делал сенсор «умным», а GUI — бесполезным для порогов. BH1750 не влезает в 1 байт. JSON не брали: фиксированный буфер, без динамической памяти. Алгоритм на роутере — тогда Settings может править `config map`.

**Что сделано.** Парсер: длина строго 3, `lux = (b1 << 8) | b2`, проверка `value ≤ 100` убрана. Сенсор клипит lux в 0–65535 и шлёт `[DEVICE_ID][lux>>8][lux&0xFF]`.

Кривая [`brightness_curve`](light_control/include/brightness_curve.h): фиксированный массив до 8 записей, `sort` по `lux_below`, `apply(lux)` — первый `lux < lux_below`, иначе последняя запись, иначе 30. Правила UCI: `lux_below` 1–65535, `brightness` 0–100; мусор пропускается; если ни одной валидной `map` — дефолты. Больше 8 порогов — лишние `add()` отвергаются.

Дефолт совпадает со старым `compute_dim_value` на сенсоре (чтобы поведение не прыгнуло при переносе алгоритма):

| lux | brightness |
|---|---|
| &lt; 200 | 100 |
| &lt; 400 | 60 |
| иначе | 30 |

LuCI Settings: `form.TableSection` типа `map`, `addremove`, datatype `range`. Демон сортирует пороги при загрузке. В шаге со сценами `UdpServer::set_curve` заменён на `set_policy` / `BrightnessPolicy`.

## 1.5 Сцены утро/день/ночь и слой диммера

**Почему.** Разное свечение в зависимости от **времени суток и** датчика. Не путать со `SensorScenario` в stub сенсора (`Morning` 450 / `Evening` 220 / `Night` 50) — это фейковые **люксы**, не расписание. Часы — локальные на роутере (`localtime_r`); NTP на устройстве появится вместе с железом (без NTP сцена пойдёт от эпохи 1970).

Реальный выход на ленту не угадывали: в репозитории нет типа разъёма, у WDR4300 нет готового коннектора. Поэтому `Dimmer` — программный слой с тем же API `set_brightness(0–100)`, что понадобится драйверу. Сейчас пишет в syslog; позже можно заменить `.cpp`, не трогая вызов.

**Цепочка после валидного UDP** (итоговая, с hold из §1.7):

```
parse 3 байта
  → BrightnessPolicy.apply(lux)     # сцена + кривая + clamp
  → BrightnessOverride.apply        # если hold — подмена N
  → LastStatus.update(..., override)
  → Dimmer.set_brightness           # syslog, не GPIO
  → лог Device N lux=… brightness=… scene=… [override]
```

**Сцены.** [`scene_scheduler`](light_control/include/scene_scheduler.h), без heap. До 8 сцен: имя, `from_min`/`to_min` (0–1439), опционально `map_set`, min/max. Интервалы:

- `from == to` — вся сутки;
- `from < to` — `[from, to)` (11:00 уже day, не morning);
- `from > to` — через полночь (`night` 18:00–06:00).

`parse_hhmm`: строго `"H:MM"` / `"HH:MM"`, `24:00` и мусор отвергаются. Выбор: первая подходящая сцена; если ни одна — кривая всё равно применяется, имя пустое, clamp нет.

Дефолты: morning 06:00–11:00, day 11:00–18:00, night 18:00–06:00 с clamp 10–40.

**`BrightnessPolicy`.** Кривая + сцена + clamp. Если у сцены непустой `map_set` и такой named set есть — он; неизвестный имя молча падает на дефолт. Новый named set создаётся с **пустой** кривой (не с дефолтной тройкой), иначе unnamed дефолты утекли бы в named. До 4 именованных карт.

UCI: один проход по секциям `light_control` / `map` / `scene`. Подмена дефолтов только если нашлось хотя бы одно валидное соответствующего типа. В дефолтном конфиге named `map_set` нет (колонка Set name в GUI пустая); код и LuCI это поддерживают, selftest проверяет имя `daylight`.

**Диммер.** Значение > 100 режется до 100. Повтор того же N **не логируется** (сенсор шлёт раз в 1 с — иначе syslog зальёт). Первое значение и смена — да.

LuCI Settings: секции `map` (опционально поле `set`) и именованные `scene` (`from_time` / `to_time`, опционально `map_set`, min/max). Status показывает имя сцены.

## 1.6 ubus `get_status`

**Почему.** Минимум для дальнейшей работы — демон отдаёт не только счётчики пакетов (`get_stats`), но и последнее чтение: иначе Status в GUI пустой, пока не откроешь syslog. Без роутера страницу не открыть, метод и JS готовы в исходниках.

**Что сделано.** После валидного пакета демон запоминает `LastStatus`: device_id, lux, **фактическая** яркость (после override), флаг override, scene, source IP/port, unix time. Mutex между UDP- и ubus-потоками.

Пока пакетов не было — `has_packet: false`, страница пишет «No valid sensor packet received yet». Hold можно поставить до первого UDP: таблица Output всё равно показывает override/яркость.

ACL read: `get_stats`, `get_status` (позже ещё `get_config`).

## 1.7 `reload` / `set_brightness` / `get_config`

**Почему три метода вместе.** Settings через Save & Apply рестартит процесс (procd trigger) — для смены UDP-порта это нужно (сокет не перебиндить на лету осмысленно), для правки карты/сцен — нет: рвётся приём. Reload читает UCI в уже живущий демон. Ручная яркость из GUI не должна жить в UCI (это не настройка, а hold «пока не снимешь»). `get_config` — не зеркало файла (его уже отдаёт form.Map на Settings), а то, чем демон **пользуется сейчас**, чтобы проверить, что reload сработал.

Потоки: UDP `recv` и ubus/uloop разные. Доступ к политике, hold и диммеру сериализуется `UdpServer::mu_`. `LastStatus` со своим mutex; порядок всегда `UdpServer::mu_` → `LastStatus::mu_`, обратной захватки нет.

Полный ubus API:

```
ubus call light_control get_stats
ubus call light_control get_status
ubus call light_control get_config
ubus call light_control reload
ubus call light_control set_brightness '{"value": 40}'
ubus call light_control set_brightness '{"release": true}'
```

### Hold (`BrightnessOverride`)

Header-only, без heap и syslog. `set(n)` — active, clamp до 100; **0 валиден** (лента выключена вручную). `clear()` снимает флаг, значение не обнуляет. `apply(computed)` подменяет выход, если hold активен. Это не замена `Dimmer`: диммер по-прежнему единственный выход в syslog.

`LastStatus.brightness` — всегда то, что ушло бы на ленту, не «сырой» результат кривой. Пока hold включён, пакеты обновляют lux/device/scene/source, яркость остаётся N. `set_manual` трогает только яркость и флаг, `has_packet` не поднимает — hold до первого пакета возможен.

Reload при hold **не** срывает ручную яркость (`reapply_output_locked`).

`set_brightness`: `release: true` побеждает даже рядом с `value`; пустой вызов — `UBUS_STATUS_INVALID_ARGUMENT`; `value < 0` — ошибка; `> 100` — clamp 100.

### Reload без рестарта

Сразу подменяются unnamed/named `map`, `config scene`, поля `enabled`/`port`/`interface` в живом снимке.

Не делается: перебинд сокета (если UCI-port ≠ bound — syslog «restart to apply»); убийство процесса при `enabled=0` (он уже запущен); сброс статистики и `has_packet`; снятие hold.

На хосте без `OPENWRT_BUILD` libuci нет — reload ставит **дефолты**. Кнопка **Reload UCI** на Status — только этот метод. Save & Apply на Settings по-прежнему рестартит.

### Живой `get_config`

`format_hhmm` рядом с `parse_hhmm`: минуты → `"HH:MM"` для ответа как в UCI. Сериализация: unnamed пороги без поля `"set"`, named — с ним; min/max у сцены только если флаги заданы (нет поля ≠ «0»). `port` — последнее из UCI, `bound_port` — реальный сокет. `override_brightness` есть всегда (0, если hold никогда не ставили).

### LuCI Status (1.0.23)

Четыре блока: Statistics (`get_stats`), Output (`get_status`), Live config (`get_config`) — опрос раз в 3 с; Manual brightness — input + Hold / Release / Reload UCI **вне** `#light-control-stats`, иначе poll затирал бы ввод.

Два `rpc.declare` на один метод `set_brightness` с разными `params` (`['value']` и `['release']`), чтобы LuCI не слал `value: undefined` вместе с `release`.

ACL: read `get_stats`/`get_status`/`get_config`; write `reload`/`set_brightness`; UCI `light_control` read+write. Без write-ACL кнопки из LuCI под обычным пользователем не пройдут rpcd.

Хостовая сборка `ENABLE_UBUS=OFF` подставляет пустой stub `UbusExporter` — новых методов на WSL в демоне нет; их проверяет ipk-сборка и selftest логики.

## 1.8 Что на роутере в коде vs конфиг

| Параметр | Где | Статус |
|---|---|---|
| UDP port | UCI `option port` | Читается libuci, есть в Settings |
| enabled | UCI `option enabled` | Читается, init + демон, есть в Settings |
| interface | UCI `option interface` | Хранится и показывается, **на bind не влияет** |
| Bind-адрес | Код `0.0.0.0` | Хардкод (хвост — bind на интерфейс, см. PLAN) |
| Кривая lux→яркость | UCI `config map` | Читается, таблица в Settings, дефолт 200/400/65535 |
| Сцены | UCI `config scene` | Читается, форма в Settings, дефолт 06–11 / 11–18 / 18–06 |
| Диммер | `Dimmer::set_brightness` | лог-заглушка; драйвер ленты — остаток |
| Ручной hold | ubus `set_brightness` | runtime, не UCI |
| Часы сцен | `localtime_r` | NTP на устройстве — с железом |
| Разбор пакета | 3 байта lux BE | Хардкод протокола (так и задумано) |
| ubus object/methods | `light_control` | Хардкод (так и должно быть) |

## 1.9 Как проверяли без роутера

[`tests/selftest.cpp`](light_control/tests/selftest.cpp) → `light_control/build_wsl/light_control_selftest`. ubus и syslog-диммер в тест не входят. WSL, g++ 16 / cmake micromamba `cpp`: **all checks passed**.

Поверх протокола и дефолтной кривой: разбор `HH:MM`, интервалы включая night через полночь, clamp night, named set `daylight`, `format_hhmm`, `BrightnessOverride` (в т.ч. ручной 0, clamp 150→100, hold не срывается сменой кривой).

Хостовой демон (`ENABLE_UBUS=OFF`): `light_control/build_wsl/light_control`. ipk 1.0.23 собран скриптом сборки, на устройство не ставился.

Повторить:

```bash
export MAMBA_ROOT_PREFIX=$HOME/micromamba
~/bin/micromamba run -n cpp cmake --build /mnt/c/Dev/light_control/light/light_control/build_wsl
/mnt/c/Dev/light_control/light/light_control/build_wsl/light_control_selftest
```

Пересборка ipk (без установки):

```bash
export PATH="$HOME/openwrt/host-tools/bin:/usr/bin:/bin"
export PYTHONPATH="$HOME/openwrt/pyshim"
/mnt/c/Dev/light_control/light/light_control/openwrt_light_control_build.sh
```

Локальная сборка демона без SDK (тот же micromamba `cpp`): `light_control/build_wsl/`.

---

# Часть 2. ESP32 (`light_sensor`)

Плата к нам не приедет. Пишем прошивку под конкретные модели клиента; **собираем мы, льёт он** — после правки `config.hpp`. Контроллер 1.0.23 в сенсорном шаге **не менялся**: тот же UDP, кривая/сцены/диммер на роутере. Менялась только прошивка узла `light_sensor`. Хостовые TEST/LINUX после правок пересобраны и линкуются.

## 2.1 Зачем три контура и «тупой» сенсор

Сенсор не считает яркость — только lux. Иначе снова нельзя крутить кривую на роутере, и прошивка должна знать сцены/время. Узел маленький: I2C → 3 байта → UDP раз в секунду.

Платформа **не** выбирается в `config.hpp` (там нельзя `#define PLATFORM_*`) — только флагами сборки. Один репозиторий, два приложения, три способа получить бинарь сенсора:

| Контур | Кто | Чем | Датчик | Транспорт |
|---|---|---|---|---|
| **TEST** | мы, без железа | хостовый CMake, `PLATFORM_TEST=ON` | `Stub(Evening)` = 220 lux | `NullTransport` |
| **LINUX** | мы или localhost-петля | CMake, `PLATFORM_LINUX=ON` | тот же Stub | UDP POSIX |
| **ESP32** | мы собираем, **клиент** льёт | ESP-IDF **5.2+** (у нас **v5.5.5**), `idf.py` в `light_sensor/esp32/` | `Bh1750` по I2C | Wi-Fi STA + UDP |

Хостовый CMake умеет **только** LINUX и TEST. `-DPLATFORM_ESP32=ON` — `FATAL_ERROR` с подсказкой идти в `esp32/`. Раньше флаг притворялся desktop-CMake и не собирал IDF.

ESP32-обёртка **не дублирует** исходники: [`esp32/main/CMakeLists.txt`](light_sensor/esp32/main/CMakeLists.txt) компилирует файлы из `../src`, `PLATFORM_ESP32=1`, компоненты `nvs_flash`, `esp_wifi`, `esp_event`, `esp_netif`, `esp_driver_i2c`. Точка входа на чипе — `app_main`, не `int main`.

На ESP32 в прошивку не попадают `stub.cpp` и `udp_posix.cpp`. На LINUX/TEST не попадает `wifi_esp32.cpp`; `bh1750.cpp` компилируется, но ветка I2C выключена (`#else` → 250 lux). В `main` на хосте датчик всё равно `Stub`, не `Bh1750`.

## 2.2 Цикл узла и протокол

`TransportWrapper` — тонкая оболочка без heap. Конструкторы разные: ESP32 берёт SSID/пароль/IP/порт, LINUX — IP/порт, TEST — пустой.

```
init transport
init sensor (адрес BH1750_I2C_ADDR)
loop:
  lux = readLux()          # <1 → в пакет уходит 0
  clamp 0…65535
  packet = [id][lux>>8][lux&0xFF]
  send
  delay SAMPLE_PERIOD_MS   # 1000 мс
```

На ESP32 ошибка `init` **не завершает** процесс: возвращать из `app_main` бессмысленно — лог + вечный delay 1 с, чтобы это было видно в `idf.py monitor`. На LINUX/TEST — `return 1`.

UDP с шага протокола на контроллере не менялся. Старая 2-байтная прошивка с контроллером 1.0.23 несовместима.

## 2.3 BH1750FVI на I2C

**Почему этот драйвер.** Клиент прислал именно BH1750FVI (GY-302) и классический ESP32 (не S2/S3/C3). API — новый master `driver/i2c_master.h` (IDF ≥ 5.2); IDF 4.x / Arduino / PlatformIO этот `CMakeLists` не возьмут. Шина `I2C_NUM_0`, 100 кГц, внутренние подтяжки включены (на GY-302 обычно ещё свои 4.7 кΩ).

Порядок `init`: новая шина → `i2c_master_probe` (нет ACK → false, в логе адрес и пины) → add device → Power On `0x01`, Reset `0x07`, Continuous H-resolution `0x10` (1 lx, ~120 мс) → пауза 180 мс.

`readLux()`: 2 байта MSB-first, `lux = raw * 5 / 6` (даташит: raw / 1.2), **без float**. Ошибка чтения → `-1` → в UDP уходит `0`.

Дефолтная разводка — оба GPIO есть на **30-pin** WROOM-32; GPIO 6–11 заняты flash и на гребенку не выведены:

| BH1750 | ESP32 | Комментарий |
|---|---|---|
| VCC | **3V3** | Не 5 V |
| GND | GND | |
| SDA | GPIO **21** | `I2C_SDA_GPIO` |
| SCL | GPIO **22** | `I2C_SCL_GPIO` |
| ADDR | GND | адрес **0x23**; на VCC → **0x5C** |

На хосте тот же `bh1750.cpp` остаётся заглушкой `readLux()=250`.

## 2.4 Wi-Fi STA и UDP

**Почему переписали транспорт.** Раньше не хватало netif/event loop, `set_mode`, `connect`, ожидания IP; вызывался несуществующий `wifi_is_connected()`, а `main` создавал транспорт одним портом. Без полного STA прошивка не получит адрес и не дойдёт до роутера.

**Что сделано** в [`wifi_esp32.cpp`](light_sensor/src/transports/wifi_esp32.cpp):

1. Отказ, если SSID/IP пустые или оставлены плейсхолдеры `YOUR_SSID` / `YOUR_PASSWORD`. Прошивка без правки конфига **намеренно не поднимает Wi-Fi** — иначе уйдёт в чужую сеть или зависнет на пустом SSID без понятного лога.
2. NVS (`nvs_flash_erase` при `NO_FREE_PAGES` / `NEW_VERSION`).
3. `esp_netif_init` + default event loop + default STA netif.
4. `esp_wifi_init` / `WIFI_MODE_STA` / `set_config` / `start`.
5. По `STA_START` — `esp_wifi_connect`; до 20 повторных попыток; ждать `IP_EVENT_STA_GOT_IP` до 30 с.
6. UDP socket, `sendto` на `CONTROLLER_IP:CONTROLLER_PORT`.

Пароль непустой → минимум WPA2-PSK. Пустой `WIFI_PASS` (`""`) → открытая сеть. После обрыва Wi-Fi обработчик снова вызывает `connect`. Неудачный `sendto` только предупреждение в лог: цикл измерений не останавливается (датчик жив, сеть может вернуться).

## 2.5 Compile-time конфиг

[`include/config.hpp`](light_sensor/include/config.hpp). Runtime-конфига на ESP32 нет: сменить SSID/пины = правка + пересборка + прошивка. Для маленького узла без экрана это проще NVS-UI; плейсхолдеры всё равно обязан прописать клиент.

| Константа | Сейчас | Зачем |
|---|---|---|
| `DEVICE_ID` | `1` | поле в 3-байтном пакете; несколько узлов — разные id |
| `CONTROLLER_IP` | `"192.168.1.1"` | LAN-адрес роутера с демоном; для хостовой петли временно `127.0.0.1` |
| `CONTROLLER_PORT` | `5005` | должен совпасть с UCI `option port` |
| `SAMPLE_PERIOD_MS` | `1000` | период UDP; диммер на роутере глушит повтор того же N |
| `BH1750_I2C_ADDR` | `0x23` | `0x5C`, если ADDR на VCC |
| `WIFI_SSID` / `WIFI_PASS` | **`YOUR_SSID` / `YOUR_PASSWORD`** | **обязательно заменить** перед прошивкой; иначе Wi-Fi не поднимется |
| `I2C_SDA_GPIO` / `I2C_SCL_GPIO` | 21 / 22 | типичный 30-pin; клиент меняет, если разводка другая |
| `I2C_HZ` | 100000 | стандарт для BH1750 |

На LINUX `WIFI_*` и пины I2C не компилируются. Пороги lux→dim с сенсора **сняты** — они на контроллере.

`sdkconfig.defaults`: flash **4 MB**, CPU **240 МГц**, UART **115200**, BT выключен, стек `app_main` **8 КБ**, лог INFO. Артефакты IDF (`esp32/build/`, `sdkconfig`, `managed_components/`) в `.gitignore`.

## 2.6 Что на сенсоре в коде vs конфиг

| Параметр | Где | Статус |
|---|---|---|
| DEVICE_ID, IP/порт, Wi-Fi, I2C, адрес, период | `config.hpp` | Хардкод compile-time |
| Пороги lux→dim | — | **Перенесены на контроллер** |
| Формат пакета | `main.cpp`: 3 байта lux BE | Хардкод протокола |
| Сценарий stub | `Stub(Evening)` на Linux/TEST | Хардкод тестовых люксов |
| Датчик | ESP32 → `Bh1750`, иначе Stub | Сборка |
| Платформа | CMake `PLATFORM_*` / ESP-IDF | Сборка, не runtime |

## 2.7 Как проверяли без платы

TEST и LINUX пересобраны в `light_sensor/build_test/` и `build_linux/`. Документация сборки — [`light_sensor/README.md`](light_sensor/README.md).

```bash
export MAMBA_ROOT_PREFIX=$HOME/micromamba
~/bin/micromamba run -n cpp cmake --build /mnt/c/Dev/light_control/light/light_sensor/build_test
~/bin/micromamba run -n cpp cmake --build /mnt/c/Dev/light_control/light/light_sensor/build_linux
```

С нуля:

```bash
cmake -S light_sensor -B light_sensor/build_test  -DPLATFORM_TEST=ON  -DPLATFORM_LINUX=OFF
cmake -S light_sensor -B light_sensor/build_linux -DPLATFORM_LINUX=ON -DPLATFORM_TEST=OFF
```

Чеклист прошивки у клиента, типичные ошибки monitor и хвосты без схемы — в [`PLAN.md`](PLAN.md).

## 2.8 Сборка прошивки ESP32 (WSL + ESP-IDF)

Раньше в WSL не было ESP-IDF: системный `python3-venv` / cmake / ninja не стояли, `sudo apt` без пароля недоступен, на `C:` ~23 ГБ свободно. IDF и toolchain поставлены **на диск Linux** (`~/esp`, `~/.espressif`), не на `/mnt/c`.

**Что поставлено (локально, не в git):**

| Что | Куда |
|---|---|
| micromamba env `espidf` | Python 3.11, CMake 3.31, GCC 13, ninja (env `cpp` с CMake 4 / Python 3.14 IDF не берёт) |
| ESP-IDF **v5.5.5** | `$HOME/esp/esp-idf` |
| Xtensa toolchain | `$HOME/.espressif` (`xtensa-esp-elf` 14.2) |

**Сборка прошла.** Цель `esp32` (WROOM-32 / D0WD-V3). Обёртка `light_sensor/esp32/`, исходники из `../src`. Каталог сборки `$HOME/esp/light_sensor_build` (ext4). Повтор:

```bash
/mnt/c/Dev/light_control/light/light_sensor/esp32/build_wsl.sh
```

Первая попытка упала на `wifi_esp32.cpp`: `sta.scan_method` в IDF 5.5 ждёт `wifi_scan_method_t` (`WIFI_FAST_SCAN`), стояло `WIFI_SCAN_TYPE_ACTIVE`. Исправлено.

Клиент собирает на **IDF 6.0.2**: без `-D_GNU_SOURCE` падает сам `esp_libc/realpath.c` (`strchrnul` спрятан в newlib). В [`esp32/CMakeLists.txt`](light_sensor/esp32/CMakeLists.txt) флаг добавлен глобально; на 5.x безвреден.

Размеры образа: bootloader 26 КБ @ `0x1000`, таблица разделов 3 КБ @ `0x8000`, приложение **704 КБ** @ `0x10000` (слот 1 МБ, ~31% свободно). Flash 4 MB, 40 МГц, DIO. `.bin` в git **не** кладём: в него запечены плейсхолдеры Wi-Fi.

> **Ещё раз: перед `flash` правь [`include/config.hpp`](light_sensor/include/config.hpp) (`WIFI_SSID` / `WIFI_PASS` / при необходимости `CONTROLLER_IP`) и пересобери.**
> Образ с `YOUR_SSID` на живой плате бесполезен. Пины I2C (21/22) и адрес `0x23` — только если разводка другая.
