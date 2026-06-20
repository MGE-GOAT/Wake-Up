"""Self-test: connect a fake device + fake app to the SFU (in-process) and verify
media forwards both ways. No real phone/Pi needed. Proves the server SFU core."""
import asyncio
import logging

import numpy as np
from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import AudioStreamTrack, VideoStreamTrack
from av import VideoFrame

# Force aioice to use ONLY loopback so the two in-process peers connect cleanly
# (this laptop has docker0 + a VPN tunnel that break same-host ICE). This is a
# TEST-ONLY shim; real clients connect to the server's LAN/public IP normally.
import aioice.ice
aioice.ice.get_host_addresses = lambda use_ipv4, use_ipv6: ["127.0.0.1"]

import sfu

logging.basicConfig(level=logging.INFO)


class TestVideo(VideoStreamTrack):
    async def recv(self):
        pts, tb = await self.next_timestamp()
        arr = np.full((480, 640, 3), 100, dtype=np.uint8)
        f = VideoFrame.from_ndarray(arr, format="bgr24")
        f.pts = pts; f.time_base = tb
        return f


async def _gather(pc):
    if pc.iceGatheringState == "complete":
        return
    ev = asyncio.Event()
    @pc.on("icegatheringstatechange")
    def _():
        if pc.iceGatheringState == "complete":
            ev.set()
    try:
        await asyncio.wait_for(ev.wait(), 5)
    except asyncio.TimeoutError:
        pass


async def _drain(track, counter, key):
    while True:
        try:
            await track.recv(); counter[key] += 1
        except Exception:
            break


async def make_peer(role):
    pc = RTCPeerConnection(RTCConfiguration(bundlePolicy="max-bundle"))
    counter = {"v": 0, "a": 0}
    if role == "device":
        pc.addTrack(AudioStreamTrack())     # device sends mic  (audio first → bundle-tag = audio)
        pc.addTrack(TestVideo())            # device sends camera
    else:  # app
        pc.addTransceiver("video", direction="recvonly")  # app wants to RECEIVE video
        pc.addTrack(AudioStreamTrack())     # app sends caregiver audio

    @pc.on("track")
    def _(t):
        asyncio.ensure_future(_drain(t, counter, "v" if t.kind == "video" else "a"))

    await pc.setLocalDescription(await pc.createOffer())
    await _gather(pc)
    ans = await sfu.sfu_join("test-dev", role, pc.localDescription.sdp)
    await pc.setRemoteDescription(RTCSessionDescription(sdp=ans, type="answer"))
    return pc, counter


async def main():
    # Prime aioice — the FIRST connection in the process reliably fails ICE on this
    # host's loopback; a throwaway pc absorbs that so the real legs both connect.
    warm = RTCPeerConnection(RTCConfiguration(bundlePolicy="max-bundle"))
    warm.addTrack(AudioStreamTrack())
    await warm.setLocalDescription(await warm.createOffer())
    await _gather(warm)
    await warm.close()
    print(">> (aioice primed)")

    print(">> device joins (sends video+audio)…")
    dev_pc, dev = await make_peer("device")
    await asyncio.sleep(2)                       # let the device leg connect first (like a real call)
    print(">> app joins (sends audio, wants video+audio)…")
    app_pc, app = await make_peer("app")
    print(">> media flowing for 6s…")
    await asyncio.sleep(6)

    print(f"\nAPP received    : video={app['v']} frames  audio={app['a']} frames   (want both >0)")
    print(f"DEVICE received : audio={dev['a']} frames                          (want >0)")
    print(f"DEVICE received : video={dev['v']} frames                          (want 0 — one-sided)")
    ok = app['v'] > 0 and app['a'] > 0 and dev['a'] > 0 and dev['v'] == 0
    print("\nSFU FORWARDING:", "PASS ✅" if ok else "FAIL ❌")

    await sfu.sfu_leave("test-dev", "app")
    await sfu.sfu_leave("test-dev", "device")
    await dev_pc.close(); await app_pc.close()


asyncio.run(main())
