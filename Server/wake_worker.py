"""GPU wake-command worker — the ONLY process that loads whisper + the LLM.

Pulls jobs from the Redis queue `wake:jobs` ({id, wav_path}), runs the existing
classify (whisper STT + LLM intent), and pushes the result to `wake:res:{id}`.
The API server no longer loads the models (so it can run multiple workers without
OOM) — it just enqueues and waits on the result.

Scaling on the real box: run this with a batched backend (vLLM for the LLM +
batched faster-whisper) and/or multiple workers — the queue + API are unchanged.
This single-GPU version uses the current models (one worker holds ~5 GB VRAM).

Env (same as run_async.sh): WAKE_WHISPER_DIR, WAKE_LLM_GGUF, WAKE_LLM_GPU_LAYERS,
WAKE_WHISPER_DEVICE/COMPUTE, LD_LIBRARY_PATH (CUDA). REDIS_URL optional.
"""
import json
import logging
import os
import time

import redis

import wake_command

logging.basicConfig(level=logging.INFO, format="[wake-worker] %(asctime)s %(message)s", datefmt="%H:%M:%S")
log = logging.getLogger(__name__)

_r = redis.Redis.from_url(os.environ.get("REDIS_URL", "redis://127.0.0.1:6379"),
                          decode_responses=True)
JOBS = "wake:jobs"
RES = "wake:res:"          # + job id


def main() -> None:
    log.info("loading models (whisper + LLM)…")
    t0 = time.monotonic()
    wake_command.warm_up()                       # load both models onto the GPU
    log.info("models ready in %.1fs — waiting for jobs on %s", time.monotonic() - t0, JOBS)
    while True:
        try:
            item = _r.blpop(JOBS, timeout=5)
        except redis.exceptions.TimeoutError:
            continue                                 # idle window elapsed — normal, keep waiting
        except redis.exceptions.ConnectionError:
            log.warning("redis connection lost — retrying"); time.sleep(1); continue
        if item is None:
            continue
        try:
            job = json.loads(item[1])
        except Exception:
            continue
        jid = job.get("id"); wav = job.get("wav_path")
        t = time.monotonic()
        try:
            intent, transcript = wake_command.classify_wav(wav)
        except Exception as ex:
            log.warning("classify failed (%s): %s", wav, ex)
            intent, transcript = "MESSAGE", ""   # safe fallback (server stores as a message)
        out = json.dumps({"intent": intent, "transcript": transcript}, ensure_ascii=False)
        _r.rpush(RES + str(jid), out)
        _r.expire(RES + str(jid), 60)
        log.info("job %s → %s (%.1fs)", jid, intent, time.monotonic() - t)


if __name__ == "__main__":
    main()
