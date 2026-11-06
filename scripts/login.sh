#!/usr/bin/env bash
set -euo pipefail

TUNNEL="" PASSWORD="${SSHPASS:-}" LOCAL_PORT=2223

while [[ $# -gt 0 ]]; do
  case $1 in
    --tunnel)   TUNNEL="$2"; shift ;;
    --password) PASSWORD="$2"; shift ;;
    --port)     LOCAL_PORT="$2"; shift ;;
    *) echo "Unknown parameter: $1" >&2; exit 1 ;;
  esac
  shift
done

[[ -n "$TUNNEL" && -n "$PASSWORD" ]] || {
  echo "Usage: $0 --tunnel <name or full hostname> --password <password>" >&2; exit 1; }

# Accept "name", "name.trycloudflare.com" or "https://name.trycloudflare.com"
TUNNEL="${TUNNEL#https://}"
TUNNEL="${TUNNEL%.trycloudflare.com}"
HOST="${TUNNEL}.trycloudflare.com"

pkill -f "cloudflared access tcp" 2>/dev/null || true

LOG="$(mktemp)"
cloudflared access tcp --hostname "$HOST" --url "localhost:${LOCAL_PORT}" >"$LOG" 2>&1 &
CF_PID=$!
trap 'kill "$CF_PID" 2>/dev/null || true; rm -f "$LOG"' EXIT

# Wait for the local listener instead of a fixed sleep
for _ in $(seq 1 20); do
  (exec 3<>"/dev/tcp/127.0.0.1/${LOCAL_PORT}") 2>/dev/null && break
  kill -0 "$CF_PID" 2>/dev/null || { echo "cloudflared exited:" >&2; cat "$LOG" >&2; exit 1; }
  sleep 0.5
done

# Cheap pre-check: does the tunnel actually reach the origin?
if ! ssh-keyscan -T 5 -p "$LOCAL_PORT" 127.0.0.1 >/dev/null 2>&1; then
  echo "Tunnel $HOST is not reachable. It is probably stale or down." >&2
  echo "On the Mac run: ./scripts/zdev.sh tunnel" >&2
  echo "--- cloudflared log ---" >&2; tail -5 "$LOG" >&2
  exit 1
fi

export SSHPASS="$PASSWORD"
sshpass -e ssh -o PubkeyAuthentication=no -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null -p "$LOCAL_PORT" frienduser@localhost
