"""Wake-command server-side inference: STT (Persian) -> intent classifier.

Loaded once at server start (heavy models stay in VRAM/RAM). Each call to
``classify_wav(path)`` runs:

    1. faster-whisper large-v3 (FULL — NOT turbo; turbo loses far-field accuracy
                                on the device's distance mic) → Persian transcript
    2. Qwen-2.5-7B-Instruct q4_K_M   → intent ∈ {PILL, CALL, MESSAGE}
                                        (grammar-constrained, no hallucination)

Hardware target: one 6 GB GPU. whisper-large-v3 int8_float16 (~1.5 GB) + Qwen-7B
q4_K_M partially offloaded (WAKE_LLM_GPU_LAYERS≈20). The intent LLM is the only
tunable for throughput (a 3B/1.5B fits with full offload); whisper stays large-v3.

Latency for a 5-s clip: ~1.0–1.5 s end-to-end on this rig.

The module is intentionally side-effect-free — server route (myserver.py)
calls classify_wav() then routes by intent.
"""
from __future__ import annotations

import logging
import os
import threading
import time
from pathlib import Path
from typing import Tuple

logger = logging.getLogger(__name__)
HERE = Path(__file__).resolve().parent

# Serializes GPU work across concurrent Flask threads. Whisper + LLM share
# 8 GB VRAM on the 3050; we cannot let two requests touch the GPU at the
# same time. Each call to classify_wav() acquires this before the heavy
# work and releases when done. Requests pile up here as an implicit FIFO
# queue — that's the intended behaviour.
_GPU_LOCK = threading.Lock()

# Serializes the (one-time) model LOADS. Without this, a /Wake_Command that
# arrives while the background warm_up_async() thread is still constructing a
# model races into a SECOND parallel load of the same model → double VRAM
# allocation on the 8 GB card → OOM/thrash → the request stalls for tens of
# seconds (the device then hits its 30 s HTTP timeout, LED stuck red, command
# lost). With this lock, an early request simply waits for warm-up to finish
# loading, then proceeds on the already-resident model. Distinct from _GPU_LOCK
# (which serializes inference); warm-up never takes _GPU_LOCK, so no deadlock.
_LOAD_LOCK = threading.Lock()

# ── Paths (override via env vars in production) ──────────────────────────────
WHISPER_DIR = os.environ.get(
    "WAKE_WHISPER_DIR", str(HERE / "models" / "faster-whisper-large-v3")
)
LLM_GGUF = os.environ.get(
    # Default matches what ships in models/ and what run_worker.sh / compose set.
    # (Was qwen2.5-14b — that file isn't present and won't fit the 6 GB GPU.)
    "WAKE_LLM_GGUF", str(HERE / "models" / "qwen2.5-7b-instruct-q4_k_m.gguf")
)

# ── Inference config ─────────────────────────────────────────────────────────
WHISPER_DEVICE        = os.environ.get("WAKE_WHISPER_DEVICE", "cuda")
WHISPER_COMPUTE_TYPE  = os.environ.get("WAKE_WHISPER_COMPUTE", "int8_float16")
WHISPER_LANGUAGE      = "fa"

LLM_N_CTX             = int(os.environ.get("WAKE_LLM_CTX", "2048"))
LLM_N_GPU_LAYERS      = int(os.environ.get("WAKE_LLM_GPU_LAYERS", "32"))
LLM_THREADS           = int(os.environ.get("WAKE_LLM_THREADS", "8"))
LLM_GRAMMAR           = 'root ::= "PILL" | "CALL" | "MESSAGE" | "SLEEP" | "WAKE"'

# ── Prompt: deliberately concrete because the LLM only emits ONE of three
# tokens (grammar-constrained), so the prompt's job is to make the right
# choice obvious even when the transcript has STT noise. ─────────────────────
SYSTEM_PROMPT = (
    "You are an intent classifier for an elderly Persian speaker talking to a home "
    "care device. Output EXACTLY ONE label: PILL, CALL, MESSAGE, SLEEP, or WAKE.\n"
    "The input is speech-to-text from a far-field mic and is OFTEN noisy, clipped, "
    "or slightly wrong. Judge by MEANING and by KEY WORDS, not exact spelling. Be "
    "decisive: if the text matches a command's meaning OR contains one of its key "
    "words (even amid garbled text), choose that command. Choose MESSAGE only when "
    "none of the four commands fit.\n\n"
    "Evaluate in THIS priority order and stop at the first that fits:\n\n"
    "1) PILL — anything about a pill / medicine / dose, or acknowledging taking it. "
    "KEY WORDS: قرص، قرصم، دارو، داروها، خوردم، مصرف، گرفتم. "
    "Examples (→ PILL): \"قرصم رو خوردم\", \"قرصمو خوردم\", \"داروهامو خوردم\", "
    "\"خوردمش\", \"مصرف کردم\", \"آره خوردم\", \"قرص رو گرفتم\", \"الان میخورمش\", "
    "\"قرص آره\", \"بله قرص رو خوردم\".\n\n"
    "2) CALL — wants to call / be called / have a family member or caregiver "
    "contacted or come over. KEY WORDS: زنگ، تماس، بزن، بیاد. "
    "Examples (→ CALL): \"زنگ بزن به آرش\", \"باهام تماس بگیر\", \"به پسرم زنگ بزن\", "
    "\"به دخترم بگو زنگ بزنه\", \"بگو بیاد\", \"یکی بیاد پیشم\", \"میخوام تماس بگیرم\".\n\n"
    "3) SLEEP — turn the device OFF / DEACTIVATE / put to sleep / quiet / rest "
    "(stop monitoring). KEY WORDS: بخواب، بخوابون، غیرفعال، خاموش، خواب، استراحت، ساکت. "
    "Examples (→ SLEEP): \"بخواب\", \"برو بخواب\", \"غیرفعالش کن\", \"غیرفعال شو\", "
    "\"خاموشش کن\", \"حالت خواب\", \"دستگاه رو بخوابون\", \"شب بخیر\", "
    "\"دیگه نگاه نکن\", \"استراحت کن\", \"حالت ساکت\".\n\n"
    "4) WAKE — turn the device ON / ACTIVATE / wake up / back to normal "
    "(resume monitoring). KEY WORDS: بیدار، فعال، روشن، مراقب، عادی. "
    "Examples (→ WAKE): \"بیدار شو\", \"بیدار باش\", \"فعالش کن\", \"فعال شو\", "
    "\"روشنش کن\", \"حالت بیدار\", \"صبح بخیر\", \"برگرد به حالت عادی\", "
    "\"دوباره مراقب باش\".\n\n"
    "5) MESSAGE — none of the above: greetings, complaints, small talk, or "
    "unintelligible / fully garbled text with no command meaning. "
    "Examples (→ MESSAGE): \"سلام خوبی\", \"هوا سرده\", \"گشنمه\", \"حالم خوب نیست\".\n\n"
    "CRITICAL RULES:\n"
    "- SLEEP and WAKE are OPPOSITES. off / deactivate / غیرفعال / خاموش / بخواب → SLEEP. "
    "on / activate / فعال / روشن / بیدار → WAKE. Never swap them.\n"
    "- Medicine in ANY form → PILL. Being called/contacted → CALL (never MESSAGE).\n"
    "- Garbled text containing one clear command key word → that command. "
    "Only fully meaningless text → MESSAGE.\n\n"
    "Reply with EXACTLY one word: PILL, CALL, MESSAGE, SLEEP, or WAKE."
)


# ── Singletons (loaded lazily on first call so importing the module is cheap)
_whisper = None
_llm     = None
_grammar = None


def _load_whisper():
    global _whisper
    if _whisper is not None:
        return _whisper
    with _LOAD_LOCK:                       # double-checked: only one real load
        if _whisper is not None:
            return _whisper
        from faster_whisper import WhisperModel
        logger.info("Loading whisper (large-v3) from %s …", WHISPER_DIR)
        _whisper = WhisperModel(
            WHISPER_DIR,
            device=WHISPER_DEVICE,
            compute_type=WHISPER_COMPUTE_TYPE,
        )
        logger.info("Whisper ready.")
    return _whisper


def _load_llm():
    global _llm, _grammar
    if _llm is not None:
        return _llm, _grammar
    with _LOAD_LOCK:                       # double-checked: only one real load
        if _llm is not None:
            return _llm, _grammar
        from llama_cpp import Llama, LlamaGrammar
        logger.info("Loading LLM from %s …", LLM_GGUF)
        _llm = Llama(
            model_path=LLM_GGUF,
            n_ctx=LLM_N_CTX,
            n_gpu_layers=LLM_N_GPU_LAYERS,
            n_threads=LLM_THREADS,
            verbose=False,
        )
        _grammar = LlamaGrammar.from_string(LLM_GRAMMAR)
        logger.info("LLM ready.")
    return _llm, _grammar


def warm_up() -> None:
    """Pre-load both models so the first request doesn't pay the cold-start
    penalty. Safe to call multiple times — guarded by the singleton check
    inside each loader."""
    _load_whisper()
    _load_llm()


def warm_up_async() -> None:
    """Fire-and-forget background warm-up. Used at module import so the
    server (whether run via `python myserver.py` or via Gunicorn) starts
    loading the heavy models as soon as the route is wired up, instead of
    waiting for the first user request to pay the cold-start penalty.

    Disable by setting WAKE_NO_AUTO_WARMUP=1 in the environment (useful for
    short-lived test runs that don't need the GPU)."""
    if os.environ.get("WAKE_NO_AUTO_WARMUP"):
        return
    def _bg():
        try:
            warm_up()
        except Exception as e:
            logger.warning("Background warm-up failed: %r", e)
    threading.Thread(target=_bg, daemon=True, name="wake-warmup").start()


# Kick off models loading in the background as soon as the module is imported.
warm_up_async()


# Seed the decoder with the expected command vocabulary. Whisper uses this as
# prior context, biasing short/far-field utterances toward the domain words the
# elder actually says (pill / call / help) instead of latching onto phonetically
# similar garbage. Kept short — an over-long prompt can cause it to echo phrases.
WHISPER_INITIAL_PROMPT = (
    "دستورهای کوتاه فارسی یک سالمند به دستگاه مراقبت: "
    "قرصم را خوردم، قرصمو خوردم، داروهامو خوردم، قرص خوردم، مصرف کردم، خوردمش. "
    "بهم زنگ بزن، باهام تماس بگیر، به پسرم زنگ بزن، به دخترم بگو زنگ بزنه، یکی بیاد. "
    "بخواب، برو بخواب، غیرفعالش کن، خاموشش کن، حالت خواب، دستگاه را بخوابان. "
    "بیدار شو، فعالش کن، روشنش کن، حالت بیدار، برگرد به حالت عادی. "
    "حالم خوب نیست، کمکم کن."
)


def transcribe(wav_path: str) -> str:
    """Persian transcription via faster-whisper large-v3. Returns empty string
    on silence / no speech detected."""
    model = _load_whisper()
    # beam_size=5: greedy (beam=1) was latching onto garbage decodings on short
    # commands ("بهم زنگ بزن" → "به گوبه امزایم به زمین"), which then fell back to
    # MESSAGE. The 30s-timeout risk that motivated greedy was actually the
    # cold-start load race (fixed via _LOAD_LOCK), not beam cost — warm beam=5 is
    # ~1.6–6.5s, well inside budget. Recall matters more than 2s here.
    segments, _info = model.transcribe(
        wav_path,
        language=WHISPER_LANGUAGE,
        beam_size=5,
        vad_filter=True,           # strip leading/trailing silence
        condition_on_previous_text=False,
        initial_prompt=WHISPER_INITIAL_PROMPT,
    )
    segs = list(segments)
    text = " ".join(s.text for s in segs).strip()
    if not text:
        return ""

    # ── Hallucination guard ──────────────────────────────────────────────
    # faster-whisper invents plausible phrases from non-speech / ambient audio
    # even with vad_filter on (observed: silence → "بگو بینم علیک همین یاد" →
    # the LLM routed it to CALL). We blank such transcripts so they fall through
    # the EXISTING empty→MESSAGE path (harmless) instead of triggering CALL/PILL.
    #
    # Uses Whisper's own reliability heuristics so genuine commands are NOT
    # affected: real speech has avg_logprob > ~-1.0, no_speech_prob < ~0.6, and
    # compression_ratio < ~2.4. Drop only when the clip is silence-like (high
    # no_speech AND low logprob — Whisper's own no-speech rule) or repetitive
    # (high compression = looping hallucination). Thresholds are env-tunable so
    # they can be tightened/loosened from real logged data without a code change.
    import statistics
    no_speech_max = float(os.environ.get("WAKE_NO_SPEECH_MAX", "0.6"))
    logprob_min   = float(os.environ.get("WAKE_LOGPROB_MIN", "-1.0"))
    compress_max  = float(os.environ.get("WAKE_COMPRESS_MAX", "2.4"))
    try:
        avg_logprob  = statistics.fmean(s.avg_logprob for s in segs)
        no_speech    = statistics.fmean(s.no_speech_prob for s in segs)
        max_compress = max(s.compression_ratio for s in segs)
    except (ValueError, AttributeError):
        return text   # metrics unavailable → don't second-guess, keep transcript

    silence_like = (no_speech > no_speech_max and avg_logprob < logprob_min)
    repetitive   = (max_compress > compress_max)
    if silence_like or repetitive:
        logger.info("[wake] hallucination guard DROPPED transcript=%r "
                    "(no_speech=%.2f avg_logprob=%.2f compress=%.2f)",
                    text, no_speech, avg_logprob, max_compress)
        return ""   # → classify_intent shortcuts to MESSAGE (existing path)

    logger.info("[wake] stt ok transcript=%r "
                "(no_speech=%.2f avg_logprob=%.2f compress=%.2f)",
                text, no_speech, avg_logprob, max_compress)
    return text


def classify_intent(transcript: str) -> str:
    """LLM intent classification. Grammar-constrained → returns exactly one
    of: 'PILL', 'CALL', 'MESSAGE'. Empty / silence transcript shortcuts to
    MESSAGE (it'll be saved as a voice memo in case it has content the STT
    missed)."""
    if not transcript:
        return "MESSAGE"
    llm, grammar = _load_llm()
    out = llm.create_chat_completion(
        messages=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user",   "content": f'Transcript: "{transcript}"'},
        ],
        grammar=grammar,
        temperature=0.0,
        max_tokens=4,
    )
    answer = out["choices"][0]["message"]["content"].strip().upper()
    # Grammar guarantees one of three tokens; defensive default just in case
    # llama-cpp ever emits a stray space.
    for tag in ("PILL", "CALL", "MESSAGE", "SLEEP", "WAKE"):
        if tag in answer:
            return tag
    return "MESSAGE"


def classify_wav(wav_path: str) -> Tuple[str, str]:
    """Full pipeline. Returns (intent, transcript).

    Thread-safe: serialized through _GPU_LOCK so concurrent Flask threads
    don't trample each other's CUDA streams. The lock acquisition is the
    de-facto request queue under load — each request waits ~(N×1.5 s) when
    N requests are ahead of it. For elderly-care load this is fine; if you
    ever push past ~50 concurrent /Wake_Command/sec switch to vLLM."""
    t_wait = time.monotonic()
    with _GPU_LOCK:
        t_stt = time.monotonic()
        transcript = transcribe(wav_path)
        t_llm = time.monotonic()
        intent     = classify_intent(transcript)
        t_end = time.monotonic()
    # print() (not just logger) so it lands in the captured server log — this
    # is the line to check when an intent looks wrong: it shows EXACTLY what
    # Whisper heard and what the LLM mapped it to.
    print(f"[wake_command] intent={intent}  transcript={transcript!r}  "
          f"[wait={t_stt-t_wait:.2f}s stt={t_llm-t_stt:.2f}s llm={t_end-t_llm:.2f}s]",
          flush=True)
    return intent, transcript


def queue_depth_hint() -> int:
    """Best-effort hint of how many requests are waiting on the GPU lock.
    Not authoritative (Python locks don't expose this), but useful for the
    Flask route to return a 503 with retry-after under heavy load. Currently
    returns 0 — drop in a Semaphore implementation if you actually need this.
    """
    return 0
