"""Phase 5.4 — async FastAPI server (PostgreSQL/asyncpg).

A 1:1 async port of myserver.py. Same routes, same request/response shapes,
same MQTT publish call-sites, same SMTP-via-SOCKS5, same single-call lock —
so the existing phone app + Pi pipeline talk to it unchanged.

Architecture (single-box / one-GPU / ~200 users):
  • Single uvicorn process, async event loop handles all I/O-bound endpoints.
  • Heavy /Wake_Command (Whisper STT + LLM) is offloaded to a separate GPU
    worker process over a Redis job queue (_classify_via_queue → wake_worker.py),
    which serializes inference on the card. The API process never loads the
    models, so the event loop keeps serving other requests while a job runs.
  • WebRTC SDP/ICE mailbox + validation codes + image-req map are in-process
    dicts. Correct for one process; move to Redis if you ever scale to
    multiple workers / boxes.

Run:  uvicorn app_async:app --host 0.0.0.0 --port 5000
"""
from __future__ import annotations

import asyncio
import base64
import glob
import hashlib
import hmac
import logging
import bcrypt
import re as _re
import subprocess
import json
import os
import random
import smtplib
import threading
import time
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

import asyncpg
import requests
from fastapi import FastAPI, Request, UploadFile, File, Form, Header, Query
from fastapi.responses import JSONResponse, FileResponse
from starlette.concurrency import run_in_threadpool

import google.auth  # noqa: F401  (kept for parity / token refresh path)
from google.auth.transport.requests import Request as GoogleRequest
from google.oauth2 import service_account

SCOPES = ['https://www.googleapis.com/auth/firebase.messaging']

# Lazy heavy imports — same pattern as myserver.py so the API still boots on
# a box without the model files / CUDA.
try:
    import wake_command
except Exception as _e:  # pragma: no cover
    wake_command = None
    print(f"⚠️  wake_command unavailable ({_e!r}); /Wake_Command → 503")

import mqtt_pub  # sync paho publisher; publish() is non-blocking (queues)

# ── Config ───────────────────────────────────────────────────────────────
DATABASE_URL = os.environ.get(
    "DATABASE_URL",
    "postgresql://elderly:elderly_local_pw@127.0.0.1:5432/elderly_care",
)
UPLOAD_FOLDER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "uploads")
PILL_AUDIO_DIR = os.path.join(UPLOAD_FOLDER, "pill_audio")
# Caregiver→device voice note: transient relay, NEVER persisted to the DB.
# device_id -> filepath; delivered once via the heartbeat, deleted on fetch.
VOICE_TO_DEVICE_DIR = os.path.join(UPLOAD_FOLDER, "voice_to_device")
os.makedirs(VOICE_TO_DEVICE_DIR, exist_ok=True)
_pending_voice = {}
# Guards the get→swap of _pending_voice now that the file write is offloaded to a
# thread (introduces an await between read and set).
_pending_voice_lock = asyncio.Lock()
# Hard cap on device→server media uploads. A forged device credential could
# otherwise POST a multi-GB body and exhaust memory/disk (the body is read fully
# before writing). Fall A/V clips are ~10 s; 25 MB is generous headroom.
_MAX_UPLOAD_BYTES = int(os.environ.get("MAX_UPLOAD_BYTES", str(25 * 1024 * 1024)))

def _safe_name(s: str) -> str:
    return "".join(c if c.isalnum() or c in "_-" else "_" for c in str(s))
os.makedirs(UPLOAD_FOLDER, exist_ok=True)
os.makedirs(PILL_AUDIO_DIR, exist_ok=True)

_PILL_ACTIVE_WINDOW_SEC = 60 * 60
_CALL_MAX_DURATION_SEC = 30 * 60
# An offer that's never answered within this window = a call that never connected
# (device couldn't answer, app gave up, helper crashed). Clear it so call_pending
# stops sticking true and the device stops re-launching the call helper in a loop.
_CALL_OFFER_TTL_SEC = int(os.environ.get("CALL_OFFER_TTL_SEC", "60"))
# Retention enforces BOTH a quantity cap and an age cap (a message is pruned if
# it's beyond its per-type newest-N per device OR older than the age window).
# Per-TYPE quantity caps because fall "Event"s carry MP4 video (~MB each) and
# are the real disk hog, so they keep fewer (25); Voice/Wake/PillAck/CallRequest
# are text or small audio, so they keep 50. subscription_history is a tiny audit
# log — quantity-capped only (NO age, so old membership records survive).
_EVENT_KEEP_PER_DEVICE = int(os.environ.get("EVENT_KEEP_PER_DEVICE", "25"))   # fall, has video
_MSG_KEEP_PER_DEVICE   = int(os.environ.get("MSG_KEEP_PER_DEVICE", "50"))     # Voice/Wake/PillAck/CallRequest
_HISTORY_KEEP_PER_DEVICE = int(os.environ.get("HISTORY_KEEP_PER_DEVICE", "50"))  # audit log, no age
_RETENTION_DAYS = int(os.environ.get("RETENTION_DAYS", "30"))

# In-process ephemeral state (single-process design — see module docstring).
validation_codes: dict[str, int] = {}
# Camera-snapshot requests live in Redis (multi-worker safe): keys
#   imgreq:{req_id} = json([device_id, username, ts])   (EX 600)
#   imgreq:dev:{device_id} = req_id                      (device index → O(1) /Live_Device)
_call_mailbox: dict[str, dict] = {}
_call_lock = threading.Lock()

# ── Janus VideoRoom call signalling ──────────────────────────────────────────
# A call is "active" while its device_id is in the Redis hash "janus:calls"
# (value = caregiver). Redis-backed so multiple API workers share the state.
# /Live_Device reports call_pending from this, so the device joins the Janus room.
_JANUS_CALLS = "janus:calls"
def room_for(device_id: str) -> int:
    import zlib
    return 100000 + (zlib.crc32(device_id.encode()) % 900000)

# ── Redis (wake-command job queue → GPU worker; shared call state) ───────────
import redis.asyncio as _aioredis
_wake_redis = _aioredis.Redis.from_url(
    os.environ.get("REDIS_URL", "redis://127.0.0.1:6379"), decode_responses=True)


async def _classify_via_queue(wav_path: str, timeout: int = 30):
    """Enqueue the wav for the GPU worker and wait for its result. The API no
    longer holds the models — the worker (wake_worker.py) does — so the API can
    run multiple workers + bursts queue instead of serializing in-process."""
    import uuid
    jid = uuid.uuid4().hex
    await _wake_redis.rpush("wake:jobs", json.dumps({"id": jid, "wav_path": wav_path}))
    res = await _wake_redis.blpop(f"wake:res:{jid}", timeout=timeout)
    if res is None:
        raise RuntimeError("wake worker timeout")
    d = json.loads(res[1])
    return d.get("intent", "MESSAGE"), d.get("transcript", "")

# NOTE: GPU serialization now lives in the wake worker (_GPU_LOCK in
# wake_command.py / wake_worker.py), reached via _classify_via_queue. There is
# deliberately no in-process GPU semaphore here — the API never runs inference.

pool: asyncpg.Pool | None = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    global pool
    async def _init_conn(conn):
        # SQLite stored timestamps as TEXT and the device/app still send some
        # as strings ("Message Time"). asyncpg's builtin timestamp codec
        # requires datetime objects, so override with a text codec that
        # accepts either: datetimes are formatted, strings pass through.
        # Reads come back as strings (active_pill_alarms + responses already
        # handle str timestamps).
        def _enc(v):
            if isinstance(v, str):
                return v
            if isinstance(v, datetime):
                return v.strftime("%Y-%m-%d %H:%M:%S.%f")
            return str(v)
        await conn.set_type_codec(
            "timestamp", encoder=_enc, decoder=lambda v: v,
            schema="pg_catalog", format="text")

    pool = await asyncpg.create_pool(DATABASE_URL, min_size=2, max_size=20,
                                     command_timeout=60, init=_init_conn)
    if wake_command is not None:
        try:
            # Fire-and-forget daemon-thread warm-up (don't block startup —
            # blocking the lifespan on the GPU model load hangs uvicorn's
            # "Application startup complete"). First /Wake_Command pays any
            # remaining cold-start; subsequent ones are warm.
            wake_command.warm_up_async()
        except Exception as e:
            print(f"⚠️  wake_command warm-up kickoff failed: {e!r}")
    retention_task = asyncio.create_task(_retention_loop())
    print("[async-server] asyncpg pool ready")
    yield
    retention_task.cancel()
    await pool.close()


app = FastAPI(lifespan=lifespan)


# ── helpers ────────────────────────────────────────────────────────────────
def _now_str() -> str:
    return datetime.now().strftime('%Y-%m-%d %H:%M:%S')


def _is_recent(ts, max_age_s: int) -> bool:
    """True if timestamp `ts` (text or datetime, naive local time like _now_str)
    is within max_age_s of now. Used to derive device online/offline from
    devices.last_seen (device heartbeats ~every 30s)."""
    if not ts:
        return False
    if isinstance(ts, datetime):
        dt = ts
    else:
        dt = None
        for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
            try:
                dt = datetime.strptime(str(ts).strip(), fmt); break
            except ValueError:
                continue
        if dt is None:
            return False
    return (datetime.now() - dt).total_seconds() < max_age_s


def err(payload: dict, status: int) -> JSONResponse:
    return JSONResponse(payload, status_code=status)


def _rowcount(res: str) -> int:
    """Parse asyncpg's command tag ('DELETE 7', 'UPDATE 1') → affected count."""
    tail = res.split()[-1] if res else ""
    return int(tail) if tail.isdigit() else 0


async def json_body(request: Request) -> dict:
    """Parse a JSON request body, returning {} on empty/invalid input instead
    of raising (which would 500). Handlers then validate required fields and
    return a clean 400."""
    try:
        data = await request.json()
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def sha256_hex(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


async def _retention_loop():
    """Hourly prune enforcing BOTH caps: per device, delete non-Voice messages
    that are beyond the newest _MSG_KEEP_PER_DEVICE OR older than
    _RETENTION_DAYS (plus their media files), prune orphan unread rows, and cap
    subscription_history per device. The constants are module ints, so inlining
    them is injection-safe. Voice is excluded — it has its own newest-100 cap."""
    keep_event = _EVENT_KEEP_PER_DEVICE
    keep_msg = _MSG_KEEP_PER_DEVICE
    keep_hist = _HISTORY_KEEP_PER_DEVICE
    days = _RETENTION_DAYS
    while True:
        # Multi-worker safe: only the worker that grabs the lock sweeps each cycle
        # (lock EX < sleep, so it frees before the next round). Single-worker: no-op.
        if not await _wake_redis.set("retention:lock", "1", nx=True, ex=3300):
            await asyncio.sleep(3600); continue
        try:
            # (image-request cleanup is now automatic via Redis key TTL)
            # Per-(device,type) quantity cap (Event=25 because it has video,
            # everything else incl. Voice=50) OR older than `days`. RETURNING the
            # file paths so we delete the uploads too. 'image captured' snapshots
            # are cleaned eagerly in /Message_Check_App, so they're the only type
            # skipped here; Event/Voice/Wake/PillAck/CallRequest are all capped.
            stale = await pool.fetch(
                f"""DELETE FROM messages WHERE id IN (
                        SELECT id FROM (
                            SELECT id, message_type, message_occur_time,
                                ROW_NUMBER() OVER (
                                    PARTITION BY device_id, message_type
                                    ORDER BY id DESC) AS rn
                            FROM messages WHERE message_type <> 'image captured'
                        ) t
                        WHERE t.rn > (CASE WHEN t.message_type = 'Event'
                                           THEN {keep_event} ELSE {keep_msg} END)
                           OR t.message_occur_time < now() - interval '{days} days'
                    )
                    RETURNING id, message_image, message_video""")
            if stale:
                ids = [r["id"] for r in stale]
                await pool.execute(
                    "DELETE FROM unread_messages WHERE message_id = ANY($1::bigint[])", ids)
                # Offload the file deletions to a thread — a large sweep (hundreds
                # of MP4s on a slow disk) would otherwise block the event loop and
                # stall live requests / device heartbeats.
                paths = [p for r in stale
                         for p in (r["message_image"], r["message_video"])
                         if p and os.path.isfile(p)]
                await run_in_threadpool(_remove_files, paths)
                print(f"[retention] pruned {len(ids)} messages "
                      f"(Event cap {keep_event}, other {keep_msg}/device, or >{days}d)")
            # Orphan unread rows whose message was already deleted elsewhere.
            await pool.execute(
                "DELETE FROM unread_messages WHERE message_id NOT IN (SELECT id FROM messages)")
            # Cap subscription_history per device (rows are tiny, no files).
            await pool.execute(
                f"""DELETE FROM subscription_history WHERE id IN (
                        SELECT id FROM (
                            SELECT id, ROW_NUMBER() OVER (
                                PARTITION BY device_id ORDER BY id DESC) AS rn
                            FROM subscription_history
                        ) t WHERE t.rn > {keep_hist}
                    )""")
        except Exception as e:
            logging.error("[retention] loop error: %s", e, exc_info=True)
        await asyncio.sleep(3600)


# ── SMTP (runs in a thread; global PySocks proxy state guarded by _smtp_lock) ─
_smtp_lock = threading.Lock()

def send_validation_code_to_user_email(email, validation_code):
    SMTP_SERVER, SMTP_PORT = "smtp.gmail.com", 465
    SENDER_EMAIL = os.environ.get("SMTP_SENDER_EMAIL", "noorijustfortest1@gmail.com")
    SENDER_PASSWORD = os.environ.get("SMTP_SENDER_PASSWORD", "")
    if not SENDER_PASSWORD:
        print("[smtp] SMTP_SENDER_PASSWORD not set — validation email skipped"); return
    msg = MIMEMultipart()
    msg["From"], msg["To"], msg["Subject"] = SENDER_EMAIL, email, "Your Validation Code"
    msg.attach(MIMEText(
        f"Hello,\n\nYour validation code is: {validation_code}\n\nThank you.", "plain"))
    proxy_host = os.environ.get("SMTP_PROXY_HOST")
    proxy_port = int(os.environ.get("SMTP_PROXY_PORT", "0"))
    proxy_type = os.environ.get("SMTP_PROXY_TYPE", "socks5").lower()
    # socks.set_default_proxy + wrap_module mutate PROCESS-GLOBAL PySocks state.
    # Two concurrent registrations would race (one clobbers the other's proxy
    # mid-connect). Serialize the whole proxy-setup→connect→send under one lock.
    with _smtp_lock:
        if proxy_host and proxy_port:
            try:
                import socks
                kind = socks.HTTP if proxy_type == "http" else socks.SOCKS5
                socks.set_default_proxy(kind, proxy_host, proxy_port)
                socks.wrap_module(smtplib)
            except Exception as e:
                print(f"[smtp] proxy setup failed ({e}) — direct")
        smtp = None
        try:
            smtp = smtplib.SMTP_SSL(SMTP_SERVER, SMTP_PORT, timeout=30)
            smtp.login(SENDER_EMAIL, SENDER_PASSWORD)
            smtp.sendmail(SENDER_EMAIL, email, msg.as_string())
            print(f"[smtp] sent code {validation_code} to {email}", flush=True)
        except Exception as e:
            print(f"[smtp] FAILED to send to {email}: {type(e).__name__}: {e}", flush=True)
        finally:
            if smtp is not None:
                try: smtp.quit()
                except Exception: pass


_VALIDATION_TTL_SEC = 180

def add_member_to_dictionary(username, validation_code):
    # Store (code, expiry_epoch) and expire inline on read, instead of a
    # threading.Timer that mutated this dict from another thread concurrently
    # with the async event loop (a real data race → possible RuntimeError /
    # lost update).
    validation_codes[username] = (validation_code, time.time() + _VALIDATION_TTL_SEC)


def _active_validation_code(username):
    """Return the still-valid code for username, or None (popping if expired)."""
    entry = validation_codes.get(username)
    if entry is None:
        return None
    code, expiry = entry
    if time.time() > expiry:
        validation_codes.pop(username, None)
        return None
    return code


# ── FCM (blocking requests; call via run_in_threadpool) ─────────────────────
def _get_access_token():
    # Path to the Firebase service-account key. Provided at runtime (gitignored;
    # mounted read-only by docker-compose as elderly-care-assistant.json). The
    # default keeps the existing laptop filename working; Docker sets FCM_KEY_FILE.
    key_file = os.environ.get(
        "FCM_KEY_FILE", "elderly-care-assistant-1bab66e453cb.json")
    creds = service_account.Credentials.from_service_account_file(
        key_file, scopes=SCOPES)
    try:
        # google-auth refresh is blocking + unbounded; don't let a bad refresh
        # blow up the whole notification path. GoogleRequest accepts a per-call
        # timeout that the refresh forwards to the underlying HTTP call.
        creds.refresh(GoogleRequest(), timeout=5)
    except TypeError:
        # Older google-auth: Request.__call__ takes timeout but refresh() may
        # not forward it — fall back to a plain refresh.
        creds.refresh(GoogleRequest())
    except Exception as ex:
        print(f"  FCM token refresh failed: {ex!r}")
        return None
    return creds.token


def send_notification(firebase_token, title, body, color="#33FF00"):
    access_token = _get_access_token()
    if access_token is None:
        return None
    url = 'https://fcm.googleapis.com/v1/projects/elderly-care-assistant/messages:send'
    payload = {"message": {
        "token": firebase_token,
        "notification": {"title": title, "body": body},
        "android": {"notification": {"color": color}},
        "data": {"color": color},
    }}
    try:
        # A slow FCM call holds a threadpool thread; enough of them exhaust the
        # pool and stall request handling. Bound it and swallow transport errors.
        resp = requests.post(url, headers={
            'Authorization': f'Bearer {access_token}',
            'Content-Type': 'application/json'}, json=payload, timeout=5)
        return resp.json()
    except (requests.Timeout, requests.RequestException) as ex:
        print(f"  FCM send failed: {ex!r}")
        return None


def _write_file(file_path, data):
    """Blocking file write; call via run_in_threadpool to keep large uploads
    off the async event loop."""
    with open(file_path, "wb") as f:
        f.write(data)


def _remove_files(paths):
    """Blocking best-effort deletion of a list of files; call via
    run_in_threadpool so a large sweep doesn't block the event loop."""
    for p in paths:
        try:
            os.remove(p)
        except OSError:
            pass


def read_file_as_base64(file_path):
    try:
        with open(file_path, "rb") as f:
            return base64.b64encode(f.read()).decode("utf-8")
    except (FileNotFoundError, TypeError):
        return None


# ── auth ────────────────────────────────────────────────────────────────────
def _kh_match(presented: str, stored: str) -> bool:
    """Transition-safe device-credential check (E2E decoupling).

    The device/app used to send KEY_HASH = sha256(device_password), which is
    ALSO the media-encryption key material — so storing it let the server
    derive the media key. They now send AUTH = sha256(KEY_HASH) instead and
    keep KEY_HASH local for media only. During the cutover this accepts either
    form with no lockout window: presented == stored (legacy KEY_HASH, or the
    already-migrated AUTH), or presented == sha256(stored) (new AUTH vs a row
    still holding KEY_HASH). After the one-time key_hash->sha256(key_hash)
    migration the first branch handles the new token; the second is then inert.
    """
    if not presented or not stored:
        return False
    if hmac.compare_digest(presented, stored):
        return True
    return hmac.compare_digest(
        presented, hashlib.sha256(stored.encode()).hexdigest())


async def verify_device_auth(request: Request):
    """Returns (device_record, err_response_or_None). On failure the second
    element is a JSONResponse the caller should return directly."""
    h = request.headers
    device_id = h.get("Device-Id") or h.get("Device-ID")
    key_hash = h.get("Key-Hash")
    legacy = h.get("Authentication-Code")
    if not device_id or (not key_hash and not legacy):
        return None, err({"error": "Device-Id or credentials missing"}, 400)
    dev = await pool.fetchrow(
        "SELECT device_id, key_hash FROM devices WHERE device_id = $1", device_id)
    if dev is None:
        return None, err({"error": "Invalid Device"}, 403)
    presented = key_hash or hashlib.sha256(legacy.encode()).hexdigest()
    if not _kh_match(presented, dev["key_hash"]):
        return None, err({"error": "Invalid credentials"}, 403)
    return dev, None


_pw_cache: dict = {}            # username -> (sha256hex(pw), expiry_ts)
_PW_CACHE_TTL = 300

def _looks_bcrypt(h) -> bool:
    return isinstance(h, str) and h.startswith(("$2a$", "$2b$", "$2y$"))

def _hash_password(pw: str) -> str:
    return bcrypt.hashpw(pw.encode(), bcrypt.gensalt()).decode()

async def _verify_user_password(username: str, password: str) -> bool:
    """Verify caregiver creds against bcrypt-hashed storage, transparently
    upgrading any legacy plaintext row on first successful login. A short
    in-memory cache keeps bcrypt off the per-request hot path (creds ride every
    API call). bcrypt runs in the threadpool so it never blocks the event loop."""
    now = time.time()
    h = hashlib.sha256(password.encode()).hexdigest()
    c = _pw_cache.get(username)
    if c and c[1] > now and hmac.compare_digest(c[0], h):
        return True
    row = await pool.fetchrow("SELECT password FROM users WHERE username=$1", username)
    if row is None:
        return False
    stored = row["password"] or ""
    if _looks_bcrypt(stored):
        ok = await run_in_threadpool(bcrypt.checkpw, password.encode(), stored.encode())
    else:
        # encode both → compare_digest TypeErrors on non-ASCII str (Persian pw)
        ok = hmac.compare_digest(stored.encode("utf-8"), password.encode("utf-8"))
        if ok:                                        # upgrade it in place
            new_hash = await run_in_threadpool(_hash_password, password)
            await pool.execute("UPDATE users SET password=$1 WHERE username=$2",
                               new_hash, username)
            _pw_cache.pop(username, None)             # drop stale cache entry
    if ok:
        _pw_cache[username] = (h, now + _PW_CACHE_TTL)
    return ok


async def check_user_creds(request: Request):
    """Returns (username, err_response_or_None)."""
    username = request.headers.get("Username")
    password = request.headers.get("Password")
    if not username or not password:
        return None, err({"error": "Username/Password missing"}, 400)
    if not await _verify_user_password(username, password):
        return None, err({"error": "Invalid Username/Password"}, 403)
    return username, None

_SAFE_ID = _re.compile(r"^[A-Za-z0-9_-]{1,64}$")
def _safe_device_id(device_id: str) -> bool:
    """device_id is used in filesystem paths/globs — restrict to a safe
    charset so it can never traverse directories or inject glob/shell."""
    return bool(device_id and _SAFE_ID.match(device_id))


# ── pill scheduler (async port of _active_pill_alarms_for) ──────────────────
async def active_pill_alarms_for(device_id: str) -> list:
    rows = await pool.fetch(
        "SELECT id, pill_id_local, name, start_at_utc, interval_seconds, "
        "total_count, consumptions_done FROM pill_schedules WHERE device_id = $1",
        device_id)
    now = datetime.now(timezone.utc).replace(tzinfo=None)
    active, dirty = [], []
    for r in rows:
        start_at = r["start_at_utc"]
        if isinstance(start_at, str):
            # The custom "timestamp" codec (see _init_conn) decodes to raw PG
            # text, which carries fractional seconds when the stored value has
            # them (e.g. "2026-06-04 12:34:08.51"). Accept both forms — a plain
            # "%Y-%m-%d %H:%M:%S" parse would throw on the ".51" and silently
            # drop the dose, so the pill would never alarm.
            parsed = None
            for _fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
                try:
                    parsed = datetime.strptime(start_at, _fmt); break
                except ValueError:
                    continue
            if parsed is None:
                continue
            start_at = parsed
        interval, total, done = int(r["interval_seconds"]), int(r["total_count"]), int(r["consumptions_done"])
        if interval <= 0 or total <= 0:
            continue
        while done < total:
            next_due = start_at + timedelta(seconds=done * interval)
            if (now - next_due).total_seconds() > _PILL_ACTIVE_WINDOW_SEC:
                done += 1; continue
            break
        if done != r["consumptions_done"]:
            # ($1, $2) → (consumptions_done, id) to match the UPDATE below.
            dirty.append((done, r["id"]))
        if done >= total:
            continue
        next_due = start_at + timedelta(seconds=done * interval)
        if next_due <= now <= next_due + timedelta(seconds=_PILL_ACTIVE_WINDOW_SEC):
            active.append({
                "id": r["id"], "pill_id": r["pill_id_local"], "name": r["name"],
                "due_at": next_due.strftime("%Y-%m-%d %H:%M:%S"),
                "audio_url": f"/Pill_Audio/{r['id']}", "dose": done + 1, "of": total,
            })
    if dirty:
        await pool.executemany(
            "UPDATE pill_schedules SET consumptions_done = $1 WHERE id = $2", dirty)
    return active


# ── call mailbox helpers (in-process) ───────────────────────────────────────
def _mailbox(device_id):
    box = _call_mailbox.get(device_id)
    if box is None:
        box = {"offer": None, "answer": None, "app_ice": [], "device_ice": [],
               "in_call_by": None, "started_at": None}
        _call_mailbox[device_id] = box
    return box


def _autoclear_stale_call(box):
    now = datetime.now()
    # Unanswered offer gone stale → the call never connected. Clear the whole
    # box so call_pending drops to false and the device stops looping the helper.
    if box.get("offer") and not box.get("answer") and box.get("started_at"):
        if (now - box["started_at"]).total_seconds() > _CALL_OFFER_TTL_SEC:
            box["offer"] = None
            box["answer"] = None
            box["in_call_by"] = None
            box["started_at"] = None
            box["app_ice"] = []
            box["device_ice"] = []
            return
    if box["in_call_by"] and box["started_at"]:
        if (now - box["started_at"]).total_seconds() > _CALL_MAX_DURATION_SEC:
            box["in_call_by"] = None
            box["started_at"] = None


# ── notification fan-out (async) ────────────────────────────────────────────
async def fanout_message(device_id, msg_id, fcm_title, fcm_body, fcm_color):
    subs = await pool.fetch(
        "SELECT username FROM subscriptions WHERE device_id = $1", device_id)
    for s in subs:
        username = s["username"]
        await pool.execute(
            "INSERT INTO unread_messages (user_username, message_id) VALUES ($1, $2)",
            username, msg_id)
        urow = await pool.fetchrow(
            "SELECT firebase_token FROM users WHERE username = $1", username)
        if urow and urow["firebase_token"]:
            try:
                await run_in_threadpool(send_notification, urow["firebase_token"],
                                        fcm_title, fcm_body, fcm_color)
            except Exception as e:
                print(f"  FCM send to {username} failed: {e}")


# ════════════════════════════════════════════════════════════════════════════
# DEVICE ENDPOINTS
# ════════════════════════════════════════════════════════════════════════════
@app.get("/Live_Device")
async def live_device(request: Request):
    dev, e = await verify_device_auth(request)
    if e: return e
    device_id = dev["device_id"]
    await pool.execute("UPDATE devices SET last_seen = $1 WHERE device_id = $2",
                       _now_str(), device_id)
    # active_devices upsert (backwards-compat snapshot row)
    row = await pool.fetchrow("SELECT 1 FROM active_devices WHERE device_id = $1", device_id)
    if row is None:
        await pool.execute(
            "INSERT INTO active_devices (device_id, key_hash, create_time, "
            "last_message_time, last_live_time, config_file) "
            "VALUES ($1,$2,$3,$4,$5,$6)",
            device_id, dev["key_hash"], _now_str(), None, _now_str(), "")
    else:
        await pool.execute("UPDATE active_devices SET last_live_time = $1 WHERE device_id = $2",
                           _now_str(), device_id)

    # MAC-based re-provision dedup. The device reports its stable wlan MAC; if
    # any OTHER device_id already owns this MAC, it's the SAME physical device
    # under an old identity (re-provisioned / re-registered) — purge that old
    # identity so only the latest hash survives. Same purge as factory reset.
    # The `cur != mac` guard makes this a no-op on every heartbeat after the
    # first, so it costs nothing in steady state.
    mac = request.headers.get("Device-Mac") or request.headers.get("Device-MAC")
    if mac:
        cur = await pool.fetchval("SELECT mac FROM active_devices WHERE device_id=$1", device_id)
        if cur != mac:
            for o in await pool.fetch(
                    "SELECT device_id FROM active_devices WHERE mac=$1 AND device_id<>$2",
                    mac, device_id):
                print(f"[mac-dedup] {device_id} (mac {mac}) supersedes "
                      f"{o['device_id']} — purging old identity")
                await _purge_device(o["device_id"])
            await pool.execute("UPDATE active_devices SET mac=$1 WHERE device_id=$2",
                               mac, device_id)

    # Guest / sleep mode flag (device sends "Guest-Mode: 1|0" each heartbeat).
    # Surfaced to the app so the card can show a "sleeping / not monitoring"
    # state — the caregiver must never assume falls are watched while paused.
    guest_hdr = request.headers.get("Guest-Mode")
    if guest_hdr is not None:
        await pool.execute("UPDATE active_devices SET guest=$1 WHERE device_id=$2",
                           guest_hdr.strip() == "1", device_id)

    # Deliver any pending app-issued guest command (one-shot: consume-on-read, so
    # a later physical-button toggle isn't overridden by a stale request). The
    # device applies it like a button press and reports back via Guest-Mode next
    # heartbeat, which keeps every caregiver's app in sync.
    guest_cmd = await pool.fetchval(
        "SELECT guest_requested FROM active_devices WHERE device_id=$1", device_id)
    if guest_cmd is not None:
        await pool.execute(
            "UPDATE active_devices SET guest_requested=NULL WHERE device_id=$1", device_id)

    image_req, image_req_id = False, None
    # O(1) device-indexed lookup in Redis (shared across API workers).
    _rid = await _wake_redis.get(f"imgreq:dev:{device_id}")
    if _rid:
        # Return as int when numeric so the device's stoi parser gets a
        # bare number, not a quoted string.
        image_req = True
        image_req_id = int(_rid) if str(_rid).isdigit() else _rid

    active_pill_alarms = await active_pill_alarms_for(device_id)

    call_pending, in_call_by = False, None
    with _call_lock:
        box = _call_mailbox.get(device_id)
        if box:
            _autoclear_stale_call(box)
            if box.get("offer") and not box.get("answer"):
                call_pending = True
            in_call_by = box.get("in_call_by")
    # Janus path: an active call request means "call pending" so the device joins
    # the Janus room (and its end-poll stays in the call until the app stops).
    if not call_pending:
        _cg = await _wake_redis.hget(_JANUS_CALLS, device_id)
        if _cg:
            call_pending = True
            in_call_by = _cg

    return {
        "message": "Device live time updated",
        "image_req": image_req, "image_req_id": image_req_id,
        "active_pill_alarms": active_pill_alarms,
        "call_pending": call_pending, "in_call_by": in_call_by,
        # one-shot caregiver voice note to play on the amp (null when none)
        "voice_msg_url": (f"/Voice_To_Device_Audio/{device_id}"
                          if device_id in _pending_voice else None),
        "guest_set": guest_cmd,   # null | true (sleep) | false (wake) — one-shot
    }


@app.post("/Config_Device")
async def config_device(request: Request):
    dev, e = await verify_device_auth(request)
    if e: return e
    device_id = dev["device_id"]
    try:
        config_data = await request.json()
    except Exception:
        return err({"error": "Invalid JSON body"}, 400)
    new_key_hash = config_data.get("key-Hash")
    if not new_key_hash:
        return err({"error": "New key-Hash missing in body"}, 400)
    row = await pool.fetchrow("SELECT 1 FROM active_devices WHERE device_id = $1", device_id)
    if row is None:
        await pool.execute(
            "INSERT INTO active_devices (device_id, key_hash, last_message_time, "
            "last_live_time, config_file) VALUES ($1,$2,$3,$4,$5)",
            device_id, new_key_hash, None, _now_str(), json.dumps(config_data))
        message = "New device added"
    else:
        await pool.execute(
            "UPDATE active_devices SET key_hash=$1, config_file=$2, last_live_time=$3 "
            "WHERE device_id=$4",
            new_key_hash, json.dumps(config_data), _now_str(), device_id)
        message = "Device config updated"
    return {"message": message}


@app.post("/Message_Device")
async def message_device(request: Request):
    dev, e = await verify_device_auth(request)
    if e: return e
    device_id = dev["device_id"]
    row = await pool.fetchrow("SELECT 1 FROM active_devices WHERE device_id = $1", device_id)
    if row is None:
        return err({"error": "No active device found"}, 404)

    form = await request.form()
    message_text = form.get("text")
    message_image = form.get("image file")
    message_video = form.get("video file")
    if not message_text or message_image is None:
        return err({"error": "Missing message text or image file"}, 400)
    try:
        message_data = json.loads(message_text)
        message_time = message_data.get("Message Time")
        message_type = message_data.get("Message Type")
        if not message_time or not message_type:
            raise ValueError("Message Time or Message Type missing")
    except Exception as ex:
        return err({"error": f"Invalid message text: {ex}"}, 400)

    ts = datetime.now().strftime('%Y%m%d%H%M%S')
    image_path = os.path.join(UPLOAD_FOLDER, f"{device_id}_{ts}_image.bin")
    video_path = os.path.join(UPLOAD_FOLDER, f"{device_id}_{ts}_video.bin")
    image_bytes = await message_image.read()
    if len(image_bytes) > _MAX_UPLOAD_BYTES:
        return err({"error": "image too large"}, 413)
    await run_in_threadpool(_write_file, image_path, image_bytes)
    has_video = message_video is not None and hasattr(message_video, "read")
    if has_video:
        video_bytes = await message_video.read()
        if len(video_bytes) > _MAX_UPLOAD_BYTES:
            return err({"error": "video too large"}, 413)
        await run_in_threadpool(_write_file, video_path, video_bytes)

    msg_id = await pool.fetchval(
        "INSERT INTO messages (device_id, message_type, message_text, message_image, "
        "message_video, message_occur_time) VALUES ($1,$2,$3,$4,$5,$6) RETURNING id",
        device_id, message_type, message_text, image_path,
        video_path if has_video else "", message_time)

    if message_type == "image captured":
        image_req_id = message_data.get("image_req_id")
        if image_req_id is None:
            return err({"error": "image_req_id missing for image captured message"}, 400)
        _raw = await _wake_redis.get(f"imgreq:{image_req_id}")
        if _raw:
            req_data = json.loads(_raw)
            _did, username = req_data[0], req_data[1]   # value is [device_id, username, ts]
            await pool.execute(
                "INSERT INTO unread_messages (user_username, message_id) VALUES ($1,$2)",
                username, msg_id)
            await _wake_redis.delete(f"imgreq:{image_req_id}", f"imgreq:dev:{_did}")
        else:
            # The request was already fulfilled (a prior frame won the race)
            # or it expired. Clear THIS device's pending flag so it stops
            # re-capturing on every 100 ms heartbeat — otherwise a request the
            # device can never satisfy would flood the server with uploads —
            # and drop the orphan blob we just stored, since no subscriber will
            # ever receive it. Best-effort: never let cleanup break the reply.
            await _wake_redis.delete(f"imgreq:dev:{device_id}")
            try:
                await pool.execute("DELETE FROM messages WHERE id=$1", msg_id)
                await run_in_threadpool(os.remove, image_path)
                if has_video:
                    await run_in_threadpool(os.remove, video_path)
            except Exception:
                pass
            return err({"error": "No user found for image_req_id"}, 404)
    elif message_type == "Event":
        await _fanout_subscribers(device_id, msg_id)
        mqtt_pub.event_fall(device_id, msg_id, message_time,
                            image_url=f"/Media/{os.path.basename(image_path)}",
                            video_url=f"/Media/{os.path.basename(video_path)}" if has_video else "")
    elif message_type == "Wake up word":
        await _fanout_subscribers(device_id, msg_id)
        mqtt_pub.event_wake_command(device_id, "wake", message_text or "", msg_id)

    return {"message": "Message recorded successfully"}


async def _fanout_subscribers(device_id, msg_id):
    """Insert msg into every subscriber's unread queue + fire FCM check."""
    subs = await pool.fetch("SELECT username FROM subscriptions WHERE device_id=$1", device_id)
    # The message is the same for every subscriber — fetch + parse it ONCE
    # (was re-fetched per subscriber) and build the notification once.
    mrow = await pool.fetchrow("SELECT message_text FROM messages WHERE id=$1", msg_id)
    try:
        mt = json.loads(mrow["message_text"]) if mrow else {}
    except Exception:
        mt = {}
    mtype = mt.get("Message Type", "_")
    notify = None  # (title, body, color) — None if this type sends no push
    if mtype == "Event":
        lvl = mt.get("Danger Level", "Unknown")
        dl = {0: "بدون خطر جدی", 1: "وضعیت نامشخص", 2: "خطر جدی"}.get(lvl, "")
        color = {0: "#33FF00", 1: "#FFA500", 2: "#FF0000"}.get(lvl, "#33FF00")
        # Samsung/One UI ignores the notification accent color, so encode
        # severity as a colored emoji in the TITLE — visible on every device.
        emoji = {0: "🟢", 1: "🟠", 2: "🔴"}.get(lvl, "⚪")
        notify = (f"{emoji} هشدار سقوط", f"میزان خطر: {dl}", color)
    elif mtype == "Wake up word":
        title = mt.get("Event Title") or mt.get("Message Title") or "New Event"
        notify = (title, mt.get("Message Text", ""), "#33FF00")

    tokens = []
    for s in subs:
        await pool.execute(
            "INSERT INTO unread_messages (user_username, message_id) VALUES ($1,$2)",
            s["username"], msg_id)
        if notify is not None:
            urow = await pool.fetchrow(
                "SELECT firebase_token FROM users WHERE username=$1", s["username"])
            if urow and urow["firebase_token"]:
                tokens.append(urow["firebase_token"])

    # Fan out all FCM sends concurrently — was sequential (N subscribers × up to
    # 5s each). send_notification swallows its own transport errors → None.
    if notify is not None and tokens:
        title, body, color = notify
        await asyncio.gather(*[
            run_in_threadpool(send_notification, t, title, body, color)
            for t in tokens
        ], return_exceptions=True)


@app.post("/Voice_Message_Device")
async def voice_message_device(request: Request):
    dev, e = await verify_device_auth(request)
    if e: return e
    device_id = dev["device_id"]
    form = await request.form()
    audio = form.get("audio")
    if audio is None:
        return err({"error": "Missing audio file"}, 400)
    ts = datetime.now().strftime('%Y%m%d%H%M%S')
    audio_path = os.path.join(UPLOAD_FOLDER, f"{device_id}_{ts}_voice.m4a")
    audio_bytes = await audio.read()
    if len(audio_bytes) > _MAX_UPLOAD_BYTES:
        return err({"error": "audio too large"}, 413)
    await run_in_threadpool(_write_file, audio_path, audio_bytes)
    occurred = _now_str()
    msg_id = await pool.fetchval(
        "INSERT INTO messages (device_id, message_type, message_text, message_image, "
        "message_video, message_occur_time) VALUES ($1,$2,$3,$4,$5,$6) RETURNING id",
        device_id, "Voice",
        json.dumps({"Message Time": occurred, "Message Type": "Voice"}),
        "", audio_path, occurred)
    subs = await pool.fetch("SELECT username FROM subscriptions WHERE device_id=$1", device_id)
    for s in subs:
        await pool.execute(
            "INSERT INTO unread_messages (user_username, message_id) VALUES ($1,$2)",
            s["username"], msg_id)
    mqtt_pub.event_voice(device_id, msg_id, occurred,
                         audio_url=f"/Media/{os.path.basename(audio_path)}")
    for s in subs:
        mqtt_pub.notify_user(s["username"],
                             {"type": "voice", "device_id": device_id, "message_id": msg_id})
    return {"message": "voice stored", "message_id": msg_id}


# ════════════════════════════════════════════════════════════════════════════
# APP (user) ENDPOINTS
# ════════════════════════════════════════════════════════════════════════════
@app.post("/Registeration_App")
async def registeration_app(request: Request):
    packet_type = request.headers.get("Packet")
    username = request.headers.get("Username")
    try:
        data = await json_body(request)
    except Exception:
        data = {}
    if not packet_type or not username:
        return err({"error": "Packet or Username missing"}, 400)

    if packet_type == "Create_Account":
        exists = await pool.fetchrow("SELECT 1 FROM users WHERE username=$1", username)
        if exists is not None:
            return err({"error": "Username already exists"}, 409)
        if _active_validation_code(username) is not None:
            return err({"error": "You have to wait for the time-out to pass then you can send another request"}, 403)
        code = random.randint(100000, 999999)
        add_member_to_dictionary(username, code)
        threading.Thread(target=send_validation_code_to_user_email,
                         args=(username, code), daemon=True).start()
        return {"message": "Validation code sent to email"}

    elif packet_type == "Validation_Code":
        user_code = data.get("Validation_Code")
        user_password = data.get("Set_Password")
        if not user_password:
            return err({"error": "Set_Password required"}, 400)
        valid = _active_validation_code(username)
        if valid is None:
            return err({"error": "Invalid validation code"}, 403)
        # Guard int() — a missing/non-numeric code must 403, not 500.
        try:
            supplied = int(user_code)
        except (TypeError, ValueError):
            return err({"error": "Invalid validation code"}, 403)
        if valid != supplied:
            validation_codes.pop(username, None)
            return err({"error": "Invalid validation code"}, 403)
        try:
            await pool.execute(
                "INSERT INTO users (username, password, last_login_time, "
                "last_message_time, last_message_check_time, config_file) "
                "VALUES ($1,$2,$3,$4,$5,$6)",
                username, await run_in_threadpool(_hash_password, user_password),
                None, None, None, '{}')
        except asyncpg.UniqueViolationError:
            return err({"error": "Username already exists"}, 409)
        validation_codes.pop(username, None)
        return {"message": "User registered successfully"}
    else:
        return err({"error": "Invalid packet type"}, 400)


@app.post("/Login_App")
async def login_app(request: Request):
    username = request.headers.get("Username")
    password = request.headers.get("Password")
    if not username or not password:
        return err({"error": "Username or Password missing"}, 400)
    # Select only the column we return — SELECT * also pulled the bcrypt hash and
    # firebase_token into the response path. (_verify_user_password does its own
    # password lookup.)
    user = await pool.fetchrow("SELECT config_file FROM users WHERE username=$1", username)
    if user is None or not await _verify_user_password(username, password):
        return err({"error": "Invalid username or password"}, 403)

    devices = await pool.fetch("SELECT device_id FROM subscriptions WHERE username=$1", username)
    device_data = {}
    for d in devices:
        device_id = d["device_id"]
        # Bound the login payload: newest N fall Events per device (matches the
        # retention cap). Without LIMIT a long-lived device returned every event
        # ever (tens of MB of base64) and blocked the loop.
        messages = await pool.fetch(
            "SELECT * FROM messages WHERE device_id=$1 AND message_type=$2 "
            "ORDER BY id DESC LIMIT $3", device_id, "Event", _EVENT_KEEP_PER_DEVICE)
        device_data[device_id] = []
        for m in messages:
            device_data[device_id].append({
                "device_id": m["device_id"], "message_id": m["id"],
                "message_type": m["message_type"], "message_text": m["message_text"],
                "message_image": await run_in_threadpool(
                    read_file_as_base64, m["message_image"]),
                "has_video": bool(m["message_video"]),  # MP4 fetched on demand
                "message_occur_time": str(m["message_occur_time"]),
            })
    await pool.execute("UPDATE users SET last_login_time=$1 WHERE username=$2",
                       _now_str(), username)
    # Always return the full envelope — a user whose devices simply have no
    # fall Events yet (e.g. a freshly provisioned device) must still get their
    # config + subscribed device list, or the app sees an empty login.
    return {
        "message": "Login to account successfully completed.",
        "config_file": json.loads(user["config_file"] or "{}"),
        "subscribed_devices": device_data,
    }


@app.post("/Config_App")
async def config_app(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    try:
        config_data = await request.json()
    except Exception:
        config_data = None
    if not config_data:
        return err({"error": "Missing configuration data"}, 400)
    firebase_token = config_data.get("firebase_token")
    # An FCM token is unique to a phone install, but it's stored per-user. If a
    # caregiver logs out and a different account logs in on the SAME phone, the
    # token would otherwise live on BOTH user rows — and events for the old
    # user's device would get pushed to whoever is now on that phone (a
    # cross-user notification leak). So "claim" the token: NULL it on every
    # other user before assigning it here. Guarantees token→exactly-one-user.
    async with pool.acquire() as conn:
        async with conn.transaction():
            if firebase_token:
                await conn.execute(
                    "UPDATE users SET firebase_token=NULL "
                    "WHERE firebase_token=$1 AND username<>$2",
                    firebase_token, username)
            await conn.execute(
                "UPDATE users SET config_file=$1, firebase_token=$2 WHERE username=$3",
                json.dumps(config_data), firebase_token, username)
    return {"message": "Configuration updated successfully"}


@app.post("/Message_Check_App")
async def message_check_app(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    unread = await pool.fetch(
        "SELECT e.id AS message_id, e.message_type, e.message_text, e.message_image, "
        "e.message_video, e.message_occur_time, e.device_id "
        "FROM unread_messages ue JOIN messages e ON ue.message_id = e.id "
        "WHERE ue.user_username = $1", username)
    if not unread:
        return {"message": "No new messages found"}
    message_list, ids, image_paths = [], [], []
    for m in unread:
        # Don't inline the (large) video — only the image thumbnail. The app
        # shows the thumbnail and lazily downloads the MP4 via /Fall_Video_App
        # when the user taps the event. `has_video` tells it one is available.
        message_list.append({
            "device_id": m["device_id"], "message_id": m["message_id"],
            "message_type": m["message_type"], "message_text": m["message_text"],
            "message_image": await run_in_threadpool(
                read_file_as_base64, m["message_image"]),
            "has_video": bool(m["message_video"]),
            "message_occur_time": str(m["message_occur_time"]),
        })
        ids.append(m["message_id"])
        if m["message_type"] == "image captured":
            image_paths.append(m["message_image"])
    await pool.executemany(
        "DELETE FROM unread_messages WHERE user_username=$1 AND message_id=$2",
        [(username, i) for i in ids])
    for p in image_paths:
        try: os.remove(p)
        except Exception: pass
    return {"new_messages": message_list}


@app.post("/Subscribe_App")
async def subscribe_app(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    data = await json_body(request)
    device_id = data.get("device_ID")
    key_hash = data.get("Key_Hash")
    if not device_id or not key_hash:
        return err({"error": "Missing device_ID or Key_Hash"}, 400)
    device = await pool.fetchrow("SELECT key_hash FROM active_devices WHERE device_id=$1", device_id)
    if device is None or not _kh_match(key_hash, device["key_hash"]):
        return err({"error": "Invalid device ID or Key_Hash"}, 403)
    existing = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2", username, device_id)
    if existing:
        return {"message": "Subscription already exists"}
    await pool.execute("INSERT INTO subscriptions (username, device_id) VALUES ($1,$2)",
                       username, device_id)
    return {"message": "Subscription added successfully", "device_ID": device_id,
            "message_archive": []}


@app.post("/Image_Request_App")
async def image_request_app(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    data = await json_body(request)
    device_id = data.get("device_id")
    req_id = data.get("req_id")
    if not device_id or req_id is None:
        return err({"error": "Missing device_id or req_id"}, 400)
    sub = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2", username, device_id)
    if sub is None:
        return err({"error": "User is not subscribed to the device"}, 403)
    import json as _json
    await _wake_redis.set(f"imgreq:{req_id}", _json.dumps([device_id, username, time.time()]), ex=600)
    await _wake_redis.set(f"imgreq:dev:{device_id}", str(req_id), ex=600)
    return {"message": "Image request added successfully"}


@app.post("/Voice_Messages_App")
async def voice_messages_app(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    if not device_id:
        return err({"error": "device_id required"}, 400)
    sub = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2", username, device_id)
    if sub is None:
        return err({"error": "Not subscribed to this device"}, 403)
    # Voice retention (quantity + age) is handled centrally by _retention_loop
    # now, so this endpoint just reads. LIMIT matches the Voice quantity cap.
    rows = await pool.fetch(
        f"SELECT id, message_occur_time, message_video, message_text FROM messages "
        f"WHERE device_id=$1 AND message_type='Voice' ORDER BY id DESC LIMIT {_MSG_KEEP_PER_DEVICE}",
        device_id)
    out = []
    for r in rows:
        path = r["message_video"]
        # Read + base64-encode off the event loop (was a synchronous read of the
        # full audio per row, up to 50 rows, blocking all other requests).
        b64 = await run_in_threadpool(read_file_as_base64, path) if path else None
        encrypted = False
        try:
            encrypted = json.loads(r["message_text"]).get("encrypted") is True
        except Exception:
            pass
        out.append({"id": r["id"], "occurred": str(r["message_occur_time"]),
                    "audio_b64": b64, "encrypted": encrypted})
    return {"messages": out}


@app.post("/Fall_Video_App")
async def fall_video_app(request: Request):
    """On-demand fall-clip download. The event feed only carries the JPEG
    thumbnail; the app calls this (with the event's message_id) when the user
    taps an item, so the large MP4 isn't transferred until actually wanted.
    Auth: subscriber of the device. Returns base64 video + the encrypted flag."""
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    message_id = body.get("message_id")
    if not device_id or message_id is None:
        return err({"error": "device_id and message_id required"}, 400)
    sub = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2", username, device_id)
    if sub is None:
        return err({"error": "not_subscribed"}, 403)
    row = await pool.fetchrow(
        "SELECT message_video, message_text FROM messages WHERE id=$1 AND device_id=$2",
        int(message_id) if str(message_id).isdigit() else -1, device_id)
    if row is None or not row["message_video"] or not os.path.isfile(row["message_video"]):
        return err({"error": "video not found"}, 404)
    # Read + encode the (multi-MB) MP4 off the event loop.
    b64 = await run_in_threadpool(read_file_as_base64, row["message_video"])
    encrypted = False
    try:
        encrypted = json.loads(row["message_text"]).get("encrypted") is True
    except Exception:
        pass
    return {"video_b64": b64, "encrypted": encrypted}


# ── pills ────────────────────────────────────────────────────────────────────
@app.post("/Pill_Sync_App")
async def pill_sync_app(request: Request, text: str = Form(...),
                        audio_file: UploadFile | None = File(None, alias="audio file")):
    username, e = await check_user_creds(request)
    if e: return e
    try:
        data = json.loads(text)
    except Exception:
        return err({"error": "invalid JSON in text field"}, 400)
    device_id = data.get("device_id")
    pill_id_local = data.get("pill_id_local")
    name = data.get("name", "")
    start_at_utc = data.get("start_at_utc")
    interval_seconds = data.get("interval_seconds")
    total_count = data.get("total_count")
    if (not device_id or pill_id_local is None or not start_at_utc
            or not interval_seconds or not total_count):
        return err({"error": "device_id, pill_id_local, start_at_utc, interval_seconds, total_count required"}, 400)
    # Only a subscriber of this device may edit its pill schedule.
    sub = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2", username, device_id)
    if sub is None:
        return err({"error": "not_subscribed"}, 403)
    try:
        start_dt = datetime.strptime(start_at_utc, "%Y-%m-%d %H:%M:%S")
    except Exception:
        return err({"error": "start_at_utc must be 'YYYY-MM-DD HH:MM:SS' UTC"}, 400)

    audio_path = ""
    if audio_file is not None:
        ts = datetime.now().strftime("%Y%m%d%H%M%S")
        # Sanitize both components — they form a filesystem path; an unsanitized
        # pill_id_local like "../../x" would escape PILL_AUDIO_DIR. The served
        # path is the DB-stored audio_path, so this stays self-consistent.
        safe_dev = _safe_name(device_id)
        safe_pid = _safe_name(pill_id_local)
        raw_path = os.path.join(PILL_AUDIO_DIR, f"{safe_dev}_{safe_pid}_{ts}.raw")
        wav_path = os.path.join(PILL_AUDIO_DIR, f"{safe_dev}_{safe_pid}.wav")
        await run_in_threadpool(_write_file, raw_path, await audio_file.read())
        # Pill reminders are played to the elderly user via aplay on the device
        # (MAX98357A I2S DAC, which runs 48 kHz natively — same as live calls).
        # This is human-heard audio, NOT an ML-pipeline input, so transcode at
        # 48 kHz for full fidelity instead of the 16 kHz used for Whisper STT.
        rc = await run_in_threadpool(lambda: subprocess.run(
            ["ffmpeg", "-y", "-i", raw_path, "-acodec", "pcm_s16le",
             "-ar", "48000", "-ac", "1", wav_path],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode)
        try: os.remove(raw_path)
        except OSError: pass
        if rc == 0:
            audio_path = wav_path
        else:
            return err({"error": "ffmpeg transcode failed"}, 500)

    await pool.execute(
        """INSERT INTO pill_schedules
              (device_id, pill_id_local, name, audio_path,
               start_at_utc, interval_seconds, total_count, consumptions_done)
           VALUES ($1,$2,$3,$4,$5,$6,$7,0)
           ON CONFLICT (device_id, pill_id_local) DO UPDATE SET
              name=excluded.name,
              audio_path=COALESCE(NULLIF(excluded.audio_path,''), pill_schedules.audio_path),
              start_at_utc=excluded.start_at_utc,
              interval_seconds=excluded.interval_seconds,
              total_count=excluded.total_count,
              consumptions_done=0, last_ack_at=NULL""",
        device_id, pill_id_local, name, audio_path,
        start_dt, int(interval_seconds), int(total_count))
    mqtt_pub.state_pill(device_id, await active_pill_alarms_for(device_id))
    return {"message": "pill synced"}


@app.post("/Pill_Ack_Device")
async def pill_ack_device(request: Request):
    # Authenticate the device by Device-Id + Key-Hash (the device already sends
    # both) so a forged ack can't advance a patient's dose count.
    dev, e = await verify_device_auth(request)
    if e: return e
    device_id = dev["device_id"]
    body = await json_body(request)
    target_id = body.get("pill_schedule_id")
    now_str = _now_str()
    if target_id is not None:
        res = await pool.execute(
            "UPDATE pill_schedules SET last_ack_at=$1, consumptions_done=consumptions_done+1 "
            "WHERE id=$2 AND device_id=$3 AND consumptions_done < total_count",
            now_str, target_id, device_id)
        # asyncpg returns "UPDATE N"; parse N rather than endswith("1") (which
        # is also true for 11, 21, …).
        affected = int(res.split()[-1]) if res and res.split()[-1].isdigit() else 0
        acked_ids = [target_id] if affected >= 1 else []
    else:
        active = await active_pill_alarms_for(device_id)
        acked_ids = [a["id"] for a in active]
        if acked_ids:
            await pool.execute(
                "UPDATE pill_schedules SET last_ack_at=$1, consumptions_done=consumptions_done+1 "
                "WHERE device_id=$2 AND id = ANY($3::bigint[]) AND consumptions_done < total_count",
                now_str, device_id, acked_ids)
    mqtt_pub.state_pill(device_id, await active_pill_alarms_for(device_id))
    return {"acked": len(acked_ids), "pill_schedule_ids": acked_ids}


@app.post("/Pill_Delete_App")
async def pill_delete_app(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    pill_id_local = body.get("pill_id_local")
    if not device_id or pill_id_local is None:
        return err({"error": "device_id and pill_id_local required"}, 400)
    sub = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2", username, device_id)
    if sub is None:
        return err({"error": "not_subscribed"}, 403)
    row = await pool.fetchrow(
        "SELECT audio_path FROM pill_schedules WHERE device_id=$1 AND pill_id_local=$2",
        device_id, pill_id_local)
    if row and row["audio_path"] and os.path.exists(row["audio_path"]):
        try: os.remove(row["audio_path"])
        except OSError: pass
    await pool.execute("DELETE FROM pill_schedules WHERE device_id=$1 AND pill_id_local=$2",
                       device_id, pill_id_local)
    mqtt_pub.state_pill(device_id, await active_pill_alarms_for(device_id))
    return {"message": "pill deleted"}


@app.post("/Pill_Status_App")
async def pill_status_app(request: Request):
    """Per-pill consumption progress for a device's course list. The app stores
    the schedule locally but consumptions_done lives server-side (advanced by
    device acks), so the pills page calls this to render "{done}/{total}".
    Returns a map keyed by pill_id_local (the app's local id)."""
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    if not device_id:
        return err({"error": "device_id required"}, 400)
    sub = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2", username, device_id)
    if sub is None:
        return err({"error": "not_subscribed"}, 403)
    rows = await pool.fetch(
        "SELECT pill_id_local, consumptions_done, total_count "
        "FROM pill_schedules WHERE device_id=$1", device_id)
    # `pending` = at least one dose is currently due AND unacked (drives the
    # binary 💊 disclaimer on the device card). Cleared when the elder takes it
    # and says the pill command (PillAck advances consumptions_done → no longer
    # in the active window). One ack clears all due doses by design.
    active = await active_pill_alarms_for(device_id)
    # call_pending: an unanswered call request exists (cross-caregiver; set on a
    # CALL command, cleared when any caregiver calls back via /Call_Offer).
    row = await pool.fetchrow(
        "SELECT call_requested, guest FROM active_devices WHERE device_id=$1", device_id)
    call_req = row["call_requested"] if row else None
    guest = bool(row["guest"]) if row else False
    # online = device heartbeated recently. It polls /Live_Device ~every 30s, so
    # 90s (~3 missed beats) is the offline threshold the app uses to show the
    # "device disconnected" banner; it clears automatically when beats resume.
    last_seen = await pool.fetchval(
        "SELECT last_seen FROM devices WHERE device_id=$1", device_id)
    online = _is_recent(last_seen, 90)
    return {"status": {
        str(r["pill_id_local"]): {"done": r["consumptions_done"], "total": r["total_count"]}
        for r in rows
    }, "pending": len(active) > 0, "call_pending": call_req is not None, "guest": guest,
       "online": online, "last_seen": last_seen}


@app.post("/Set_Guest_App")
async def set_guest_app(request: Request):
    """Caregiver toggles the device's guest/sleep mode remotely. Stores a
    one-shot command the device consumes on its next /Live_Device heartbeat
    (see live_device). The device's reported `guest` (in Pill_Status_App) is the
    source of truth for the UI — this just requests the change."""
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    want = body.get("guest")
    if not device_id or not isinstance(want, bool):
        return err({"error": "device_id and guest(bool) required"}, 400)
    sub = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2", username, device_id)
    if sub is None:
        return err({"error": "not_subscribed"}, 403)
    await pool.execute(
        "UPDATE active_devices SET guest_requested=$1 WHERE device_id=$2", want, device_id)
    return {"message": "guest command queued", "guest_requested": want}


# ── media ────────────────────────────────────────────────────────────────────
@app.get("/Media/{filename:path}")
async def serve_media(request: Request, filename: str):
    # Require valid user credentials — patient media (image/video/voice) must
    # not be served to anonymous callers who guess a filename. (basename()
    # already neutralizes path traversal.)
    _, e = await check_user_creds(request)
    if e: return e
    safe = os.path.basename(filename)
    full = os.path.join(UPLOAD_FOLDER, safe)
    if not os.path.exists(full):
        return err({"error": "not_found"}, 404)
    return FileResponse(full)


@app.get("/Pill_Audio/{pill_db_id}")
async def pill_audio(request: Request, pill_db_id: int):
    # Device-authenticated, and the pill must belong to THIS device — so a
    # device can't enumerate other devices' pill-reminder audio by integer id.
    dev, e = await verify_device_auth(request)
    if e: return e
    row = await pool.fetchrow(
        "SELECT audio_path, device_id FROM pill_schedules WHERE id=$1", pill_db_id)
    if row is None or row["device_id"] != dev["device_id"] \
            or not row["audio_path"] or not os.path.exists(row["audio_path"]):
        return err({"error": "audio not found"}, 404)
    return FileResponse(row["audio_path"], media_type="audio/wav")


@app.post("/Voice_To_Device")
async def voice_to_device(request: Request):
    """Caregiver records a voice note in the app; it's queued (transiently) for
    the device to play once on its amp. Not stored in the DB; overwritten by a
    newer note; deleted as soon as the device fetches it."""
    username, e = await check_user_creds(request)
    if e: return e
    form = await request.form()
    device_id = form.get("device_id")
    audio = form.get("audio")
    if not device_id or audio is None or not hasattr(audio, "read"):
        return err({"error": "device_id and audio required"}, 400)
    sub = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE username=$1 AND device_id=$2",
        username, device_id)
    if sub is None:
        return err({"error": "not_subscribed"}, 403)
    data = await audio.read()
    if not data:
        return err({"error": "empty audio"}, 400)
    if len(data) > 8 * 1024 * 1024:          # cap: ~8 MB / a few minutes
        return err({"error": "audio too large"}, 413)
    # Unique filename per note so the device's one-shot delete (a background task
    # that runs after the response is sent) can never delete a *newer* note that
    # arrived in between. Drop any previous un-fetched note on overwrite.
    path = os.path.join(VOICE_TO_DEVICE_DIR,
                        f"{_safe_name(device_id)}_{int(time.time() * 1000)}.wav")
    # Write the new (fully-formed) file off the event loop FIRST, then atomically
    # swap the pending pointer under a lock. Ordering guarantees the pointer never
    # references a half-written file, and the lock closes the get→set race the
    # offloaded write would otherwise open. Finally drop the superseded note.
    await run_in_threadpool(_write_file, path, data)
    async with _pending_voice_lock:
        old = _pending_voice.get(device_id)
        _pending_voice[device_id] = path
    if old and old != path and os.path.exists(old):
        await run_in_threadpool(_remove_files, [old])
    return {"message": "voice queued"}


@app.get("/Voice_To_Device_Audio/{device_id}")
async def voice_to_device_audio(request: Request, device_id: str):
    """Device fetches its pending voice note. One-shot: cleared + the temp file
    deleted right after it's handed over, so nothing lingers."""
    dev, e = await verify_device_auth(request)
    if e: return e
    if dev["device_id"] != device_id:
        return err({"error": "forbidden"}, 403)
    path = _pending_voice.pop(device_id, None)
    if not path or not os.path.exists(path):
        return err({"error": "no pending voice"}, 404)
    from starlette.background import BackgroundTask
    return FileResponse(
        path, media_type="audio/wav",
        background=BackgroundTask(
            lambda p=path: os.path.exists(p) and os.remove(p)))


# ════════════════════════════════════════════════════════════════════════════
# CALL SIGNALING (in-process mailbox)
# ════════════════════════════════════════════════════════════════════════════
async def _call_auth(request: Request):
    """Call signaling is used by BOTH the app (Username/Password) and the device
    (Device-Id/Key-Hash). Accept either; reject anonymous. Returns an err
    response to return directly, or None when authorized."""
    h = request.headers
    if h.get("Device-Id") or h.get("Device-ID"):
        _, e = await verify_device_auth(request); return e
    if h.get("Username"):
        _, e = await check_user_creds(request); return e
    return err({"error": "auth required"}, 401)


@app.post("/Call_Offer")
async def call_offer(request: Request):
    if (e := await _call_auth(request)) is not None: return e
    body = {}
    try: body = await request.json()
    except Exception: pass
    body = body or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    username = request.headers.get("Username")
    if not device_id:
        return err({"error": "device_id required"}, 400)
    stored_offer = False
    with _call_lock:
        box = _mailbox(device_id)
        _autoclear_stale_call(box)
        if "sdp" in body:
            if box["in_call_by"] and box["in_call_by"] != username:
                return err({"error": "in_call_by_other", "in_call_by": box["in_call_by"]}, 409)
            box["in_call_by"] = username or "anonymous"
            box["started_at"] = datetime.now()
            box["offer"] = {"sdp": body["sdp"], "type": body.get("type", "offer")}
            box["answer"] = None; box["app_ice"] = []; box["device_ice"] = []
            mqtt_pub.state_call_pending(device_id, True, box["in_call_by"])
            stored_offer = True
        else:
            return {"offer": box["offer"]}
    # CRITICAL: never hold _call_lock (a sync threading.Lock) across an await.
    # asyncio is single-threaded, so a coroutine suspended at an await while
    # holding this lock deadlocks the whole event loop against any other handler
    # that acquires it synchronously — e.g. the device's constant /Live_Device
    # polling (with _call_lock). Do the DB write AFTER releasing the lock.
    if stored_offer:
        # A caregiver is calling the device back → resolve the call request for
        # ALL caregivers (clears the green banner everywhere).
        await pool.execute(
            "UPDATE active_devices SET call_requested = NULL WHERE device_id=$1", device_id)
        return {"message": "offer stored"}


@app.post("/Call_Answer")
async def call_answer(request: Request):
    if (e := await _call_auth(request)) is not None: return e
    body = {}
    try: body = await request.json()
    except Exception: pass
    body = body or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    if not device_id:
        return err({"error": "device_id required"}, 400)
    with _call_lock:
        box = _mailbox(device_id)
        if "sdp" in body:
            box["answer"] = {"sdp": body["sdp"], "type": body.get("type", "answer")}
            return {"message": "answer stored"}
        return {"answer": box["answer"]}


@app.post("/Call_Ice")
async def call_ice(request: Request):
    if (e := await _call_auth(request)) is not None: return e
    body = {}
    try: body = await request.json()
    except Exception: pass
    body = body or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    side = body.get("side")
    if not device_id or side not in ("app", "device"):
        return err({"error": "device_id and side=app|device required"}, 400)
    with _call_lock:
        box = _mailbox(device_id)
        if body.get("candidate") is not None:
            box[f"{side}_ice"].append(body["candidate"])
            return {"message": "candidate stored"}
        other = "device_ice" if side == "app" else "app_ice"
        pending = box[other][:]; box[other] = []
        return {"candidates": pending}


@app.post("/Call_End")
async def call_end(request: Request):
    if (e := await _call_auth(request)) is not None: return e
    body = {}
    try: body = await request.json()
    except Exception: pass
    body = body or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    username = request.headers.get("Username")
    if not device_id:
        return err({"error": "device_id required"}, 400)
    with _call_lock:
        box = _call_mailbox.get(device_id)
        if not box:
            return {"message": "call cleared"}
        if username and box["in_call_by"] and box["in_call_by"] != username:
            return err({"error": "not_call_owner", "in_call_by": box["in_call_by"]}, 403)
        del _call_mailbox[device_id]
    mqtt_pub.state_call_pending(device_id, False, None)
    return {"message": "call cleared"}


# Calls now go through the Janus VideoRoom SFU (see /Call_Start + /Call_Stop and
# the device/app Janus clients). The old aiortc forwarder (sfu.py + /Call_Join /
# Call_Leave) was removed — it transcoded every frame and froze video.


@app.post("/Call_Start")
async def call_start(request: Request):
    """Caregiver app starts a Janus call → mark it active (so the device joins via
    call_pending) and return the deterministic Janus room for this device."""
    if (e := await _call_auth(request)) is not None: return e
    body = {}
    try: body = await request.json()
    except Exception: pass
    body = body or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    if not device_id:
        return err({"error": "device_id required"}, 400)
    await _wake_redis.hset(_JANUS_CALLS, device_id, request.headers.get("Username") or "caregiver")
    # A caregiver answering (Janus path) resolves the device's call REQUEST for all
    # caregivers — clears the green banner everywhere (the old /Call_Offer path did
    # this; the Janus path must too, or the banner sticks after the call).
    await pool.execute(
        "UPDATE active_devices SET call_requested = NULL WHERE device_id=$1", device_id)
    return {"room": room_for(device_id)}


@app.post("/Call_Stop")
async def call_stop(request: Request):
    if (e := await _call_auth(request)) is not None: return e
    body = {}
    try: body = await request.json()
    except Exception: pass
    body = body or {}
    device_id = body.get("device_id") or request.headers.get("Device-ID")
    if not device_id:
        return err({"error": "device_id required"}, 400)
    await _wake_redis.hdel(_JANUS_CALLS, device_id)
    return {"message": "call stopped"}


# ════════════════════════════════════════════════════════════════════════════
# WAKE COMMAND (heavy — STT + LLM, GPU-gated threadpool)
# ════════════════════════════════════════════════════════════════════════════
@app.post("/Wake_Command")
async def wake_command_route(request: Request):
    if wake_command is None:
        return err({"error": "wake_command module not loaded"}, 503)
    dev, e = await verify_device_auth(request)
    if e: return e
    device_id = dev["device_id"]
    wav_bytes = await request.body()
    if len(wav_bytes) < 64:
        return err({"error": "Audio body too small / missing"}, 400)
    if len(wav_bytes) > _MAX_UPLOAD_BYTES:
        return err({"error": "audio too large"}, 413)
    ts = datetime.now().strftime("%Y%m%d%H%M%S")
    occurred = _now_str()
    wav_path = os.path.join(UPLOAD_FOLDER, f"{device_id}_{ts}_wake.wav")
    await run_in_threadpool(_write_file, wav_path, wav_bytes)
    # Enqueue to the GPU worker (wake_worker.py) and wait for the result. The API
    # holds no models now → it can run multiple workers + bursts queue instead of
    # serializing in-process. Scale = more workers / batched backend on the worker.
    try:
        intent, transcript = await _classify_via_queue(wav_path)
    except Exception as ex:
        logging.error("wake classify (queue) failed for %s: %s", wav_path, ex)
        return err({"error": "classify failed"}, 500)

    text_payload = json.dumps(
        {"Message Time": occurred, "Message Type": intent, "transcript": transcript},
        ensure_ascii=False)

    if intent == "PILL":
        # Atomically mark the currently-active pill(s) consumed here, so the
        # app's done/total updates immediately on PILL classification — no
        # dependence on a separate device ack round-trip (which could miss the
        # active window or fail). Mirrors /Pill_Ack_Device's no-id path.
        _active_ids = [a["id"] for a in await active_pill_alarms_for(device_id)]
        if _active_ids:
            await pool.execute(
                "UPDATE pill_schedules SET last_ack_at=$1, "
                "consumptions_done=consumptions_done+1 "
                "WHERE device_id=$2 AND id = ANY($3::bigint[]) "
                "AND consumptions_done < total_count",
                _now_str(), device_id, _active_ids)
        msg_id = await pool.fetchval(
            "INSERT INTO messages (device_id, message_type, message_text, message_image, "
            "message_video, message_occur_time) VALUES ($1,'PillAck',$2,'','',$3) RETURNING id",
            device_id, text_payload, occurred)
        await fanout_message(device_id, msg_id, "💊 قرص مصرف شد", "بیمار قرص خود را خورد", "#33FF00")
        try: os.remove(wav_path)
        except OSError: pass
    elif intent == "CALL":
        # Include an "Event Title" so the app's event card shows "درخواست تماس"
        # instead of the unknown-event fallback when it surfaces this in-app.
        call_payload = json.dumps(
            {"Message Time": occurred, "Message Type": intent,
             "Event Title": "درخواست تماس", "transcript": transcript},
            ensure_ascii=False)
        msg_id = await pool.fetchval(
            "INSERT INTO messages (device_id, message_type, message_text, message_image, "
            "message_video, message_occur_time) VALUES ($1,'CallRequest',$2,'',$3,$4) RETURNING id",
            device_id, call_payload, wav_path, occurred)
        await fanout_message(device_id, msg_id, "📞 درخواست تماس", "بیمار درخواست تماس کرده است", "#FF8800")
        # Cross-caregiver call flag: every caregiver's card shows the green call
        # banner until SOMEONE calls the device back (cleared in /Call_Offer).
        await pool.execute(
            "UPDATE active_devices SET call_requested = now() WHERE device_id=$1", device_id)
    elif intent in ("SLEEP", "WAKE"):
        # Voice toggle of the EXISTING guest/sleep mode — same mechanism as the
        # app moon button and the device 3-tap(sleep)/1-tap(wake). SLEEP→guest on,
        # WAKE→guest off; delivered one-shot via guest_set on the next heartbeat,
        # so voice / moon / button all stay in sync.
        want = (intent == "SLEEP")
        await pool.execute(
            "UPDATE active_devices SET guest_requested=$1 WHERE device_id=$2", want, device_id)
        title = "😴 حالت خواب" if want else "🔔 حالت بیدار"
        body  = "دستگاه به حالت خواب رفت (دوربین و تشخیص سقوط خاموش)" if want \
                else "دستگاه دوباره فعال شد"
        evt = json.dumps({"Message Time": occurred, "Message Type": intent,
                          "Event Title": title, "transcript": transcript}, ensure_ascii=False)
        msg_id = await pool.fetchval(
            "INSERT INTO messages (device_id, message_type, message_text, message_image, "
            "message_video, message_occur_time) VALUES ($1,'Event',$2,'','',$3) RETURNING id",
            device_id, evt, occurred)
        await fanout_message(device_id, msg_id, title, body, "#9C27B0")
        try: os.remove(wav_path)
        except OSError: pass
    else:
        msg_id = await pool.fetchval(
            "INSERT INTO messages (device_id, message_type, message_text, message_image, "
            "message_video, message_occur_time) VALUES ($1,'Voice',$2,'',$3,$4) RETURNING id",
            device_id, text_payload, wav_path, occurred)
        await fanout_message(device_id, msg_id, "🎙️ پیام صوتی جدید", "بیمار پیام صوتی فرستاد", "#33AAFF")

    mqtt_pub.event_wake_command(device_id, intent, transcript, msg_id)
    if intent == "PILL":
        mqtt_pub.state_pill(device_id, await active_pill_alarms_for(device_id))
    return {"intent": intent, "transcript": transcript, "message_id": msg_id}


# ════════════════════════════════════════════════════════════════════════════
# PHASE 2 SUBSCRIPTION ENDPOINTS
# ════════════════════════════════════════════════════════════════════════════
@app.post("/Register_Device")
async def register_device(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    key_hash = body.get("key_hash")
    if not device_id or not key_hash:
        return err({"error": "device_id and key_hash required"}, 400)
    if len(key_hash) != 64:
        return err({"error": "key_hash must be 64-hex SHA-256"}, 400)
    if not _safe_device_id(device_id):
        return err({"error": "device_id must be 1-64 chars [A-Za-z0-9_-]"}, 400)
    try:
        async with pool.acquire() as conn:
            async with conn.transaction():
                await conn.execute("INSERT INTO devices (device_id, key_hash) VALUES ($1,$2)",
                                   device_id, key_hash)
                await conn.execute("INSERT INTO subscriptions (device_id, username) VALUES ($1,$2)",
                                   device_id, username)
                await conn.execute(
                    "INSERT INTO subscription_history (device_id, username, action, actor) "
                    "VALUES ($1,$2,'joined',NULL)", device_id, username)
    except asyncpg.UniqueViolationError:
        return err({"error": "device_id_taken"}, 409)
    except Exception as ex:
        return err({"error": str(ex)}, 500)
    return JSONResponse({"registered": device_id, "owner": username}, status_code=201)


@app.post("/Subscribe_Device")
async def subscribe_device(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    key_hash = body.get("key_hash")
    if not device_id or not key_hash:
        return err({"error": "device_id and key_hash required"}, 400)
    dev = await pool.fetchrow("SELECT key_hash FROM devices WHERE device_id=$1", device_id)
    if dev is None:
        return err({"error": "device_not_found"}, 404)
    if not _kh_match(key_hash, dev["key_hash"]):
        return err({"error": "wrong_password"}, 403)
    already = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE device_id=$1 AND username=$2", device_id, username)
    if already:
        return {"message": "already_subscribed"}
    try:
        async with pool.acquire() as conn:
            async with conn.transaction():
                await conn.execute("INSERT INTO subscriptions (device_id, username) VALUES ($1,$2)",
                                   device_id, username)
                await conn.execute(
                    "INSERT INTO subscription_history (device_id, username, action, actor) "
                    "VALUES ($1,$2,'joined',NULL)", device_id, username)
        return JSONResponse({"subscribed": device_id, "username": username}, status_code=201)
    except asyncpg.PostgresError as ex:
        if "device_full" not in str(ex):
            return err({"error": str(ex)}, 500)
        subs = await pool.fetch(
            "SELECT username, joined_at FROM subscriptions WHERE device_id=$1 ORDER BY joined_at",
            device_id)
        return err({"error": "device_full",
                    "subscribers": [{"username": s["username"], "joined_at": str(s["joined_at"])} for s in subs]}, 409)


@app.post("/Replace_Subscriber")
async def replace_subscriber(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    key_hash = body.get("key_hash")
    replace_target = body.get("replace_target")
    if not device_id or not key_hash or not replace_target:
        return err({"error": "device_id, key_hash, replace_target required"}, 400)
    if replace_target == username:
        return err({"error": "cannot_replace_self"}, 400)
    dev = await pool.fetchrow("SELECT key_hash FROM devices WHERE device_id=$1", device_id)
    if dev is None:
        return err({"error": "device_not_found"}, 404)
    if not _kh_match(key_hash, dev["key_hash"]):
        return err({"error": "wrong_password"}, 403)
    target = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE device_id=$1 AND username=$2", device_id, replace_target)
    if target is None:
        return err({"error": "target_not_subscribed"}, 404)
    already = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE device_id=$1 AND username=$2", device_id, username)
    if already:
        return err({"error": "already_subscribed"}, 409)
    async with pool.acquire() as conn:
        async with conn.transaction():
            await conn.execute("DELETE FROM subscriptions WHERE device_id=$1 AND username=$2",
                               device_id, replace_target)
            await conn.execute("INSERT INTO subscriptions (device_id, username) VALUES ($1,$2)",
                               device_id, username)
            await conn.execute(
                "INSERT INTO subscription_history (device_id, username, action, actor) "
                "VALUES ($1,$2,'kicked',$3)", device_id, replace_target, username)
            await conn.execute(
                "INSERT INTO subscription_history (device_id, username, action, actor) "
                "VALUES ($1,$2,'joined',$3)", device_id, username, username)
    return {"replaced": replace_target, "added": username}


@app.post("/Unsubscribe_Device")
async def unsubscribe_device(request: Request):
    username, e = await check_user_creds(request)
    if e: return e
    body = await json_body(request)
    device_id = body.get("device_id")
    key_hash = body.get("key_hash")
    if not device_id or not key_hash:
        return err({"error": "device_id and key_hash required"}, 400)
    dev = await pool.fetchrow("SELECT key_hash FROM devices WHERE device_id=$1", device_id)
    if dev is None:
        return err({"error": "device_not_found"}, 404)
    if not _kh_match(key_hash, dev["key_hash"]):
        return err({"error": "wrong_password"}, 403)
    async with pool.acquire() as conn:
        async with conn.transaction():
            res = await conn.execute(
                "DELETE FROM subscriptions WHERE device_id=$1 AND username=$2", device_id, username)
            # "DELETE N" — parse N (endswith("0") also matches 10, 20, …).
            deleted = int(res.split()[-1]) if res and res.split()[-1].isdigit() else 0
            if deleted == 0:
                return err({"error": "not_subscribed"}, 404)
            await conn.execute(
                "INSERT INTO subscription_history (device_id, username, action, actor) "
                "VALUES ($1,$2,'left',NULL)", device_id, username)
    return {"unsubscribed": device_id, "username": username}


async def _purge_device(device_id: str) -> dict:
    """Erase EVERYTHING tied to device_id — subscriptions, history, messages
    (+ per-user unread rows), pill schedules, both device tables, and on-disk
    media/audio. Shared by factory reset AND MAC-based re-provision dedup.
    Idempotent: a second call just finds nothing."""
    # Capture subscribers BEFORE deletion so we can push a live "device_removed"
    # notice to each of their apps (so the device vanishes from their list
    # immediately, without waiting for a re-login).
    sub_users = [r["username"] for r in await pool.fetch(
        "SELECT username FROM subscriptions WHERE device_id=$1", device_id)]
    file_paths = set()
    for r in await pool.fetch(
            "SELECT message_image, message_video FROM messages WHERE device_id=$1", device_id):
        for p in (r["message_image"], r["message_video"]):
            if p: file_paths.add(p)
    for r in await pool.fetch(
            "SELECT audio_path FROM pill_schedules WHERE device_id=$1", device_id):
        if r["audio_path"]: file_paths.add(r["audio_path"])

    counts = {}
    async with pool.acquire() as conn:
        async with conn.transaction():
            # FK-safe order: children (referencing the device / its messages) first.
            counts["unread_messages"] = _rowcount(await conn.execute(
                "DELETE FROM unread_messages WHERE message_id IN "
                "(SELECT id FROM messages WHERE device_id=$1)", device_id))
            counts["messages"] = _rowcount(await conn.execute(
                "DELETE FROM messages WHERE device_id=$1", device_id))
            counts["pill_schedules"] = _rowcount(await conn.execute(
                "DELETE FROM pill_schedules WHERE device_id=$1", device_id))
            counts["subscription_history"] = _rowcount(await conn.execute(
                "DELETE FROM subscription_history WHERE device_id=$1", device_id))
            counts["subscriptions"] = _rowcount(await conn.execute(
                "DELETE FROM subscriptions WHERE device_id=$1", device_id))
            counts["active_devices"] = _rowcount(await conn.execute(
                "DELETE FROM active_devices WHERE device_id=$1", device_id))
            counts["devices"] = _rowcount(await conn.execute(
                "DELETE FROM devices WHERE device_id=$1", device_id))

    # Glob + delete on a thread — a device with many fall MP4s can have hundreds
    # of files; doing this on the event loop would stall live requests/heartbeats
    # (and this can fire from the MAC-dedup path on every heartbeat until done).
    def _glob_and_remove(known, patterns):
        paths = set(known)
        for pat in patterns:
            paths.update(glob.glob(pat))
        n = 0
        for p in paths:
            try:
                os.remove(p); n += 1
            except OSError:
                pass
        return n
    files_removed = await run_in_threadpool(
        _glob_and_remove, file_paths,
        [os.path.join(UPLOAD_FOLDER, f"{device_id}_*"),
         os.path.join(PILL_AUDIO_DIR, f"{device_id}_*")])

    try: mqtt_pub.state_pill(device_id, [])
    except Exception: pass
    # Live removal signal → each subscriber's app deletes it locally on the spot.
    for u in sub_users:
        try: mqtt_pub.notify_user(u, {"type": "device_removed", "device_id": device_id})
        except Exception: pass

    print(f"[purge] {device_id}: {counts}, files={files_removed}, notified={len(sub_users)}")
    return {"deleted": counts, "files_removed": files_removed}


@app.post("/Factory_Reset_Device")
async def factory_reset_device(request: Request):
    """Device-initiated purge: when a device is factory-reset it calls this
    (authenticated with its CURRENT creds, before it wipes them) and the server
    erases everything tied to that device_id (see _purge_device).

    Device-authenticated (not user) — the physical reset is the authority. After
    this, any caregiver still subscribed sees the device vanish from their list."""
    dev, e = await verify_device_auth(request)
    if e: return e
    res = await _purge_device(dev["device_id"])
    return {"reset": dev["device_id"], **res}


@app.post("/Fall_Clip_Replace_Device")
async def fall_clip_replace_device(request: Request):
    """After a fall, the device first POSTs an INSTANT event (thumbnail +
    pre-roll video) so the notification fires immediately. Once the pain session
    finishes it assembles the full 10 s A/V clip (5 s pre-fall + 5 s of the
    spoken response, muxed with audio) and calls this to REPLACE that event's
    video. The clip is E2E-encrypted device-side exactly like the original
    (server only ever stores ciphertext). The lazy /Fall_Video_App download then
    serves the richer clip; the app decrypts + plays it (audio included).

    Device-authenticated. Replaces the newest Event for this device (falls are
    minutes apart and a pain session is ~11 s, so there's no ambiguity)."""
    dev, e = await verify_device_auth(request)
    if e: return e
    device_id = dev["device_id"]
    form = await request.form()
    clip = form.get("video file")
    if clip is None or not hasattr(clip, "read"):
        return err({"error": "missing 'video file'"}, 400)
    row = await pool.fetchrow(
        "SELECT id, message_video FROM messages "
        "WHERE device_id=$1 AND message_type='Event' ORDER BY id DESC LIMIT 1",
        device_id)
    if row is None:
        return err({"error": "no Event to replace"}, 404)
    clip_bytes = await clip.read()
    if len(clip_bytes) > _MAX_UPLOAD_BYTES:
        return err({"error": "clip too large"}, 413)
    ts = datetime.now().strftime("%Y%m%d%H%M%S")
    new_path = os.path.join(UPLOAD_FOLDER, f"{device_id}_{ts}_avclip.bin")
    await run_in_threadpool(_write_file, new_path, clip_bytes)
    old = row["message_video"]
    await pool.execute("UPDATE messages SET message_video=$1 WHERE id=$2",
                       new_path, row["id"])
    if old and old != new_path:
        await run_in_threadpool(_remove_files, [old])
    sz = len(clip_bytes)
    print(f"[fall-clip] replaced Event {row['id']} video for {device_id} ({sz} B)")
    return {"replaced": row["id"], "bytes": sz}


@app.get("/Device_Subscribers")
async def device_subscribers(request: Request, device_id: str = Query(...)):
    username, e = await check_user_creds(request)
    if e: return e
    is_member = await pool.fetchrow(
        "SELECT 1 FROM subscriptions WHERE device_id=$1 AND username=$2", device_id, username)
    if not is_member:
        return err({"error": "not_subscribed"}, 403)
    subs = await pool.fetch(
        "SELECT username, joined_at FROM subscriptions WHERE device_id=$1 ORDER BY joined_at",
        device_id)
    return {"device_id": device_id,
            "subscribers": [{"username": s["username"], "joined_at": str(s["joined_at"])} for s in subs]}
