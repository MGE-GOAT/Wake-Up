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
