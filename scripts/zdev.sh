#!/usr/bin/env bash
# zdev: manage the zahlen dev container and its Cloudflare quick tunnel
set -euo pipefail

NAME=zahlen-dev-container
IMAGE=zahlen-dev
HOST_PORT=2222
SRC_VOL=zahlen-src
CCACHE_VOL=zahlen-ccache

HERE="$(cd "$(dirname "$0")" && pwd)"
DOCKERFILE="${ZDEV_DOCKERFILE:-$HERE/Dockerfile.dev}"
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/zdev"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/zdev"
PW_FILE="$CONF/password"
TUN_LOG="$STATE/tunnel.log"
TUN_PID="$STATE/tunnel.pid"
mkdir -p "$STATE" "$CONF"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

get_password() {
  if [[ -n "${ZDEV_PASSWORD:-}" ]]; then printf '%s' "$ZDEV_PASSWORD"; return; fi
  if [[ ! -s "$PW_FILE" ]]; then
    umask 077
    openssl rand -base64 18 | tr -d '/+=\n' > "$PW_FILE"
    log "generated a new password, saved to $PW_FILE" >&2
  fi
  cat "$PW_FILE"
}

ensure_machine() {
  if ! podman machine list --format '{{.Running}}' 2>/dev/null | grep -q true; then
    log "starting podman machine"
    podman machine start
  fi
}

build() {
  [[ -f "$DOCKERFILE" ]] || die "Dockerfile not found: $DOCKERFILE"
  ensure_machine
  local ctx; ctx="$(mktemp -d)"
  trap 'rm -rf "$ctx"' RETURN
  log "building $IMAGE"
  local args=(--build-arg "FRIEND_PASSWORD=$(get_password)")
  [[ -n "${ZDEV_SSH_PUBKEY:-}" ]] && args+=(--build-arg "SSH_PUBKEY=$ZDEV_SSH_PUBKEY")
  podman build -f "$DOCKERFILE" -t "$IMAGE" "${args[@]}" "$ctx"
}

run() {
  ensure_machine
  podman image exists "$IMAGE" || die "image $IMAGE missing, run: $0 rebuild"
  podman volume exists "$SRC_VOL"    || podman volume create "$SRC_VOL" >/dev/null
  podman volume exists "$CCACHE_VOL" || podman volume create "$CCACHE_VOL" >/dev/null
  podman rm -f "$NAME" >/dev/null 2>&1 || true
  log "starting container $NAME"
  podman run -d --name "$NAME" \
    -p "127.0.0.1:${HOST_PORT}:22" \
    -v "$SRC_VOL:/home/frienduser/Projects" \
    -v "$CCACHE_VOL:/home/frienduser/.ccache" \
    "$IMAGE" >/dev/null
}

tunnel_stop() {
  if [[ -f "$TUN_PID" ]] && kill -0 "$(cat "$TUN_PID")" 2>/dev/null; then
    kill "$(cat "$TUN_PID")" 2>/dev/null || true
  fi
  rm -f "$TUN_PID"
}

tunnel_start() {
  command -v cloudflared >/dev/null || die "cloudflared not installed (brew install cloudflared)"
  tunnel_stop
  : > "$TUN_LOG"
  log "starting tunnel"
  nohup cloudflared tunnel --url "tcp://localhost:${HOST_PORT}" >"$TUN_LOG" 2>&1 &
  echo $! > "$TUN_PID"
  local url=""
  for _ in $(seq 1 30); do
    url="$(grep -Eo 'https://[a-z0-9-]+\.trycloudflare\.com' "$TUN_LOG" | head -n1 || true)"
    [[ -n "$url" ]] && break
    sleep 1
  done
  [[ -n "$url" ]] || die "no tunnel URL after 30s, see $TUN_LOG"
  print_connect "${url#https://}"
}

print_connect() {
  local host="$1"
  cat <<EOF

Tunnel:   $host
Password: $(get_password)

On the client machine:
  cloudflared access tcp --hostname $host --url localhost:2223
  ssh -p 2223 frienduser@localhost

EOF
}

status() {
  podman ps -a --filter "name=^${NAME}\$" --format 'container: {{.Names}} {{.Status}}' || true
  if [[ -f "$TUN_PID" ]] && kill -0 "$(cat "$TUN_PID")" 2>/dev/null; then
    local url; url="$(grep -Eo '[a-z0-9-]+\.trycloudflare\.com' "$TUN_LOG" | head -n1 || true)"
    echo "tunnel:    running ($url)"
  else
    echo "tunnel:    stopped"
  fi
}

usage() {
  cat <<EOF
usage: $(basename "$0") <command>

  up         start machine, container and tunnel (builds the image if missing)
  rebuild    rebuild the image and recreate the container (volumes are kept)
  restart    recreate the container from the current image, keep the tunnel
  tunnel     (re)start only the tunnel and print the new hostname
  down       stop the tunnel and the container
  status     show container and tunnel state
  shell      open a shell in the container
  logs       follow dropbear logs
  password   print the current password
  backup     export the source volume to ./zahlen-src-<date>.tar
  nuke       remove container, image and volumes (asks first)
EOF
}

case "${1:-}" in
  up)
    ensure_machine
    podman image exists "$IMAGE" || build
    run
    tunnel_start ;;
  rebuild) build; run; log "container recreated; run '$0 tunnel' if the tunnel is down" ;;
  restart) run ;;
  tunnel)  ensure_machine; tunnel_start ;;
  down)    tunnel_stop; podman rm -f "$NAME" >/dev/null 2>&1 || true; log "stopped" ;;
  status)  status ;;
  shell)   podman exec -it -u frienduser "$NAME" bash ;;
  logs)    podman logs -f "$NAME" ;;
  password) get_password; echo ;;
  backup)
    out="zahlen-src-$(date +%Y%m%d-%H%M).tar"
    podman volume export "$SRC_VOL" > "$out" && log "wrote $out" ;;
  nuke)
    read -r -p "Delete container, image AND volumes (source + ccache)? type 'yes': " a
    [[ "$a" == yes ]] || die "aborted"
    tunnel_stop
    podman rm -f "$NAME" >/dev/null 2>&1 || true
    podman rmi -f "$IMAGE" >/dev/null 2>&1 || true
    podman volume rm -f "$SRC_VOL" "$CCACHE_VOL" >/dev/null 2>&1 || true ;;
  *) usage ;;
esac
