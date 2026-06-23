#!/bin/bash
# Noban self-heal watchdog — runs at boot+120s and then every 60s.
#   A) Mic contention: the pipeline logs "failed to open mic" only when it wants
#      the mic (i.e. NOT in a call — during a call it releases it). So sustained
#      mic-open failures = the call-daemon is stuck holding the mic from a prior
#      call. Restart the daemon to free it (safe; no active call). Cheap recovery.
#   B) Never reached "Listening": audio/i2s wedged at startup — only a full reboot
#      clears it. Reboot ONCE (capped) so it can't loop forever.
set +e
CNT=/var/lib/elderly-care/wd_count
MAX=2
mkdir -p /var/lib/elderly-care

reached_listening() {
  systemctl is-active --quiet elderly-pipeline || return 1
  journalctl -u elderly-pipeline -b --no-pager 2>/dev/null | grep -q "Listening for wake word" || return 1
  return 0
}
mic_contention() {
  local n
  n=$(journalctl -u elderly-pipeline --since "60 seconds ago" --no-pager 2>/dev/null | grep -c "failed to open mic")
  [ "${n:-0}" -ge 5 ]
}

# A) stuck mic → free it by restarting the call-daemon (no reboot needed)
if mic_contention; then
  logger -t noban-watchdog "mic contention (stuck call-daemon) — restarting elderly-call-daemon"
  systemctl restart elderly-call-daemon
  exit 0
fi

# C) crash-loop AFTER coming up: systemd Restart=always silently respawns a
#    crashing pipeline every RestartSec forever (e.g. a wedged i2s/ALSA state
#    after a power event). reached_listening() can't see this — the boot journal
#    still holds an old "Listening" line — so detect a BURST of restarts within
#    one tick and reboot once (capped) to clear it, like (B).
RST=/var/lib/elderly-care/wd_restarts
now_r=$(systemctl show -p NRestarts --value elderly-pipeline 2>/dev/null || echo 0)
prev_r=$(cat "$RST" 2>/dev/null || echo "$now_r")
echo "$now_r" > "$RST"
# Only act when the counter advanced (it resets to 0 on a real reboot, so a
# negative delta = post-reboot, ignore). >=3 restarts in one 60s tick = looping.
if [ "${now_r:-0}" -ge "${prev_r:-0}" ] 2>/dev/null; then
  if [ "$(( now_r - prev_r ))" -ge 3 ]; then
    n=$(cat "$CNT" 2>/dev/null || echo 0); n=$((n + 1))
    if [ "$n" -gt "$MAX" ]; then
      logger -t noban-watchdog "crash-loop ($((now_r - prev_r)) restarts/tick) but reboot cap ($MAX) reached — manual attention needed"
      exit 0
    fi
    echo "$n" > "$CNT"
    logger -t noban-watchdog "crash-loop ($((now_r - prev_r)) restarts/tick) — reboot #$n/$MAX to clear it"
    sync; sleep 2; systemctl reboot
    exit 0
  fi
fi

# B) never came up → reboot once (capped)
if ! reached_listening; then
  n=$(cat "$CNT" 2>/dev/null || echo 0); n=$((n + 1))
  if [ "$n" -gt "$MAX" ]; then
    logger -t noban-watchdog "UNHEALTHY but reboot cap ($MAX) reached — manual attention needed"
    exit 0
  fi
  echo "$n" > "$CNT"
  logger -t noban-watchdog "not listening — reboot #$n/$MAX to clear i2s wedge"
  sync; sleep 2; systemctl reboot
  exit 0
fi

# healthy
echo 0 > "$CNT"
logger -t noban-watchdog "OK — pipeline listening"
