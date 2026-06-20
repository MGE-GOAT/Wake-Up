"""Wi-Fi provisioning HTTP server.

Runs on the Pi when wifi_setup.sh has brought up the AP. The Flutter app
connects to that AP and uses these two endpoints:

  GET  /scan                  -> {"networks": [{"ssid", "signal", "secure"}, ...]}
  POST /connect  {ssid,pwd}   -> 200 on success (device joined the chosen Wi-Fi),
                                 400 on bad creds, 500 on system error.

On a successful /connect the script writes ${DONE_FLAG} and exits, so
wifi_setup.sh can tear down the AP.

Uses Python's stdlib http.server — no Flask dependency. That keeps the Pi
image lean: only `python3` is required (no `pip install flask`).
"""
from __future__ import annotations

import argparse
import json
import logging
import shlex
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

logging.basicConfig(level=logging.INFO, format="[wifi-setup-server] %(message)s")
log = logging.getLogger(__name__)


def run(cmd: list[str], timeout: int = 30) -> subprocess.CompletedProcess:
    """Run a shell command and capture output. nmcli uses sudo here because
    the script is invoked from wifi_setup.sh which itself runs unprivileged
    (the user that owns the C++ pipeline). NetworkManager requires root for
    connection mutations."""
    log.info("$ %s", " ".join(shlex.quote(c) for c in cmd))
    return subprocess.run(
        cmd, capture_output=True, text=True, timeout=timeout, check=False
    )


def scan_networks(iface: str) -> list[dict]:
    """Return nearby Wi-Fi networks, dedup-by-SSID, signal-sorted descending."""
    # `nmcli dev wifi rescan` triggers a fresh scan (otherwise we'd get cached).
    run(["sudo", "nmcli", "dev", "wifi", "rescan", "ifname", iface])
    r = run([
        "nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY",
        "dev", "wifi", "list", "ifname", iface,
    ])
    if r.returncode != 0:
        log.warning("scan failed: %s", r.stderr)
        return []

    seen: dict[str, dict] = {}
    for line in r.stdout.splitlines():
        # nmcli -t separates fields with `:`. SSIDs may contain `:` which nmcli
        # escapes as `\:`. We split on un-escaped colons.
        parts: list[str] = []
        cur = ""
        i = 0
        while i < len(line):
            c = line[i]
            if c == "\\" and i + 1 < len(line):
                cur += line[i + 1]; i += 2; continue
            if c == ":":
                parts.append(cur); cur = ""; i += 1; continue
            cur += c; i += 1
        parts.append(cur)
        if len(parts) < 3 or not parts[0]:
            continue
        ssid, signal, security = parts[0], parts[1], parts[2]
        try:
            signal_int = int(signal)
        except ValueError:
            signal_int = 0
        entry = {
            "ssid": ssid,
            "signal": signal_int,
            "secure": security not in ("", "--"),
        }
        # Dedup: keep strongest signal per SSID.
        if ssid not in seen or seen[ssid]["signal"] < signal_int:
            seen[ssid] = entry
    return sorted(seen.values(), key=lambda x: -x["signal"])


def connect_network(iface: str, ssid: str, password: str) -> tuple[bool, str]:
    """Attempt to join `ssid`. nmcli handles both open and WPA networks via
    the `password` arg (empty string for open)."""
    cmd = ["sudo", "nmcli", "dev", "wifi", "connect", ssid, "ifname", iface]
    if password:
        cmd += ["password", password]
    r = run(cmd, timeout=45)
    if r.returncode == 0:
        return True, r.stdout.strip()
    return False, (r.stderr.strip() or r.stdout.strip())


class Handler(BaseHTTPRequestHandler):
    iface: str = "wlan0"
    done_flag: str = "/tmp/wifi_setup_done"

    def _json(self, code: int, body: dict) -> None:
        payload = json.dumps(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        # CORS — the Flutter app might be hitting us from any origin.
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):  # quieter than default
        log.info("%s - %s", self.address_string(), fmt % args)

    def do_OPTIONS(self) -> None:  # noqa: N802 (BaseHTTPRequestHandler API)
        self._json(200, {})

    def do_GET(self) -> None:  # noqa: N802
        if self.path.rstrip("/") == "/scan":
            self._json(200, {"networks": scan_networks(self.iface)})
            return
        if self.path.rstrip("/") in ("/", "/status"):
            self._json(200, {"status": "ready"})
            return
        self._json(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        if self.path.rstrip("/") != "/connect":
            self._json(404, {"error": "not found"})
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self._json(400, {"error": "invalid JSON"})
            return
        ssid = (body.get("ssid") or "").strip()
        password = body.get("password") or ""
        if not ssid:
            self._json(400, {"error": "ssid is required"})
            return

        ok, msg = connect_network(self.iface, ssid, password)
        if ok:
            self._json(200, {"status": "connected", "detail": msg})
            # Tell wifi_setup.sh we're done. Shut the HTTP server down on a
            # background thread so the response can flush first.
            Path(self.done_flag).touch()
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        self._json(400, {"error": "connect failed", "detail": msg})


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--iface", default="wlan0")
    p.add_argument("--done-flag", default="/tmp/wifi_setup_done")
    args = p.parse_args()

    Handler.iface = args.iface
    Handler.done_flag = args.done_flag

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    log.info("Serving on http://%s:%d (iface=%s)", args.bind, args.port, args.iface)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    main()
