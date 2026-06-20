"""Pi-side call client for the Janus VideoRoom SFU (HTTP API).

The device joins a Janus VideoRoom as a PUBLISHER (camera + mic) and SUBSCRIBES
to the caregiver app's audio (played on the amp). Janus forwards RTP between the
two — no transcoding, so video doesn't freeze (unlike the old aiortc SFU).

Uses Janus's HTTP/long-poll API (the Pi has `requests` but not `websockets`).

Env (set by the C++ launcher): DEVICE_ID, KEY_HASH, SERVER_URL.
Janus: JANUS_HTTP (default http://<server-host>:8088/janus), derived from SERVER_URL.
Mic: MIC_DEVICE=micleft, MIC_FILTER (ffmpeg filtergraph), MIC_RATE=48000.
Camera: CAM_DEVICE=/dev/video9. Speaker: SPEAKER_DEV=plughw:CARD=sndrpigooglevoi,DEV=0.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import socket
import subprocess
import sys
import zlib
from typing import Optional
from urllib.parse import urlparse

import requests

logging.basicConfig(level=logging.INFO, format="[call-helper] %(message)s")
log = logging.getLogger(__name__)

# ── Warm-daemon fast path (unchanged): a resident --serve process pre-imports
# aiortc so each call skips the ~1.3s import. A normal invocation hands the call
# to it over a Unix socket and blocks until it ends.
_CALL_SOCK = "/tmp/noban_call.sock"

def _delegate_to_daemon() -> bool:
    if "--serve" in sys.argv:
        return False
    try:
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        c.settimeout(5); c.connect(_CALL_SOCK)
    except OSError:
        return False
    try:
        cfg = (json.dumps({
            "DEVICE_ID": os.environ.get("DEVICE_ID", ""),
            "KEY_HASH": os.environ.get("KEY_HASH", ""),
            "SERVER_URL": os.environ.get("SERVER_URL", ""),
        }) + "\n").encode()
        c.sendall(cfg); c.settimeout(None)
        buf = b""
        while b"\n" not in buf:
            d = c.recv(256)
            if not d:
                break
            buf += d
        return True
    except OSError:
        return False
    finally:
        try: c.close()
        except OSError: pass

if __name__ == "__main__" and "--serve" not in sys.argv:
    if _delegate_to_daemon():
        sys.exit(0)

try:
    from aiortc import (
        RTCPeerConnection, RTCSessionDescription, MediaStreamTrack,
    )
    from aiortc.contrib.media import MediaPlayer, MediaRecorder, MediaBlackhole, MediaRelay
    from av import VideoFrame
    import numpy as _np
    _AIORTC_OK = True
except Exception as e:
    _AIORTC_OK = False
    _AIORTC_ERR = repr(e)

if _AIORTC_OK:
    class VFlipTrack(MediaStreamTrack):
        """Camera is mounted upside-down — flip each frame vertically."""
        kind = "video"
        def __init__(self, source):
            super().__init__(); self.source = source
        async def recv(self):
            frame = await self.source.recv()
            img = frame.to_ndarray(format="bgr24")[::-1]
            new = VideoFrame.from_ndarray(_np.ascontiguousarray(img), format="bgr24")
            new.pts = frame.pts; new.time_base = frame.time_base
            return new

DEVICE_ID = os.environ.get("DEVICE_ID", "ID_12233945")
KEY_HASH = os.environ.get("KEY_HASH", "")
SERVER_URL = os.environ.get("SERVER_URL", "http://127.0.0.1:5000")


def janus_http_url() -> str:
    if os.environ.get("JANUS_HTTP"):
        return os.environ["JANUS_HTTP"]
    host = urlparse(SERVER_URL).hostname or "127.0.0.1"
    return f"http://{host}:8088/janus"


def room_for(device_id: str) -> int:
    # Deterministic room per device so the app + device meet in the same room.
    return 100000 + (zlib.crc32(device_id.encode()) % 900000)


def play_connect_tone() -> None:
    for cmd in ("play -q -n synth 0.25 sine 880 vol 0.4 2>/dev/null",
                "aplay -q /usr/share/sounds/alsa/Front_Center.wav 2>/dev/null"):
        if os.system(cmd) == 0:
            return


# ── Mic capture through an ffmpeg high-pass+gain stage (the I2S mic is quiet/rumbly).
_MIC_PROCS: list = []
_MIC_FIFOS: list = []

def open_mic_player(mic_dev: str, mic_rate: str):
    gain = os.environ.get("MIC_GAIN", "30")
    flt = os.environ.get("MIC_FILTER", f"highpass=f=120,volume={gain},alimiter=limit=0.9")
    if not flt or flt.lower() == "none":
        return MediaPlayer(mic_dev, format="alsa",
                           options={"channels": "1", "sample_rate": mic_rate})
    import tempfile
    fifo = tempfile.mktemp(prefix="noban_mic_", suffix=".raw")
    os.mkfifo(fifo); _MIC_FIFOS.append(fifo)
    cmd = ["ffmpeg", "-loglevel", "error", "-nostdin",
           "-f", "alsa", "-channels", "1", "-sample_rate", mic_rate, "-i", mic_dev,
           "-af", flt, "-f", "s16le", "-ar", mic_rate, "-ac", "1", "-y", fifo]
    proc = subprocess.Popen(cmd); _MIC_PROCS.append(proc)
    try:
        return MediaPlayer(fifo, format="s16le",
                           options={"channels": "1", "sample_rate": mic_rate})
    except Exception:
        proc.terminate(); raise


def cleanup_mic() -> None:
    for p in _MIC_PROCS:
        try: p.terminate(); p.wait(timeout=2)
        except Exception:
            try: p.kill()
            except Exception: pass
    for f in _MIC_FIFOS:
        try: os.unlink(f)
        except Exception: pass
    _MIC_PROCS.clear(); _MIC_FIFOS.clear()


# ── Janus HTTP/long-poll client ──────────────────────────────────────────────
class Janus:
    """Minimal Janus HTTP API client. Synchronous requests run off the event
    loop via to_thread; a background long-poll feeds async events into per-handle
    asyncio.Queues, keyed so we can await the result of a plugin message."""

    def __init__(self, base: str):
        self.base = base.rstrip("/")
        self.session = requests.Session()
        self.session_id: Optional[int] = None
        self._txn = 0
        self.events: "asyncio.Queue" = asyncio.Queue()
        self._poll_task = None
        self._alive = True

    def _t(self) -> str:
        self._txn += 1
        return f"tx{self._txn}"

    async def _post(self, path: str, body: dict) -> dict:
        def _do():
            r = self.session.post(self.base + path, json=body, timeout=15)
            return r.json()
        return await asyncio.to_thread(_do)

    async def create(self):
        r = await self._post("", {"janus": "create", "transaction": self._t()})
        self.session_id = r["data"]["id"]
        self._poll_task = asyncio.ensure_future(self._poll_loop())
        asyncio.ensure_future(self._keepalive_loop())

    async def attach(self, plugin="janus.plugin.videoroom") -> int:
        r = await self._post(f"/{self.session_id}",
                             {"janus": "attach", "plugin": plugin, "transaction": self._t()})
        return r["data"]["id"]

    async def message(self, handle: int, body: dict, jsep: dict | None = None) -> dict:
        msg = {"janus": "message", "body": body, "transaction": self._t()}
        if jsep:
            msg["jsep"] = jsep
        # Janus returns an immediate ack; the real plugindata/jsep arrives via long-poll.
        return await self._post(f"/{self.session_id}/{handle}", msg)

    async def trickle_complete(self, handle: int):
        await self._post(f"/{self.session_id}/{handle}",
                         {"janus": "trickle", "candidate": {"completed": True},
                          "transaction": self._t()})

    async def _keepalive_loop(self):
        while self._alive:
            await asyncio.sleep(25)
            try:
                await self._post(f"/{self.session_id}",
                                 {"janus": "keepalive", "transaction": self._t()})
            except Exception:
                pass

    async def _poll_loop(self):
        def _poll():
            try:
                r = self.session.get(f"{self.base}/{self.session_id}",
                                     params={"maxev": 5}, timeout=35)
                return r.json()
            except Exception:
                return None
        while self._alive:
            data = await asyncio.to_thread(_poll)
            if data is None:
                await asyncio.sleep(0.5); continue
            for ev in (data if isinstance(data, list) else [data]):
                await self.events.put(ev)

    async def wait_event(self, predicate, timeout=20):
        """Await an event matching predicate(ev)->bool from the queue."""
        loop = asyncio.get_event_loop()
        end = loop.time() + timeout
        backlog = []
        try:
            while loop.time() < end:
                ev = await asyncio.wait_for(self.events.get(), timeout=max(0.1, end - loop.time()))
                if predicate(ev):
                    for b in backlog:
                        await self.events.put(b)
                    return ev
                backlog.append(ev)
        except asyncio.TimeoutError:
            pass
        for b in backlog:
            await self.events.put(b)
        return None

    def close(self):
        self._alive = False


def _plugindata(ev):
    return (ev.get("plugindata") or {}).get("data") or {}


async def run_call() -> int:
    if not _AIORTC_OK:
        log.error("aiortc unavailable: %s", _AIORTC_ERR); return 2
    play_connect_tone()
    room = room_for(DEVICE_ID)
    j = Janus(janus_http_url())
    log.info("Janus %s  room=%s", j.base, room)

    speaker_dev = os.environ.get("SPEAKER_DEV", "plughw:CARD=sndrpigooglevoi,DEV=0")
    cam_dev = os.environ.get("CAM_DEVICE", "/dev/video9")
    mic_dev = os.environ.get("MIC_DEVICE", "micleft")
    mic_rate = os.environ.get("MIC_RATE", "48000")

    pub = None            # publisher RTCPeerConnection (we send cam+mic)
    subs = {}             # feed_id -> subscriber RTCPeerConnection
    relay = MediaRelay()
    cam_player = mic_player = None
    sinks = []            # MediaRecorder amp sinks to stop on exit
    done = asyncio.Event()

    try:
        await j.create()
        # ensure the room exists (ignore "already exists")
        ph = await j.attach()
        await j.message(ph, {"request": "create", "room": room, "publishers": 4,
                             "audiocodec": "opus", "videocodec": "vp8", "bitrate": 256000})
        # join as publisher
        await j.message(ph, {"request": "join", "room": room, "ptype": "publisher",
                             "display": "device"})
        joined = await j.wait_event(lambda e: _plugindata(e).get("videoroom") == "joined")
        if not joined:
            log.error("publisher join failed"); return 1
        my_id = _plugindata(joined).get("id")
        log.info("joined room as publisher id=%s", my_id)

        # open media + publish
        pub = RTCPeerConnection()
        try:
            cam_player = MediaPlayer(cam_dev, format="v4l2",
                                     options={"framerate": "16", "video_size": "640x480"})
        except Exception as e:
            log.error("camera open failed: %s", e)
        try:
            mic_player = open_mic_player(mic_dev, mic_rate)
        except Exception as e:
            log.error("mic open failed: %s", e)
        if mic_player and mic_player.audio:
            pub.addTrack(relay.subscribe(mic_player.audio))
        if cam_player and cam_player.video:
            pub.addTrack(relay.subscribe(VFlipTrack(cam_player.video)))

        offer = await pub.createOffer()
        await pub.setLocalDescription(offer)
        await j.message(ph, {"request": "configure", "audio": True, "video": True},
                        jsep={"type": "offer", "sdp": pub.localDescription.sdp})
        ans = await j.wait_event(lambda e: e.get("jsep") and _plugindata(e).get("configured") == "ok"
                                 or (e.get("jsep") and e.get("jsep", {}).get("type") == "answer"))
        if ans and ans.get("jsep"):
            await pub.setRemoteDescription(RTCSessionDescription(
                sdp=ans["jsep"]["sdp"], type=ans["jsep"]["type"]))
            log.info("publishing cam+mic to Janus")

        # subscribe to any existing publishers (the app) + new ones
        async def subscribe(feed_id):
            if feed_id in subs or feed_id == my_id:
                return
            sh = await j.attach()
            await j.message(sh, {"request": "join", "room": room, "ptype": "subscriber",
                                 "streams": [{"feed": feed_id}]})
            off = await j.wait_event(lambda e: e.get("jsep") and e.get("sender") == sh)
            if not off or not off.get("jsep"):
                log.warning("no offer for feed %s", feed_id); return
            spc = RTCPeerConnection(); subs[feed_id] = spc
            sink = MediaRecorder(speaker_dev, format="alsa")
            sinks.append(sink)
            @spc.on("track")
            def _on(track, _sink=sink):
                if track.kind == "audio":
                    log.info("caregiver audio → amp"); _sink.addTrack(track)
                else:
                    MediaBlackhole().addTrack(track)
            await spc.setRemoteDescription(RTCSessionDescription(
                sdp=off["jsep"]["sdp"], type=off["jsep"]["type"]))
            answer = await spc.createAnswer()
            await spc.setLocalDescription(answer)
            await sink.start()
            await j.message(sh, {"request": "start"},
                            jsep={"type": "answer", "sdp": spc.localDescription.sdp})
            log.info("subscribed to feed %s (two-way audio on)", feed_id)

        for p in _plugindata(joined).get("publishers", []):
            await subscribe(p["id"])

        # event loop: new publishers, leaving, and our own end-poll
        async def end_poll():
            empties = 0
            while not done.is_set():
                try:
                    r = await asyncio.to_thread(lambda: j.session  # noqa
                                                .get(f"{SERVER_URL}/Live_Device",
                                                     headers={"Device-Id": DEVICE_ID, "Key-Hash": KEY_HASH},
                                                     timeout=8).json())
                    if not r.get("call_pending"):
                        empties += 1
                        if empties >= 2:
                            log.info("server reports call ended — leaving"); done.set()
                    else:
                        empties = 0
                except Exception:
                    empties = 0
                await asyncio.sleep(2)
        asyncio.ensure_future(end_poll())

        while not done.is_set():
            ev = await j.wait_event(lambda e: True, timeout=2)
            if ev is None:
                continue
            d = _plugindata(ev)
            for p in d.get("publishers", []) or []:
                await subscribe(p["id"])
            if d.get("leaving") or d.get("unpublished"):
                gone = d.get("leaving") or d.get("unpublished")
                if gone in subs:
                    try: await subs[gone].close()
                    except Exception: pass
                    subs.pop(gone, None)
                # if the app (the only other publisher) left, end the call
                if gone != my_id:
                    log.info("peer left — ending"); done.set()
        return 0
    finally:
        j.close()
        for s in sinks:
            try: await s.stop()
            except Exception: pass
        for pc in [pub, *subs.values()]:
            if pc:
                try: await pc.close()
                except Exception: pass
        for pl in (cam_player, mic_player):
            try:
                if pl and pl.audio: pl.audio.stop()
                if pl and pl.video: pl.video.stop()
            except Exception: pass
        cleanup_mic()
        log.info("clean exit")


def serve() -> int:
    if not _AIORTC_OK:
        print(f"aiortc not installed: {_AIORTC_ERR}", file=sys.stderr); return 2
    global DEVICE_ID, KEY_HASH, SERVER_URL
    try: os.unlink(_CALL_SOCK)
    except OSError: pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(_CALL_SOCK); srv.listen(1)
    try: os.chmod(_CALL_SOCK, 0o666)
    except OSError: pass
    log.info("call daemon ready (Janus client, aiortc pre-warmed) on %s", _CALL_SOCK)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError as e:
            log.error("accept failed: %s", e); continue
        try:
            conn.settimeout(5); buf = b""
            while b"\n" not in buf:
                dd = conn.recv(256)
                if not dd: break
                buf += dd
            cfg = json.loads(buf.decode().strip() or "{}")
            if cfg.get("DEVICE_ID"): DEVICE_ID = cfg["DEVICE_ID"]
            if cfg.get("KEY_HASH"): KEY_HASH = cfg["KEY_HASH"]
            if cfg.get("SERVER_URL"): SERVER_URL = cfg["SERVER_URL"]
            conn.settimeout(None)
            log.info("daemon: handling call for %s", DEVICE_ID)
            rc = asyncio.run(run_call())
            log.info("daemon: call ended rc=%s", rc)
        except Exception as e:
            log.error("daemon call error: %s", e)
        finally:
            try: conn.sendall(b"done\n")
            except OSError: pass
            try: conn.close()
            except OSError: pass


def main() -> int:
    if "--serve" in sys.argv:
        return serve()
    if not _AIORTC_OK:
        print(f"aiortc not installed: {_AIORTC_ERR}", file=sys.stderr); return 2
    p = argparse.ArgumentParser()
    p.add_argument("--device-id"); p.add_argument("--server-url")
    a = p.parse_args()
    global DEVICE_ID, SERVER_URL
    if a.device_id: DEVICE_ID = a.device_id
    if a.server_url: SERVER_URL = a.server_url
    return asyncio.run(run_call())


if __name__ == "__main__":
    sys.exit(main())
