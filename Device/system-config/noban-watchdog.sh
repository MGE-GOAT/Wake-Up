#!/bin/bash
# Noban self-heal watchdog — runs at boot+120s and then every 60s.
#
# Reboots ONLY on a genuinely unhealthy pipeline, capped so it can never loop:
#   A) Mic contention — sustained "failed to open mic" = a stuck call-daemon
#      holding the mic from a prior call. Restart the daemon to free it (cheap,
#      no reboot).
#   B) Crash-loop — systemd Restart=always silently respawns a crashing pipeline
#      (e.g. a wedged i2s/ALSA state, which historically SIGILLs). A burst of
#      restarts within one tick → reboot once (capped) to clear it.
#   C) Service dead — the unit is not active for 2 consecutive checks (it failed
#      and stayed down) → reboot once (capped).
#
# IMPORTANT: health is judged by systemd state + restart counters only. We do
# NOT grep the journal for the one-time "Listening for wake word" line — the
# pipeline logs continuously, so on a volatile journal that line is evicted from
# the ring after ~10 min, which made the old check reboot a HEALTHY listening
# device on a ~10-min cadence. (See setup_new_device.sh: journald is also made
# persistent now.)
set +e
CNT=/var/lib/elderly-care/wd_count        # reboot cap counter
RST=/var/lib/elderly-care/wd_restarts     # last-seen NRestarts (crash-loop)
DEAD=/var/lib/elderly-care/wd_dead        # consecutive not-active checks
MAX=2
mkdir -p /var/lib/elderly-care

mic_contention() {
  local n
  n=$(journalctl -u elderly-pipeline --since "60 seconds ago" --no-pager 2>/dev/null | grep -c "failed to open mic")
  [ "${n:-0}" -ge 5 ]
}

reboot_capped() {  # $1 = reason
  local n
  n=$(cat "$CNT" 2>/dev/null || echo 0); n=$((n + 1))
  if [ "$n" -gt "$MAX" ]; then
    logger -t noban-watchdog "$1 but reboot cap ($MAX) reached — manual attention needed"
    exit 0
  fi
  echo "$n" > "$CNT"
  logger -t noban-watchdog "$1 — reboot #$n/$MAX"
  sync; sleep 2; systemctl reboot
  exit 0
}

# A) stuck mic → free it by restarting the call-daemon (no reboot)
if mic_contention; then
  logger -t noban-watchdog "mic contention (stuck call-daemon) — restarting elderly-call-daemon"
  systemctl restart elderly-call-daemon
  exit 0
fi

# B) crash-loop: NRestarts jumped by >=3 within one tick (counter resets to 0 on
#    a real reboot, so ignore a negative delta).
now_r=$(systemctl show -p NRestarts --value elderly-pipeline 2>/dev/null || echo 0)
prev_r=$(cat "$RST" 2>/dev/null || echo "$now_r")
echo "$now_r" > "$RST"
if [ "${now_r:-0}" -ge "${prev_r:-0}" ] 2>/dev/null; then
  if [ "$(( now_r - prev_r ))" -ge 3 ]; then
    reboot_capped "crash-loop ($(( now_r - prev_r )) restarts/tick)"
  fi
fi

# C) service dead for 2 consecutive checks → reboot once (capped). A single
#    not-active check is ignored (it's usually a normal 5s RestartSec gap).
if systemctl is-active --quiet elderly-pipeline; then
  echo 0 > "$DEAD"
else
  d=$(cat "$DEAD" 2>/dev/null || echo 0); d=$((d + 1)); echo "$d" > "$DEAD"
  if [ "$d" -ge 2 ]; then
    reboot_capped "pipeline dead (not active for 2 checks)"
  fi
  logger -t noban-watchdog "pipeline not active (check $d/2) — watching"
  exit 0
fi

# healthy → reset the reboot cap + dead counter
echo 0 > "$CNT"; echo 0 > "$DEAD"
logger -t noban-watchdog "OK — pipeline active"
