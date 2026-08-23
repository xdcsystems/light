#!/usr/bin/env bash
set -euo pipefail

# --- НАСТРОЙКИ ---
ROUTER_IP="${ROUTER_IP:-192.168.1.1}"
ROUTER_USER="${ROUTER_USER:-root}"

BASE_DIR="${OPENWRT_SDK:-$HOME/Projects/light/light_control/externals/openwrt}"
IPK_DIR="${BASE_DIR}/bin/packages/mips_24kc"

# --- ПРОВЕРКИ ---
if [[ ! -d "$BASE_DIR" ]]; then
  echo "Ошибка: папка сборки не найдена: $BASE_DIR" >&2
  exit 1
fi

DAEMON_IPK="$(find "$IPK_DIR" -name 'light_control_*.ipk' | sort | tail -n 1 || true)"
LUCI_IPK="$(find "$IPK_DIR" -name 'luci-app-light-control_*.ipk' | sort | tail -n 1 || true)"

if [[ -z "$DAEMON_IPK" ]]; then
  echo "Ошибка: .ipk не найден: $IPK_DIR/light_control_*.ipk" >&2
  echo "Сначала запусти сборку: ./openwrt_light_control_build.sh" >&2
  exit 1
fi
if [[ -z "$LUCI_IPK" ]]; then
  echo "Ошибка: .ipk не найден: $IPK_DIR/luci-app-light-control_*.ipk" >&2
  echo "Сначала запусти сборку: ./openwrt_light_control_build.sh" >&2
  exit 1
fi

echo "=== Деплой пакета на роутер ($ROUTER_IP) ==="
echo "Демон: $(ls -lh "$DAEMON_IPK")"
echo "LuCI:  $(ls -lh "$LUCI_IPK")"

DAEMON_NAME="$(basename "$DAEMON_IPK")"
LUCI_NAME="$(basename "$LUCI_IPK")"

# --- ПЕРЕДАЧА (tar | ssh) ---
tar czf - -C "$(dirname "$DAEMON_IPK")" "$DAEMON_NAME" \
  -C "$(dirname "$LUCI_IPK")" "$LUCI_NAME" \
  | ssh "${ROUTER_USER}@${ROUTER_IP}" 'cat > /tmp/light_control.tar.gz'

# --- РАСПАКОВКА И УСТАНОВКА ---
ssh "${ROUTER_USER}@${ROUTER_IP}" \
  "cd /tmp && tar xzf light_control.tar.gz && opkg install --force-reinstall ${DAEMON_NAME} ${LUCI_NAME}"

echo "=== Перезапуск сервиса light_control ==="
ssh "${ROUTER_USER}@${ROUTER_IP}" "/etc/init.d/light_control restart && /etc/init.d/rpcd restart"

#echo "=== Проверка бинарника (размер и дата) ==="
#ssh "${ROUTER_USER}@${ROUTER_IP}" "ls -l /usr/bin/light_control"

#echo "=== Быстрая проверка ELF-заголовка (убеждаемся, что это Linux-бинарник) ==="
# Если head и hexdump есть — покажем первые байты. Если нет — просто пропустим без ошибки.
#if ssh "${ROUTER_USER}@${ROUTER_IP}" command -v head >/dev/null 2>&1 && \
#   ssh "${ROUTER_USER}@${ROUTER_IP}" command -v hexdump >/dev/null 2>&1; then
#  ssh "${ROUTER_USER}@${ROUTER_IP}" "head -c 16 /usr/bin/light_control | hexdump -C"
#else
#  echo "(head/hexdump отсутствуют на роутере — пропускаем проверку заголовка)"
#fi

#echo "=== Последние логи (поиск light_control) ==="
#ssh -t "${ROUTER_USER}@${ROUTER_IP}" "logread | grep -i light_control | tail -n 20"

echo "=== Готово ==="
