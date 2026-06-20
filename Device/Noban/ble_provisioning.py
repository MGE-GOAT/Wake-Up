"""Phase 7 — BLE Wi-Fi + identity provisioning sidecar.

Spawned by main.cpp's ButtonGesture::ENTER_SETUP path when the user holds
the button for 3-10 seconds. Replaces the legacy AP-mode `wifi_setup.sh`
(which forced the user's phone to disconnect from their LAN to hit the
device at 192.168.4.1:8080 — terrible UX).

Flow:
    1. Advertise a GATT service so the companion app (flutter_blue_plus)
       can find us under "ElderlyCare-Setup".
    2. App connects + pairs (Just Works — anyone within BLE range during
       the setup window is implicitly trusted; window is short so the
       attack surface is narrow).
    3. App calls `wifi_scan` (READ) → we run `nmcli dev wifi list`, return
       JSON list of {ssid, signal, security}.
    4. App writes `wifi_join` with chosen network → we run
       `nmcli dev wifi connect`, notify `status` with the result.
    5. App writes `commit_setup` with the user-picked device_id +
       32-char password → we write /var/lib/elderly-care/id.txt, notify
       `done`, exit cleanly.

Topology choice — why a separate Python sidecar instead of doing BLE in
C++ directly: BlueZ's GATT API is DBus-based, and the Python `bless`
library wraps it in a tiny async loop. The C++ equivalent (bluetooth-dev
+ glib + dbus) is 10× the code and a build-time dependency we don't need
99% of the time (device only runs BLE during the ~3-minute setup window).
Sidecar is spawned + killed by main.cpp; no process running otherwise.

Window timing:
    - Sidecar exits after 5 minutes if no client connects (Window A).
    - Once a client is connected, no timeout (Window B) — user can take
      as long as they want to type their Wi-Fi password.
    - Sidecar exits voluntarily after commit_setup completes.
    - main.cpp can also SIGTERM the sidecar process at any time.

Reads/writes id.txt at /var/lib/elderly-care/id.txt (production) or
$ELDERLY_ID_FILE (override for testing). Same path that main.cpp's
loadDeviceIdentity() reads from.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import subprocess
import sys
from pathlib import Path
from typing import Any, Optional

try:
    from bless import (
        BlessServer,
        BlessGATTCharacteristic,
        GATTCharacteristicProperties,
        GATTAttributePermissions,
    )
except ImportError:
    print("[ble] FATAL: `bless` not installed. Run: pip install bless", file=sys.stderr)
    sys.exit(2)


log = logging.getLogger("ble_provisioning")
logging.basicConfig(
    level=logging.INFO,
    format="[ble] %(asctime)s %(levelname)s %(message)s",
)

# ── UUIDs ──────────────────────────────────────────────────────────────────
# Custom 128-bit base; characteristics use sequential suffixes. Companion
# app's BleProvisioningService must use these exact strings.
SVC_UUID         = "11111111-2222-3333-4444-555555555555"
CHAR_WIFI_SCAN   = "11111111-2222-3333-4444-555555555001"  # READ
CHAR_WIFI_JOIN   = "11111111-2222-3333-4444-555555555002"  # WRITE
CHAR_STATUS      = "11111111-2222-3333-4444-555555555003"  # NOTIFY
CHAR_COMMIT      = "11111111-2222-3333-4444-555555555004"  # WRITE
CHAR_DONE        = "11111111-2222-3333-4444-555555555005"  # NOTIFY

def _factory_serial_suffix() -> str:
    """A short factory-unique tag so multiple un-provisioned devices in range
    are distinguishable in the app's picker. Uses the last 4 hex of the Pi's
    CPU serial (stable per board, available before provisioning). Falls back
    to the BT MAC suffix, then a random tag."""
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("Serial"):
                s = line.split(":")[-1].strip()
                if len(s) >= 4:
                    return s[-4:].upper()
    except Exception:
        pass
    try:
        import uuid
        return f"{uuid.getnode() & 0xFFFF:04X}"
    except Exception:
        return "0000"

# Advertised as "Noban-Setup-<serial>" so the app can list + pick a specific
# device when several are advertising at once. The service UUID (below) is
# what the app filters on; the suffix is just the human label. Must match the
# app's devicePrefix in ble_provisioning.dart.
DEVICE_NAME      = "Noban-Setup-" + _factory_serial_suffix()

# Where id.txt lives. Must match loadDeviceIdentity() in main.cpp.
ID_FILE = Path(os.environ.get(
    "ELDERLY_ID_FILE", "/var/lib/elderly-care/id.txt"))

# How long to wait for an app to connect before giving up.
WINDOW_A_SECONDS = 5 * 60

# Module-level state. Bless's read/write callbacks are sync (called by
# the dbus loop) but our nmcli ops are blocking, so we shell out and let
# them run in an executor.
_server: Optional[BlessServer] = None
_status_value = b"idle"
_done_value   = b""
_client_connected = False
_setup_completed  = False
_loop: Optional[asyncio.AbstractEventLoop] = None


# ── Helpers ────────────────────────────────────────────────────────────────

def _run_nmcli(*args: str, timeout: int = 30) -> tuple[int, str, str]:
    """Run nmcli, return (rc, stdout, stderr). Doesn't raise on non-zero.

    Runs via `sudo -n` because creating/activating system connection profiles
    (`connection add/up/delete`) needs root — the sidecar runs as the
    unprivileged pipeline user, which otherwise hits NetworkManager's
    'Insufficient privileges'. The pi user has passwordless sudo on Pi OS;
    setup.sh also installs a NOPASSWD rule scoped to /usr/bin/nmcli. If sudo
    is unavailable we fall back to a bare nmcli (works for read-only ops)."""
    try:
        r = subprocess.run(
            ["sudo", "-n", "nmcli", *args],
            capture_output=True, text=True, timeout=timeout,
        )
        # If sudo itself failed (no privilege), retry without it so read-only
        # commands (dev wifi list) still work.
        if r.returncode != 0 and "sudo:" in (r.stderr or ""):
            r = subprocess.run(["nmcli", *args],
                               capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"
    except FileNotFoundError:
        return 127, "", "nmcli not installed"


def _scan_wifi() -> list[dict]:
    """Return list of {ssid, signal, security} for nearby networks. Filters
    duplicates (NM lists each BSSID separately; we keep the strongest)."""
    # --rescan no: return NetworkManager's CACHED scan (instant). A fresh
    # "--rescan yes" blocks for up to ~20 s, which stalls this synchronous BLE
    # read handler past the phone's read timeout → app sees an empty list. NM
    # rescans on its own every ~2 min, and the radio scans on connect, so the
    # cache is reliably populated. Worst case the user taps Scan again.
    rc, out, _err = _run_nmcli(
        "-t", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi", "list",
        "--rescan", "no", timeout=10,
    )
    if rc != 0:
        return []
    best: dict[str, dict] = {}
    for line in out.strip().splitlines():
        # nmcli -t format: SSID:SIGNAL:SECURITY. NM escapes colons in SSID
        # with backslash — split on un-escaped colon.
        parts: list[str] = []
        cur = ""
        i = 0
        while i < len(line):
            if line[i] == "\\" and i + 1 < len(line):
                cur += line[i + 1]; i += 2; continue
            if line[i] == ":":
                parts.append(cur); cur = ""; i += 1; continue
            cur += line[i]; i += 1
        parts.append(cur)
        if len(parts) < 3 or not parts[0]:
            continue
        ssid, signal_str, security = parts[0], parts[1], parts[2]
        try:
            signal = int(signal_str)
        except ValueError:
            continue
        if ssid not in best or signal > best[ssid]["signal"]:
            best[ssid] = {"ssid": ssid, "signal": signal, "security": security}
    return sorted(best.values(), key=lambda d: -d["signal"])


def _join_wifi(ssid: str, psk: str) -> tuple[bool, str]:
    """Attempt to connect. Returns (ok, error_message).

    We build the connection profile explicitly rather than relying on
    `nmcli dev wifi connect`, because that path reuses any pre-existing
    profile for the SSID — and a half-created/broken one (e.g. from a prior
    failed attempt) yields
    '802-11-wireless-security.key-mgmt: property is missing'. So: delete any
    stale profile for this SSID first, then create a fresh one with the
    security type pinned (wpa-psk), then bring it up."""
    # 1. Wipe any existing profile(s) with this con-name so we start clean.
    _run_nmcli("connection", "delete", ssid, timeout=15)

    if psk:
        # 2a. Secured network — create with key-mgmt explicitly set.
        rc, out, err = _run_nmcli(
            "connection", "add", "type", "wifi",
            "con-name", ssid, "ifname", "wlan0", "ssid", ssid,
            "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", psk,
            timeout=30,
        )
        if rc != 0:
            return False, (err.strip() or out.strip() or f"add rc={rc}")
    else:
        # 2b. Open network — no security node at all.
        rc, out, err = _run_nmcli(
            "connection", "add", "type", "wifi",
            "con-name", ssid, "ifname", "wlan0", "ssid", ssid,
            timeout=30,
        )
        if rc != 0:
            return False, (err.strip() or out.strip() or f"add rc={rc}")

    # 3. Activate it.
    rc, out, err = _run_nmcli("connection", "up", ssid, timeout=60)
    if rc == 0:
        return True, ""
    # Clean up the failed profile so the next attempt starts fresh.
    _run_nmcli("connection", "delete", ssid, timeout=15)
    return False, (err.strip() or out.strip() or f"up rc={rc}")


def _persist_id_txt(device_id: str, password: str) -> tuple[bool, str]:
    """Atomically write id.txt with the user-chosen credentials.

    Tries ID_FILE (default /var/lib/elderly-care/id.txt) first; if that path
    isn't writable (e.g. the dir is root-owned and we run as the pipeline
    user), falls back to ~/.config/elderly-care/id.txt — which main.cpp's
    loadDeviceIdentity() also checks. setup.sh chowns /var/lib/elderly-care
    to the runtime user so the primary path normally works."""
    candidates = [ID_FILE,
                  Path.home() / ".config" / "elderly-care" / "id.txt"]
    last_err = ""
    for path in candidates:
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_suffix(".tmp")
            tmp.write_text(f"{device_id}\n{password}\n")
            tmp.chmod(0o600)
            tmp.rename(path)
            log.info("id.txt written to %s", path)
            return True, ""
        except Exception as e:
            last_err = f"{type(e).__name__}: {e}"
            continue
    return False, last_err


# ── GATT callbacks ─────────────────────────────────────────────────────────
# Bless calls read/write handlers synchronously from the dbus thread. For
# anything that takes >100ms (nmcli scan, wifi connect) we hand off to the
# asyncio loop via call_soon_threadsafe + run_in_executor so the BLE thread
# isn't blocked.

def _on_read(char: BlessGATTCharacteristic, **kwargs) -> bytearray:
    uuid = char.uuid.lower()
    if uuid == CHAR_WIFI_SCAN.lower():
        # Synchronous — short enough (nmcli takes ~5 s); BLE peer waits.
        nets = _scan_wifi()
        return bytearray(json.dumps(nets, ensure_ascii=False).encode("utf-8"))
    if uuid == CHAR_STATUS.lower():
        return bytearray(_status_value)
    if uuid == CHAR_DONE.lower():
        return bytearray(_done_value)
    return bytearray()


def _notify_status(value: bytes) -> None:
    global _status_value
    _status_value = value
    log.info("status → %s", value.decode("utf-8", errors="replace"))
    try:
        _server.get_characteristic(CHAR_STATUS).value = bytearray(value)
        _server.update_value(SVC_UUID, CHAR_STATUS)
    except Exception as e:
        log.warning("status notify failed: %s", e)


def _notify_done(value: bytes) -> None:
    global _done_value, _setup_completed
    _done_value = value
    _setup_completed = True
    log.info("done → %s", value.decode("utf-8", errors="replace"))
    try:
        _server.get_characteristic(CHAR_DONE).value = bytearray(value)
        _server.update_value(SVC_UUID, CHAR_DONE)
    except Exception as e:
        log.warning("done notify failed: %s", e)


async def _handle_wifi_join(payload: bytes) -> None:
    try:
        body = json.loads(payload.decode("utf-8"))
    except Exception:
        _notify_status(b"err: bad json")
        return
    ssid = body.get("ssid")
    psk  = body.get("psk", "")
    if not ssid:
        _notify_status(b"err: ssid required")
        return
    _notify_status(b"connecting")
    ok, err = await asyncio.get_running_loop().run_in_executor(
        None, _join_wifi, ssid, psk
    )
    if ok:
        _notify_status(b"wifi_ok")
    else:
        _notify_status(("wifi_fail: " + err).encode("utf-8")[:200])


async def _handle_commit(payload: bytes) -> None:
    try:
        body = json.loads(payload.decode("utf-8"))
    except Exception:
        _notify_done(b"err: bad json")
        return
    device_id = body.get("device_id")
    password  = body.get("password")
    if not device_id or not password:
        _notify_done(b"err: device_id and password required")
        return
    if len(password) < 32:
        _notify_done(b"err: password must be >= 32 chars")
        return
    ok, err = await asyncio.get_running_loop().run_in_executor(
        None, _persist_id_txt, device_id, password
    )
    _notify_done(b"ok" if ok else ("err: " + err).encode("utf-8")[:200])


def _on_write(char: BlessGATTCharacteristic, value: bytearray, **kwargs) -> None:
    global _client_connected
    _client_connected = True   # write ⇒ a client is attached
    uuid = char.uuid.lower()
    log.info("write → %s (%d bytes)", uuid[-4:], len(value))
    if uuid == CHAR_WIFI_JOIN.lower():
        asyncio.run_coroutine_threadsafe(_handle_wifi_join(bytes(value)), _loop)
    elif uuid == CHAR_COMMIT.lower():
        asyncio.run_coroutine_threadsafe(_handle_commit(bytes(value)), _loop)
    else:
        log.warning("unknown write to %s", char.uuid)


# ── Main loop ──────────────────────────────────────────────────────────────

async def main() -> int:
    global _server, _loop
    _loop = asyncio.get_running_loop()
    _server = BlessServer(name=DEVICE_NAME, loop=_loop)
    _server.read_request_func  = _on_read
    _server.write_request_func = _on_write

    await _server.add_new_service(SVC_UUID)

    char_specs = [
        (CHAR_WIFI_SCAN, GATTCharacteristicProperties.read,
         GATTAttributePermissions.readable, b"[]"),
        (CHAR_WIFI_JOIN, GATTCharacteristicProperties.write,
         GATTAttributePermissions.writeable, b""),
        (CHAR_STATUS,
         GATTCharacteristicProperties.read | GATTCharacteristicProperties.notify,
         GATTAttributePermissions.readable, b"idle"),
        (CHAR_COMMIT, GATTCharacteristicProperties.write,
         GATTAttributePermissions.writeable, b""),
        (CHAR_DONE,
         GATTCharacteristicProperties.read | GATTCharacteristicProperties.notify,
         GATTAttributePermissions.readable, b""),
    ]
    for uuid, props, perms, initial in char_specs:
        await _server.add_new_characteristic(
            SVC_UUID, uuid, props, bytearray(initial), perms)

    await _server.start()
    log.info("advertising as %s", DEVICE_NAME)
    log.info("service UUID: %s", SVC_UUID)

    # Window A — exit if no client connects within 5 minutes.
    waited = 0.0
    while not _client_connected and waited < WINDOW_A_SECONDS:
        await asyncio.sleep(1)
        waited += 1.0
    if not _client_connected:
        log.info("no client in %d s — exiting", WINDOW_A_SECONDS)
        await _server.stop()
        return 1

    # Window B — stay alive until commit completes (or signal).
    log.info("client attached; waiting for commit_setup")
    while not _setup_completed:
        await asyncio.sleep(1)

    # Brief tail so the final `done` notification flushes.
    await asyncio.sleep(2)
    log.info("commit complete — shutting down")
    await _server.stop()
    return 0


def _signal_handler(signum, frame):  # noqa: ARG001
    global _setup_completed
    log.info("signal %d — exiting", signum)
    _setup_completed = True


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _signal_handler)
    signal.signal(signal.SIGINT,  _signal_handler)
    sys.exit(asyncio.run(main()))
