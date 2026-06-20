"""Synthetic SFU load test — measures Janus's per-call CPU + bandwidth so we can
extrapolate to 200. Each "call" = an aiortc publisher (synthetic 320x240 video +
silent audio) + a subscriber, in its own Janus room → Janus forwards the RTP.

The aiortc generators are CPU-heavy (they ENCODE), so the laptop caps how many we
can spin; that's fine — we read JANUS's own %cpu/bandwidth, which is what scales.
Run: python sfu_loadtest.py <n_calls>
"""
import asyncio, json, os, sys, time
import numpy as np
import requests
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import AudioStreamTrack, VideoStreamTrack
from av import VideoFrame

JANUS = os.environ.get("JANUS_HTTP", "http://127.0.0.1:8088/janus")


class BlackVideo(VideoStreamTrack):
    async def recv(self):
        pts, tb = await self.next_timestamp()
        f = VideoFrame.from_ndarray(np.zeros((240, 320, 3), dtype=np.uint8), format="bgr24")
        f.pts, f.time_base = pts, tb
        return f


class J:
    def __init__(self): self.s = requests.Session(); self.sid = None; self._t = 0
    def t(self): self._t += 1; return f"t{self._t}"
    async def post(self, path, body):
        return await asyncio.to_thread(lambda: self.s.post(JANUS + path, json=body, timeout=15).json())
    async def create(self):
        r = await self.post("", {"janus": "create", "transaction": self.t()}); self.sid = r["data"]["id"]
    async def attach(self):
        r = await self.post(f"/{self.sid}", {"janus": "attach", "plugin": "janus.plugin.videoroom", "transaction": self.t()})
        return r["data"]["id"]
    async def msg(self, h, body, jsep=None):
        m = {"janus": "message", "body": body, "transaction": self.t()}
        if jsep: m["jsep"] = jsep
        return await self.post(f"/{self.sid}/{h}", m)
    async def poll(self, want, timeout=20):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                data = await asyncio.to_thread(lambda: self.s.get(f"{JANUS}/{self.sid}", params={"maxev": 5}, timeout=8).json())
            except Exception:
                continue                    # long-poll idle window — retry
            for ev in (data if isinstance(data, list) else [data]):
                if want(ev): return ev
        return None


async def one_call(idx):
    """Publisher + subscriber in room (1,000,000+idx). Returns (pub_pc, sub_pc) to keep alive."""
    room = 1_000_000 + idx
    j = J(); await j.create()
    ph = await j.attach()
    await j.msg(ph, {"request": "create", "room": room, "publishers": 4, "bitrate": 256000})
    await j.msg(ph, {"request": "join", "room": room, "ptype": "publisher", "display": f"pub{idx}"})
    joined = await j.poll(lambda e: (e.get("plugindata", {}).get("data", {}) or {}).get("videoroom") == "joined")
    if not joined: raise RuntimeError("no joined event")
    my_id = joined["plugindata"]["data"]["id"]
    pub = RTCPeerConnection()
    pub.addTrack(AudioStreamTrack()); pub.addTrack(BlackVideo())
    off = await pub.createOffer(); await pub.setLocalDescription(off)
    ans = await j.msg(ph, {"request": "configure", "audio": True, "video": True},
                      jsep={"type": "offer", "sdp": pub.localDescription.sdp})
    je = ans.get("jsep") or (await j.poll(lambda e: e.get("jsep")) or {}).get("jsep")
    if not je: raise RuntimeError("no publish answer")
    await pub.setRemoteDescription(RTCSessionDescription(sdp=je["sdp"], type=je["type"]))
    for _ in range(40):                      # let the publisher's ICE connect before subscribing
        if pub.iceConnectionState in ("connected", "completed"): break
        await asyncio.sleep(0.2)
    if idx == 0: print(f"  [debug] pub ICE = {pub.iceConnectionState}")
    # subscriber
    sh = await j.attach()
    await j.msg(sh, {"request": "join", "room": room, "ptype": "subscriber", "streams": [{"feed": my_id}]})
    offer = await j.poll(lambda e: e.get("jsep") and e.get("sender") == sh, timeout=25)
    if not offer: raise RuntimeError("no subscriber offer")
    sub = RTCPeerConnection()
    @sub.on("track")
    def _(t):
        async def drain():
            while True:
                try: await t.recv()
                except Exception: break
        asyncio.ensure_future(drain())
    await sub.setRemoteDescription(RTCSessionDescription(sdp=offer["jsep"]["sdp"], type=offer["jsep"]["type"]))
    a = await sub.createAnswer(); await sub.setLocalDescription(a)
    await j.msg(sh, {"request": "start"}, jsep={"type": "answer", "sdp": sub.localDescription.sdp})
    return pub, sub


def janus_stat():
    import subprocess
    out = subprocess.run(["ps", "-o", "%cpu,rss", "-C", "janus", "--no-headers"],
                         capture_output=True, text=True).stdout.split()
    return (float(out[0]), float(out[1]) / 1024) if len(out) >= 2 else (0.0, 0.0)


async def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    print(f"establishing {n} calls (pub+sub each) through Janus…")
    pcs = []
    for i in range(n):
        try:
            pcs.append(await one_call(i))
        except Exception as e:
            print(f"  call {i} failed: {e}")
    await asyncio.sleep(6)  # let media flow + CPU settle
    samples = []
    for _ in range(5):
        samples.append(janus_stat()); await asyncio.sleep(1)
    cpu = sum(s[0] for s in samples) / len(samples); rss = samples[-1][1]
    print(f"  calls up: {len(pcs)}/{n}")
    print(f"  JANUS cpu={cpu:.1f}%  rss={rss:.0f}MB  → per-call ≈ {cpu/max(1,len(pcs)):.2f}% cpu")
    for pub, sub in pcs:
        await pub.close(); await sub.close()

asyncio.run(main())
