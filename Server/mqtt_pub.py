"""MQTT publisher used by Phase 5.1.

One global client, started lazily on first publish. All calls are
fire-and-forget — if the broker is down, the publish call logs and returns;
the existing HTTP fallbacks keep the system working during the transition.

Topic layout (kept in one place so app + device clients can mirror it):

    device/<id>/state/pill           — retained, current active alarm(s)
    device/<id>/state/call_pending   — retained, bool
    device/<id>/state/in_call_by     — retained, "username" or empty
    device/<id>/state/image_req      — retained, request id or empty

    device/<id>/event/fall           — fan-out, fall confirmed payload
    device/<id>/event/voice          — fan-out, new voice msg ID
    device/<id>/event/wake_command   — fan-out, wake/intent

    device/<id>/online               — LWT (broker auto-publishes "0")
    device/<id>/cmd/pill_ack         — device → server (subscribed by server)
    device/<id>/cmd/call_end         — either side

    user/<username>/notify           — per-user catch-all (subscriber inbox)

QoS choices:
    state/*  → 1, retained=True     (latest value matters, must arrive once)
    event/*  → 1, retained=False    (must arrive but don't replay history)
    cmd/*    → 1, retained=False
    online   → 1, retained=True (LWT)
"""
from __future__ import annotations

import json
import logging
import os
import socket
import threading
from typing import Any, Optional

import paho.mqtt.client as mqtt

log = logging.getLogger("mqtt_pub")

BROKER_HOST = os.environ.get("MQTT_HOST", "127.0.0.1")
BROKER_PORT = int(os.environ.get("MQTT_PORT", "1883"))
CLIENT_ID   = f"elderly-server-{socket.gethostname()}"

_client: Optional[mqtt.Client] = None
_lock = threading.Lock()
_connected = False


def _on_connect(client, _userdata, _flags, reason_code, _properties=None):
    global _connected
    _connected = (reason_code == 0 or str(reason_code) == "Success")
    log.info("[mqtt] connected rc=%s", reason_code)


def _on_disconnect(_client, _userdata, _flags, reason_code, _properties=None):
    global _connected
    _connected = False
    log.warning("[mqtt] disconnected rc=%s", reason_code)


def _ensure_client() -> Optional[mqtt.Client]:
    global _client
    with _lock:
        if _client is not None:
            return _client
        try:
            c = mqtt.Client(
                callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                client_id=CLIENT_ID,
                clean_session=True,
            )
            c.on_connect    = _on_connect
            c.on_disconnect = _on_disconnect
            c.reconnect_delay_set(min_delay=1, max_delay=30)
            c.connect_async(BROKER_HOST, BROKER_PORT, keepalive=60)
            c.loop_start()      # background thread runs reconnect + I/O
            _client = c
            return c
        except Exception as e:
            log.warning("[mqtt] init failed: %s", e)
            return None


def publish(topic: str, payload: Any, qos: int = 1, retain: bool = False) -> bool:
    """Fire-and-forget. Returns False if broker is unreachable, but does not
    raise — caller can keep using HTTP fallback during the transition."""
    c = _ensure_client()
    if c is None:
        return False
    body = payload if isinstance(payload, (str, bytes)) else json.dumps(
        payload, ensure_ascii=False)
    try:
        info = c.publish(topic, body, qos=qos, retain=retain)
        # info.rc=0 means queued for delivery (NOT yet confirmed); paho handles
        # reconnect + delivery internally with QoS≥1.
        return info.rc == mqtt.MQTT_ERR_SUCCESS
    except Exception as e:
        log.warning("[mqtt] publish %s failed: %s", topic, e)
        return False


# ── Domain-specific convenience wrappers ────────────────────────────────────

def state_pill(device_id: str, active_alarms: list) -> bool:
    """Replaces the active_pill_alarms field in /Live_Device responses.
    Device subscribes to device/<id>/state/pill and re-renders/replays."""
    return publish(
        f"device/{device_id}/state/pill",
        {"active_pill_alarms": active_alarms},
        retain=True,
    )


def state_call_pending(device_id: str, pending: bool, in_call_by: Optional[str]) -> bool:
    """Pushes call-pending + lock state. Device wakes call_helper if pending=true;
    every subscribed user's app updates the "in call by X" badge."""
    return publish(
        f"device/{device_id}/state/call_pending",
        {"pending": pending, "in_call_by": in_call_by},
        retain=True,
    )


def state_image_req(device_id: str, req_id: Optional[int]) -> bool:
    return publish(
        f"device/{device_id}/state/image_req",
        {"req_id": req_id},
        retain=True,
    )


def event_fall(device_id: str, message_id: int, occur_time: str,
               image_url: str = "", video_url: str = "") -> bool:
    return publish(
        f"device/{device_id}/event/fall",
        {
            "message_id": message_id,
            "occur_time": occur_time,
            "image_url":  image_url,
            "video_url":  video_url,
        },
        retain=False,
    )


def event_voice(device_id: str, message_id: int, occur_time: str,
                audio_url: str = "") -> bool:
    return publish(
        f"device/{device_id}/event/voice",
        {
            "message_id": message_id,
            "occur_time": occur_time,
            "audio_url":  audio_url,
        },
        retain=False,
    )


def event_wake_command(device_id: str, intent: str, transcript: str,
                       message_id: Optional[int]) -> bool:
    return publish(
        f"device/{device_id}/event/wake_command",
        {"intent": intent, "transcript": transcript, "message_id": message_id},
        retain=False,
    )


def notify_user(username: str, payload: dict) -> bool:
    """Catch-all topic per user for misc notifications (new event on a
    subscribed device, kick notice, etc.). App subscribes to
    user/<self>/notify and routes by payload['type']."""
    return publish(f"user/{username}/notify", payload, retain=False)
