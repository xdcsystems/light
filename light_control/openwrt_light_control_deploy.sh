#!/usr/bin/env bash
set -euo pipefail

# --- НАСТРОЙКИ ---
ROUTER_IP="${ROUTER_IP:-192.168.1.1}"
ROUTER_USER="${ROUTER_USER:-root}"

BASE_DIR="${OPENWRT_SDK:-$HOME/openwrt/sdk}"
IPK_DIR="${BASE_DIR}/bin/packages/mips_24kc"

if [[ ! -d "$BASE_DIR" ]]; then
  echo "Ошибка: SDK не найден: $BASE_DIR" >&2
  exit 1
fi

DAEMON_IPK="$(find "$IPK_DIR" -name 'light_control_*.ipk' | sort | tail -n 1 || true)"
LUCI_IPK="$(find "$IPK_DIR" -name 'luci-app-light-control_*.ipk' | sort | tail -n 1 || true)"

if [[ -z "$DAEMON_IPK" ]]; then
  echo "Ошибка: light_control_*.ipk не найден в $IPK_DIR" >&2
  echo "Сначала запусти сборку: ./openwrt_light_control_build.sh" >&2
  exit 1
fi

echo "=== Деплой на роутер ($ROUTER_IP) ==="
echo "Демон: $DAEMON_IPK"
[[ -n "$LUCI_IPK" ]] && echo "LuCI:  $LUCI_IPK"

REMOTE_FILES=("$(basename "$DAEMON_IPK")")
tar czf - -C "$(dirname "$DAEMON_IPK")" "$(basename "$DAEMON_IPK")" \
  ${LUCI_IPK:+-C "$(dirname "$LUCI_IPK")" "$(basename "$LUCI_IPK")"} \
  | ssh "${ROUTER_USER}@${ROUTER_IP}" 'cat > /tmp/light_control.tar.gz'

INSTALL_CMD="cd /tmp && tar xzf light_control.tar.gz && opkg install --force-reinstall $(basename "$DAEMON_IPK")"
if [[ -n "$LUCI_IPK" ]]; then
  INSTALL_CMD="$INSTALL_CMD $(basename "$LUCI_IPK")"
  REMOTE_FILES+=("$(basename "$LUCI_IPK")")
fi
INSTALL_CMD="$INSTALL_CMD && /etc/init.d/light_control restart && /etc/init.d/rpcd restart"

ssh "${ROUTER_USER}@${ROUTER_IP}" "$INSTALL_CMD"

echo "=== Готово ==="
