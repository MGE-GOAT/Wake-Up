#!/usr/bin/env bash
# Inject a fake fall event into the server, exactly as the Pi would. Lets
# you exercise the app's event-card UI (red banner, image, video, read/
# unread badge) without needing the C++ pipeline or a real Pi.
#
# Usage:
#   ./seed_event.sh                              # default: localhost, fall event
#   SERVER=http://10.0.0.42:5000 ./seed_event.sh # remote server
#   TYPE=image-only ./seed_event.sh              # snapshot only, no MP4
#
# Requires: curl, an image file you point at (default: any JPG on the system)

set -uo pipefail

SERVER="${SERVER:-http://127.0.0.1:5000}"
DEVICE_ID="${DEVICE_ID:-ID_12233945}"
AUTH_CODE="${AUTH_CODE:-1223344556677889}"
TYPE="${TYPE:-event}"   # event | image-only

# Pick any JPEG to send. Override IMAGE=/path/to/test.jpg if you want.
IMAGE="${IMAGE:-}"
if [ -z "${IMAGE}" ]; then
    # First JPG in common test locations, else generate a tiny 1x1 black one
    for cand in /tmp/*.jpg /tmp/*.jpeg ~/Pictures/*.jpg /usr/share/pixmaps/*.png; do
        [ -f "${cand}" ] && IMAGE="${cand}" && break
    done
    if [ -z "${IMAGE}" ]; then
        IMAGE=/tmp/seed_event_test.jpg
        # 1x1 black JPEG, hardcoded base64 — works without ImageMagick
        echo -n "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/2wBDAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAr/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/8QAFAEBAAAAAAAAAAAAAAAAAAAAAP/EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAMAwEAAhEDEQA/AL+AAAAAAAAAAAA//Z" \
            | base64 -d > "${IMAGE}"
    fi
fi

TS=$(date '+%Y-%m-%d %H:%M:%S')

if [ "${TYPE}" = "image-only" ]; then
    JSON='{"Message Type":"image captured","image_req_id":99,"Message Time":"'${TS}'"}'
    echo "[seed] POST ${SERVER}/Message_Device  (image only)"
    curl -sS -w '\n[seed] HTTP %{http_code}\n' \
        -H "Device-Id: ${DEVICE_ID}" \
        -H "Authentication-Code: ${AUTH_CODE}" \
        -F "text=${JSON};type=application/json" \
        -F "image file=@${IMAGE};type=image/jpeg" \
        "${SERVER}/Message_Device"
    exit $?
fi

# Default: fall event. Severity 2 = red banner in the app.
JSON='{"Message Type":"Event","Event Title":"Fall Detection","Message Title":"Fall Detection","Danger Level":2,"Message Time":"'${TS}'"}'

VIDEO_ARG=()
if [ -n "${VIDEO:-}" ] && [ -f "${VIDEO}" ]; then
    VIDEO_ARG=(-F "video file=@${VIDEO};type=video/mp4")
    echo "[seed] including video clip from ${VIDEO}"
else
    echo "[seed] no VIDEO=... provided; sending image-only event (set VIDEO=/path/to/clip.mp4 to add)"
fi

echo "[seed] POST ${SERVER}/Message_Device  (Event, Danger Level 2)"
curl -sS -w '\n[seed] HTTP %{http_code}\n' \
    -H "Device-Id: ${DEVICE_ID}" \
    -H "Authentication-Code: ${AUTH_CODE}" \
    -F "text=${JSON};type=application/json" \
    -F "image file=@${IMAGE};type=image/jpeg" \
    "${VIDEO_ARG[@]}" \
    "${SERVER}/Message_Device"
