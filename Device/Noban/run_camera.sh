#!/bin/bash
# CSI camera → /dev/video9 loopback feeder.
#
# Boot-robustness: starting rpicam-vid before the CSI camera (imx708) is actually
# enumerated makes rpicam stall, which wedges the ffmpeg consumer at 0% CPU — the
# loopback then never gets a valid format (stuck on a stale BGR4/Size=0), so with
# exclusive_caps=1 it never becomes a capture device and the pipeline grabber can
# never open it. So we (1) wait for the loopback node, (2) wait for the real
# camera to be listed, and (3) run a self-heal monitor that exits the script if
# the loopback isn't streaming shortly after start, letting systemd restart us.

# 1. Wait for the v4l2loopback node.
for i in $(seq 1 30); do [ -e /dev/video9 ] && break; sleep 1; done
sudo chmod 666 /dev/video9 2>/dev/null || true

# 2. Wait for the actual camera to be enumerated before streaming.
for i in $(seq 1 30); do
  rpicam-hello --list-cameras 2>/dev/null | grep -qE '^[0-9]+ *:' && break
  sleep 1
done

# 3. Self-heal: if /dev/video9 has no valid streaming format within ~60s of
#    start (wedged ffmpeg), kill the feed so the script exits and systemd
#    (Restart=always) relaunches us with a fresh, by-then-ready camera.
(
  for w in $(seq 1 6); do
    sleep 10
    sz=$(v4l2-ctl -d /dev/video9 --get-fmt-video 2>/dev/null \
          | awk -F: '/Size Image/{gsub(/[^0-9]/,"",$2); print $2}')
    [ "${sz:-0}" -gt 0 ] && exit 0          # streaming OK → stop monitoring
  done
  echo "[run_camera] /dev/video9 not streaming after 60s — restarting feed" >&2
  pkill -P $$ rpicam-vid 2>/dev/null
  pkill -P $$ ffmpeg 2>/dev/null
) &

# 4. Stream. NOT exec'd, so if the pipeline ever exits the script returns and
#    systemd restarts it.
rpicam-vid -t 0 --nopreview --width 640 --height 480 --framerate 16 \
     --codec yuv420 -o - 2>/dev/null \
   | ffmpeg -loglevel error -f rawvideo -pix_fmt yuv420p -video_size 640x480 \
       -framerate 16 -i - -f v4l2 -pix_fmt yuyv422 /dev/video9
