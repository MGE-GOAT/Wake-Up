#!/bin/bash
# Server health watchdog (#5) — run every 60s via the timer. Restarts a unit if
# unhealthy. For the systemd (non-Docker) deployment; Docker uses restart policies
# + healthchecks instead.
set +e
API_URL="${API_URL:-http://127.0.0.1:5000/Pill_Status_App}"

# API healthy = responds (403 to a bad-cred probe = up and routing).
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST "$API_URL" \
        -H 'Username: x' -H 'Password: x')
if [ "$code" != "403" ] && [ "$code" != "200" ]; then
  logger -t noban-health "API unhealthy (HTTP $code) — restarting noban-api"
  systemctl restart noban-api
fi

# Worker healthy = the wake queue isn't backing up (worker alive + draining).
depth=$(redis-cli llen wake:jobs 2>/dev/null || echo 0)
if [ "${depth:-0}" -gt 50 ]; then
  logger -t noban-health "wake queue backed up ($depth) — restarting noban-worker"
  systemctl restart noban-worker
fi

# Redis / Postgres reachable?
redis-cli ping >/dev/null 2>&1 || { logger -t noban-health "redis down — restarting"; systemctl restart redis-server; }
pg_isready -q 2>/dev/null || logger -t noban-health "postgres not ready"
