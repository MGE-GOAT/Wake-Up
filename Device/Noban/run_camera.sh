#!/bin/bash
# CSI camera → /dev/video9 loopback feeder.
for i in $(seq 1 30); do [ -e /dev/video9 ] && break; sleep 1; done
sudo chmod 666 /dev/video9 2>/dev/null || true
exec rpicam-vid -t 0 --nopreview --width 640 --height 480 --framerate 16 \
     --codec yuv420 -o - 2>/dev/null \
   | ffmpeg -loglevel error -f rawvideo -pix_fmt yuv420p -video_size 640x480 \
       -framerate 16 -i - -f v4l2 -pix_fmt yuyv422 /dev/video9
