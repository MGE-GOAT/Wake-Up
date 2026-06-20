#!/usr/bin/env bash
# Wi-Fi provisioning helper. Invoked by the C++ pipeline binary when the
# physical button on the device is pressed.
#
# Flow:
#   1. Remember the currently-active Wi-Fi connection (so we can restore it
#      if the user cancels).
#   2. Bring up wlan0 as an open Access Point named "ElderlyCare_<DEVICE_ID>".
#   3. Launch the Python provisioning HTTP server (wifi_setup_server.py)
#      on 192.168.4.1:8080. It blocks until the user POSTs valid credentials
#      and the device successfully joins the chosen network.
#   4. Tear down the AP and exit.
#
# Tested on: Raspberry Pi OS Bookworm (uses NetworkManager / nmcli).
# Bullseye/legacy images that use dhcpcd+wpa_supplicant need a different
# approach; see README for the legacy variant.
#
# Returns 0 on successful provisioning, 1 on failure/timeout.

set -uo pipefail

DEVICE_ID="${DEVICE_ID:-ID_12233945}"
AP_NAME="ElderlyCare_${DEVICE_ID#ID_}"          # e.g. ElderlyCare_12233945
AP_PASSWORD=""                                  # empty = open AP (simpler UX)
AP_IFACE="wlan0"
AP_GATEWAY="192.168.4.1"
AP_CONN_NAME="elderly-care-setup"               # name for nmcli to manage
SETUP_TIMEOUT_SEC="${SETUP_TIMEOUT_SEC:-600}"   # 10 min — plenty for a human

HERE="$(cd "$(dirname "$0")" && pwd)"
SERVER_SCRIPT="${HERE}/wifi_setup_server.py"
DONE_FLAG="/tmp/wifi_setup_done"

log() { echo "[wifi-setup] $*" >&2; }

cleanup() {
    log "Cleaning up AP and restoring previous connection..."
    sudo nmcli con down "${AP_CONN_NAME}" 2>/dev/null || true
    sudo nmcli con delete "${AP_CONN_NAME}" 2>/dev/null || true
    if [ -n "${PREV_CONN:-}" ] && [ "${PREV_CONN}" != "${AP_CONN_NAME}" ]; then
        sudo nmcli con up "${PREV_CONN}" 2>/dev/null || true
    fi
    rm -f "${DONE_FLAG}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Capture the currently-active Wi-Fi (so we can restore on cancel/timeout)
PREV_CONN=$(nmcli -t -f NAME,TYPE,DEVICE con show --active \
            | awk -F: -v ifc="${AP_IFACE}" '$2=="802-11-wireless" && $3==ifc {print $1; exit}')
log "Previous active Wi-Fi connection: ${PREV_CONN:-<none>}"

# Bring up AP via NetworkManager. We use `nmcli` so the connection appears as
# a manageable profile and a single `nmcli con down` cleans up dnsmasq, DHCP
# server, hostapd-equivalent, and IP forwarding all at once.
log "Bringing up AP '${AP_NAME}' on ${AP_IFACE}..."
sudo nmcli con add type wifi ifname "${AP_IFACE}" con-name "${AP_CONN_NAME}" \
    autoconnect no ssid "${AP_NAME}"
sudo nmcli con modify "${AP_CONN_NAME}" \
    802-11-wireless.mode ap 802-11-wireless.band bg \
    ipv4.method shared ipv4.addresses "${AP_GATEWAY}/24"
if [ -n "${AP_PASSWORD}" ]; then
    sudo nmcli con modify "${AP_CONN_NAME}" \
        wifi-sec.key-mgmt wpa-psk wifi-sec.psk "${AP_PASSWORD}"
fi
sudo nmcli con up "${AP_CONN_NAME}" || { log "AP up failed"; exit 1; }

# Spawn the HTTP provisioning server. It listens on 192.168.4.1:8080 and
# writes ${DONE_FLAG} when the user successfully connects to a network.
log "Starting provisioning HTTP server on ${AP_GATEWAY}:8080..."
rm -f "${DONE_FLAG}"
python3 "${SERVER_SCRIPT}" \
    --bind "${AP_GATEWAY}" --port 8080 \
    --done-flag "${DONE_FLAG}" --iface "${AP_IFACE}" &
SERVER_PID=$!

# Wait for the done flag (or timeout).
ELAPSED=0
while [ ! -f "${DONE_FLAG}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    if [ "${ELAPSED}" -ge "${SETUP_TIMEOUT_SEC}" ]; then
        log "Setup timed out after ${SETUP_TIMEOUT_SEC}s"
        kill "${SERVER_PID}" 2>/dev/null || true
        exit 1
    fi
done

# Graceful shutdown of the server (it should exit on its own after writing
# the flag, but force-kill if it lingers).
sleep 1
kill "${SERVER_PID}" 2>/dev/null || true

if [ -f "${DONE_FLAG}" ]; then
    log "Provisioning successful. Cleaning up AP."
    exit 0
fi
log "Provisioning failed or cancelled."
exit 1
