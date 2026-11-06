#!/usr/bin/env bash
set -euo pipefail

ENV_FILE=".env"

# Load existing .env if present so variables act as defaults
if [[ -f "$ENV_FILE" ]]; then
  set -a
  source "$ENV_FILE"
  set +a
fi

TUNNEL="${TUNNEL:-}"
PASSWORD="${PASSWORD:-${SSHPASS:-}}"
LOCAL_PORT="${LOCAL_PORT:-2223}"

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
  echo "Usage: $0 [--tunnel <name or full hostname>] [--password <password>] [--port <port>]" >&2; exit 1; }

# Helper function to update or append specific variables in .env without wiping others
update_env() {
  local key="$1"
  local val="$2"
  if [[ ! -f "$ENV_FILE" ]]; then
    echo "${key}=${val}" > "$ENV_FILE"
    return
  fi
  if grep -q "^${key}=" "$ENV_FILE"; then
    grep -v "^${key}=" "$ENV_FILE" > "${ENV_FILE}.tmp" || true
    echo "${key}=${val}" >> "${ENV_FILE}.tmp"
    mv "${ENV_FILE}.tmp" "$ENV_FILE"
  else
    echo "${key}=${val}" >> "$ENV_FILE"
  fi
}

# Automatically save/update the current values to .env
update_env "TUNNEL" "$TUNNEL"
update_env "PASSWORD" "$PASSWORD"
update_env "LOCAL_PORT" "$LOCAL_PORT"

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
