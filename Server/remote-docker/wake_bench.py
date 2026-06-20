"""Wake-command throughput benchmark — fires N concurrent /Wake_Command requests
with a real clip through the live queue→worker path, and reports clips/sec +
latency. Run it against the CURRENT worker for a baseline, then again after
swapping in vLLM/batched whisper (or more workers) to measure the gain.

Usage:
  python wake_bench.py <n_concurrent> <clip.wav> [server_url] [device_id] [key_hash]
e.g.
  python wake_bench.py 50 uploads/room-tesr_..._wake.wav http://127.0.0.1:5000 room-tesr <KEY_HASH>
"""
import asyncio, sys, time
import requests


async def one(url, headers, data, lat, errs):
    t = time.monotonic()
    try:
        r = await asyncio.to_thread(
            lambda: requests.post(url, headers=headers, data=data, timeout=300))
        (lat if r.status_code == 200 else errs).append(
            time.monotonic() - t if r.status_code == 200 else r.status_code)
    except Exception as e:
        errs.append(str(e)[:50])


async def main():
    if len(sys.argv) < 3:
        print(__doc__); return
    n = int(sys.argv[1])
    data = open(sys.argv[2], "rb").read()
    base = sys.argv[3] if len(sys.argv) > 3 else "http://127.0.0.1:5000"
    dev = sys.argv[4] if len(sys.argv) > 4 else "room-tesr"
    kh = sys.argv[5] if len(sys.argv) > 5 else ""
    url = base.rstrip("/") + "/Wake_Command"
    headers = {"Device-Id": dev, "Key-Hash": kh}
    lat, errs = [], []
    print(f"firing {n} concurrent wake-commands at {url} …")
    t0 = time.monotonic()
    await asyncio.gather(*[one(url, headers, data, lat, errs) for _ in range(n)])
    span = time.monotonic() - t0
    lat.sort()
    print(f"  done={len(lat)}/{n}  errors={len(errs)}  wall={span:.1f}s  "
          f"throughput={len(lat)/span:.2f} clips/s")
    if lat:
        print(f"  latency  p50={lat[len(lat)//2]:.1f}s  "
              f"p95={lat[int(len(lat)*0.95)]:.1f}s  worst={lat[-1]:.1f}s")
    if errs:
        print(f"  first errors: {errs[:5]}")


asyncio.run(main())
