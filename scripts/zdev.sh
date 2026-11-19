#!/usr/bin/env bash
# zdev: manage the zahlen dev container and its Cloudflare quick tunnel (Cross-Platform: Arch & macOS)
set -euo pipefail

NAME=zahlen-dev-container
IMAGE=zahlen-dev
HOST_PORT=2222
SRC_VOL=zahlen-src
CCACHE_VOL=zahlen-ccache

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Detect Host OS
OS="$(uname -s)"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
    printf '\033[1;31merror:\033[0m %s\n' "$*" >&2
    exit 1
}

find_containerfile() {
    local c
    local candidates=(
        "${ZDEV_CONTAINERFILE:-${ZDEV_DOCKERFILE:-}}"
        "$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null)/docker/dev/Containerfile"
        "$PWD/docker/dev/Containerfile"
        "$SCRIPT_DIR/docker/dev/Containerfile"
        "$SCRIPT_DIR/../docker/dev/Containerfile"
    )
    for c in "${candidates[@]}"; do
        [[ -n "$c" && -f "$c" ]] && {
            (cd "$(dirname "$c")" && printf '%s/%s' "$PWD" "$(basename "$c")")
            return
        }
    done
    return 1
}

STATE="${XDG_STATE_HOME:-$HOME/.local/state}/zdev"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/zdev"
PW_FILE="$CONF/password"
TUN_LOG="$STATE/tunnel.log"
TUN_PID="$STATE/tunnel.pid"
mkdir -p "$STATE" "$CONF"

for bin in podman openssl; do
    command -v "$bin" >/dev/null || die "$bin not found in PATH"
done

get_password() {
    if [[ -n "${ZDEV_PASSWORD:-}" ]]; then
        printf '%s' "$ZDEV_PASSWORD"
        return
    fi
    if [[ ! -s "$PW_FILE" ]]; then
        umask 077
        openssl rand -base64 18 | tr -d '/+=\n' >"$PW_FILE"
        log "generated a new password, saved to $PW_FILE" >&2
    fi
    cat "$PW_FILE"
}

ensure_machine() {
    # podman machine is only required and valid on macOS/Windows
    if [[ "$OS" == "Darwin" ]]; then
        if ! podman machine list --format '{{.Running}}' 2>/dev/null | grep -q true; then
            log "starting podman machine"
            podman machine start
        fi
    fi
}

build() {
    local CONTAINERFILE
    CONTAINERFILE="$(find_containerfile)" || die "docker/dev/Containerfile not found (set ZDEV_CONTAINERFILE=/path/to/Containerfile)"
    ensure_machine
    # The dev image has no build-context COPY, so the context only has to hold the
    # Containerfile. Using its own directory keeps the whole working tree out of it.
    log "building $IMAGE from $(dirname "$CONTAINERFILE")"
    local args=(--build-arg "FRIEND_PASSWORD=$(get_password)")
    [[ -n "${ZDEV_SSH_PUBKEY:-}" ]] && args+=(--build-arg "SSH_PUBKEY=$ZDEV_SSH_PUBKEY")
    podman build -f "$CONTAINERFILE" -t "$IMAGE" "${args[@]}" "$(dirname "$CONTAINERFILE")"
}

confirm_if_busy() {
    podman container exists "$NAME" 2>/dev/null || return 0
    local n
    n="$(podman exec "$NAME" sh -c 'pgrep -c dropbear || true' 2>/dev/null || true)"
    n="${n:-0}"
    if ((n > 1)) && [[ "${ZDEV_FORCE:-}" != 1 ]]; then
        read -r -p "$((n - 1)) active session(s) will be killed. Continue? [y/N] " a
        [[ "$a" == [yY]* ]] || die "aborted (ZDEV_FORCE=1 skips this check)"
    fi
}

run() {
    confirm_if_busy
    ensure_machine
    podman image exists "$IMAGE" || die "image $IMAGE missing, run: $0 rebuild"
    podman volume exists "$SRC_VOL" || podman volume create "$SRC_VOL" >/dev/null
    podman volume exists "$CCACHE_VOL" || podman volume create "$CCACHE_VOL" >/dev/null
    podman rm -f "$NAME" >/dev/null 2>&1 || true

    # Conditionally add GPU flags if running on Linux with NVIDIA CDI configured
    local run_args=()
    if [[ "$OS" == "Linux" ]] && nvidia-ctk cdi list 2>/dev/null | grep -q "nvidia.com/gpu"; then
        log "enabling NVIDIA GPU passthrough via CDI"
        run_args+=(--device nvidia.com/gpu=all --security-opt=label=disable)
    else
        log "running in CPU / software-rendering mode (macOS or non-NVIDIA host)"
    fi

    log "starting container $NAME"
    podman run -d --name "$NAME" \
        "${run_args[@]}" \
        -p "127.0.0.1:${HOST_PORT}:22" \
        -v "$SRC_VOL:/home/frienduser/Projects" \
        -v "$CCACHE_VOL:/home/frienduser/.ccache" \
        "$IMAGE" >/dev/null

    for _ in $(seq 1 15); do
        podman logs "$NAME" 2>&1 | grep -q 'Not backgrounding' && return 0
        sleep 1
    done
    die "dropbear did not start, see: podman logs $NAME"
}

tunnel_stop() {
    if [[ -f "$TUN_PID" ]] && kill -0 "$(cat "$TUN_PID")" 2>/dev/null; then
        kill "$(cat "$TUN_PID")" 2>/dev/null || true
    fi
    rm -f "$TUN_PID"
    pkill -f "cloudflared.*tunnel --url tcp://localhost:${HOST_PORT}" 2>/dev/null || true
}

tunnel_start() {
    command -v cloudflared >/dev/null || die "cloudflared not installed"
    tunnel_stop
    : >"$TUN_LOG"
    log "starting tunnel"
    nohup cloudflared --config /dev/null tunnel --url "tcp://localhost:${HOST_PORT}" >"$TUN_LOG" 2>&1 &
    echo $! >"$TUN_PID"
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
        local url
        url="$(grep -Eo '[a-z0-9-]+\.trycloudflare\.com' "$TUN_LOG" | head -n1 || true)"
        echo "tunnel:   running ($url)"
    else
        echo "tunnel:   stopped"
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
    tunnel_start
    ;;
rebuild)
    build
    run
    log "container recreated; run '$0 tunnel' if the tunnel is down"
    ;;
restart) run ;;
tunnel)
    ensure_machine
    tunnel_start
    ;;
down)
    confirm_if_busy
    tunnel_stop
    podman rm -f "$NAME" >/dev/null 2>&1 || true
    log "stopped"
    ;;
status) status ;;
shell) podman exec -it -u frienduser "$NAME" bash ;;
logs) podman logs -f "$NAME" ;;
password)
    get_password
    echo
    ;;
backup)
    out="zahlen-src-$(date +%Y%m%d-%H%M).tar"
    podman volume export "$SRC_VOL" >"$out" && log "wrote $out"
    ;;
nuke)
    read -r -p "Delete container, image AND volumes (source + ccache)? type 'yes': " a
    [[ "$a" == yes ]] || die "aborted"
    tunnel_stop
    podman rm -f "$NAME" >/dev/null 2>&1 || true
    podman rmi -f "$IMAGE" >/dev/null 2>&1 || true
    podman volume rm -f "$SRC_VOL" "$CCACHE_VOL" >/dev/null 2>&1 || true
    ;;
*) usage ;;
esac
