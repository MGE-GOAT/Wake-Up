"""Fake device for the elderly_care_app system.

Acts as the IoT device-half of the protocol:
  1. Exposes :6000 for the Flutter app's setup flow
       GET  /configurable_devices  -> [{"id": "<DEVICE_ID>"}]
       POST /config_device         -> accept wifi_ssid/wifi_password/key
  2. Pretends to be a real device toward the Flask server at :5000
       GET  /Live_Device           -> heartbeat every 30s
       POST /Config_Device         -> registers a config on first run
       POST /Message_Device        -> sample event message (CLI command)

Run:
    python fake_device.py                       # use defaults
    python fake_device.py --device-id ID_12233946
    python fake_device.py --server http://192.168.1.42:5000

While running, in another terminal:
    curl -X POST http://localhost:6000/send_event   # trigger a fake "Event" message
"""

import argparse
import asyncio
import io
import json
import math
import threading
import time
from datetime import datetime
from fractions import Fraction

import av
import numpy as np
import requests
from aiortc import (
    RTCPeerConnection, RTCSessionDescription, RTCIceCandidate,
    MediaStreamTrack,
)
from aiortc.contrib.media import MediaBlackhole
from av import VideoFrame
from flask import Flask, jsonify, request

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser()
parser.add_argument("--device-id", default="ID_12233945",
                    help="A device id present in produced_devices (default: ID_12233945)")
parser.add_argument("--auth-code", default="1223344556677889",
                    help="Authentication code for the device id")
parser.add_argument("--server", default="http://127.0.0.1:5000",
                    help="Base URL of the Flask server")
parser.add_argument("--listen-port", type=int, default=6000,
                    help="Port to expose to the app (default 6000)")
parser.add_argument("--heartbeat-interval", type=int, default=30,
                    help="Seconds between Live_Device heartbeats")
args = parser.parse_args()

DEVICE_ID = args.device_id
AUTH_CODE = str(args.auth_code)
SERVER = args.server.rstrip("/")
LISTEN_PORT = args.listen_port

# Cached state set by /config_device from app
_setup_state = {
    "configured": False,
    "wifi_ssid": None,
    "wifi_password": None,
    "key": None,
}

app = Flask(__name__)


# ---------------------------------------------------------------------------
# Endpoints the Flutter app calls during setup (acts as the device's HTTP face)
# ---------------------------------------------------------------------------
@app.route("/configurable_devices", methods=["GET"])
def configurable_devices():
    """App discovery: list of unsetup devices reachable on this LAN."""
    return jsonify([
        {"id": DEVICE_ID, "name": f"FakeDevice-{DEVICE_ID}"}
    ])


@app.route("/config_device", methods=["POST"])
def config_device_from_app():
    """App pushes wifi/key setup."""
    body = request.get_json(silent=True) or {}
    print(f"[device] /config_device received: {body}")

    _setup_state.update({
        "configured": True,
        "wifi_ssid": body.get("wifi_ssid"),
        "wifi_password": body.get("wifi_password"),
        "key": body.get("key"),
    })

    # Immediately echo this config to the server so it appears in active_devices.
    try:
        r = requests.post(
            f"{SERVER}/Config_Device",
            headers={"Device-ID": DEVICE_ID, "Authentication-Code": AUTH_CODE},
            json={"key-Hash": str(hash(body.get("key", "")))},
            timeout=10,
        )
        print(f"[device] -> server /Config_Device: {r.status_code} {r.text[:120]}")
    except Exception as e:
        print(f"[device] !! server /Config_Device failed: {e}")

    return jsonify({"message": "device configured"}), 200


# ---------------------------------------------------------------------------
# Endpoints to manually trigger device->server traffic for testing
# ---------------------------------------------------------------------------
@app.route("/send_voice", methods=["POST"])
def trigger_voice():
    """Curl this with a multipart 'audio' file to ship a voice message to the app.
    If no file is attached, a tiny placeholder clip is sent so the inbox can be tested."""
    audio = request.files.get("audio")
    if audio is None:
        # synthesize a 1-byte placeholder so the server has *something* to store
        fake = io.BytesIO(b"\x00" * 16)
        files = {"audio": ("placeholder.m4a", fake, "audio/mp4")}
    else:
        buf = io.BytesIO(audio.read())
        files = {"audio": (audio.filename or "clip.m4a", buf, audio.mimetype or "audio/mp4")}
    try:
        r = requests.post(
            f"{SERVER}/Voice_Message_Device",
            headers={"Device-ID": DEVICE_ID, "Authentication-Code": AUTH_CODE},
            files=files,
            timeout=20,
        )
        print(f"[device] -> server /Voice_Message_Device: {r.status_code} {r.text[:200]}")
        return jsonify({"server_status": r.status_code, "server_body": r.text}), r.status_code
    except Exception as e:
        return jsonify({"error": str(e)}), 502


@app.route("/send_event", methods=["POST"])
def trigger_event():
    """Curl this from your laptop to send a sample 'Event' message."""
    payload = {
        "Message Time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "Message Type": "Event",
        "description": "Fake fall detected",
    }
    fake_image = io.BytesIO(b"\x89PNG\r\n\x1a\n" + b"\x00" * 32)
    try:
        r = requests.post(
            f"{SERVER}/Message_Device",
            headers={"Device-ID": DEVICE_ID, "Authentication-Code": AUTH_CODE},
            data={"text": json.dumps(payload)},
            files={"image file": ("evt.bin", fake_image, "application/octet-stream")},
            timeout=15,
        )
        print(f"[device] -> server /Message_Device: {r.status_code} {r.text[:200]}")
        return jsonify({"server_status": r.status_code, "server_body": r.text}), r.status_code
    except Exception as e:
        return jsonify({"error": str(e)}), 502


# ---------------------------------------------------------------------------
# Heartbeat loop
# ---------------------------------------------------------------------------
def heartbeat_loop():
    while True:
        try:
            r = requests.get(
                f"{SERVER}/Live_Device",
                headers={"Device-id": DEVICE_ID, "Authentication-Code": AUTH_CODE},
                timeout=10,
            )
            print(f"[heartbeat] {datetime.now().strftime('%H:%M:%S')} -> {r.status_code}: {r.text[:120]}")
        except Exception as e:
            print(f"[heartbeat] error: {e}")
        time.sleep(args.heartbeat_interval)


# ---------------------------------------------------------------------------
# Boot
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# WebRTC: send a synthetic video track (animated test pattern) + send/recv
# audio. Signaling is shuttled via the Flask server's /Call_* endpoints.
# ---------------------------------------------------------------------------
class TestPatternVideoTrack(MediaStreamTrack):
    kind = "video"

    def __init__(self, width=320, height=240, fps=15):
        super().__init__()
        self._w, self._h, self._fps = width, height, fps
        self._frame = 0
        self._start = time.time()

    async def recv(self):
        # pace frames to fps
        target_pts = self._frame / self._fps
        elapsed = time.time() - self._start
        if elapsed < target_pts:
            await asyncio.sleep(target_pts - elapsed)

        # generate a moving gradient + timestamp banner so it's obviously "live"
        t = self._frame
        ys, xs = np.mgrid[0:self._h, 0:self._w].astype(np.float32)
        r = ((xs + t) % 256).astype(np.uint8)
        g = ((ys + t * 2) % 256).astype(np.uint8)
        b = np.full((self._h, self._w), int(128 + 127 * math.sin(t / 20.0)), dtype=np.uint8)
        rgb = np.stack([r, g, b], axis=-1)
        frame = VideoFrame.from_ndarray(rgb, format="rgb24")
        frame.pts = self._frame
        frame.time_base = Fraction(1, self._fps)
        self._frame += 1
        return frame


_pc = None
_pc_lock = threading.Lock()


def _signal_loop():
    """Poll the Flask server for an app offer; when it arrives, build a
    peer connection (recv audio from app, send video+audio to app)."""
    print("[call] signaling loop started")
    while True:
        try:
            r = requests.post(
                f"{SERVER}/Call_Offer",
                json={"device_id": DEVICE_ID},
                timeout=10,
            )
            offer = (r.json() or {}).get("offer")
            if offer:
                print(f"[call] received offer from app")
                asyncio.run(_handle_offer(offer))
        except Exception as e:
            print(f"[call] poll error: {e}")
        time.sleep(2)


async def _handle_offer(offer):
    global _pc
    if _pc is not None:
        try:
            await _pc.close()
        except Exception:
            pass
        _pc = None

    pc = RTCPeerConnection()
    _pc = pc

    # outbound: send the test-pattern video to the app
    pc.addTrack(TestPatternVideoTrack())

    # inbound from app: just consume into a blackhole (or print level)
    blackhole = MediaBlackhole()

    @pc.on("track")
    def on_track(track):
        print(f"[call] inbound track from app: kind={track.kind}")
        blackhole.addTrack(track)

    @pc.on("iceconnectionstatechange")
    async def on_state():
        print(f"[call] ICE state -> {pc.iceConnectionState}")
        if pc.iceConnectionState in ("failed", "closed", "disconnected"):
            try:
                await pc.close()
            except Exception:
                pass

    @pc.on("icecandidate")
    async def on_ice(event):
        if event.candidate:
            cand = event.candidate
            payload = {
                "candidate": cand.to_sdp() if hasattr(cand, "to_sdp") else str(cand),
                "sdpMid": cand.sdpMid,
                "sdpMLineIndex": cand.sdpMLineIndex,
            }
            try:
                requests.post(
                    f"{SERVER}/Call_Ice",
                    json={"device_id": DEVICE_ID, "side": "device", "candidate": payload},
                    timeout=5,
                )
            except Exception as e:
                print(f"[call] ice send err: {e}")

    await pc.setRemoteDescription(RTCSessionDescription(sdp=offer["sdp"], type=offer["type"]))
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)

    # Push answer + start the blackhole consumer
    await blackhole.start()
    requests.post(
        f"{SERVER}/Call_Answer",
        json={"device_id": DEVICE_ID, "sdp": pc.localDescription.sdp, "type": pc.localDescription.type},
        timeout=10,
    )
    print("[call] answer posted")

    # Drain app ICE candidates for the lifetime of the call
    while pc.iceConnectionState not in ("failed", "closed"):
        try:
            r = requests.post(
                f"{SERVER}/Call_Ice",
                json={"device_id": DEVICE_ID, "side": "device"},
                timeout=8,
            )
            for cand in (r.json() or {}).get("candidates", []):
                try:
                    ice = RTCIceCandidate(
                        component=1,
                        foundation="0",
                        ip="0.0.0.0",
                        port=0,
                        priority=0,
                        protocol="udp",
                        type="host",
                        sdpMid=cand.get("sdpMid"),
                        sdpMLineIndex=cand.get("sdpMLineIndex"),
                    )
                    # aiortc accepts addIceCandidate(cand) where cand has .candidate sdp
                    ice.sdpMid = cand.get("sdpMid")
                    ice.sdpMLineIndex = cand.get("sdpMLineIndex")
                    await pc.addIceCandidate(ice)
                except Exception as e:
                    print(f"[call] addIceCandidate err: {e}")
        except Exception as e:
            print(f"[call] ice poll err: {e}")
        await asyncio.sleep(1)


if __name__ == "__main__":
    print(f"=== fake device {DEVICE_ID} ===")
    print(f"  exposes  :{LISTEN_PORT}  (app -> device setup)")
    print(f"  reports to {SERVER}    (device -> server heartbeats/messages + signaling)")
    threading.Thread(target=heartbeat_loop, daemon=True).start()
    threading.Thread(target=_signal_loop, daemon=True).start()
    app.run(host="0.0.0.0", port=LISTEN_PORT, debug=False, use_reloader=False)
