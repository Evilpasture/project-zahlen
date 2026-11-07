#!/usr/bin/env bash
# zgame: run the game-development container alongside zdev.sh's bare Fedora box.
#
# zdev.sh hardcodes the name, image, port and volumes of the container you use to
# share the VM. Setting only ZDEV_CONTAINERFILE=docker/gamedev/Containerfile would
# rebuild *that* image and recreate *that* container on port 2222. Everything here
# has a separate identity and its own state/password directory, so both boxes can
# be up at the same time. Override any default with the ZGAME_* variables below.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

NAME=${ZGAME_CONTAINER:-zahlen-game-container}
IMAGE=${ZGAME_IMAGE:-zahlen-gamedev}
PORT=${ZGAME_PORT:-2224}
CONTAINERFILE=${ZGAME_CONTAINERFILE:-"$ROOT/docker/gamedev/Containerfile"}
GAME_VOL=${ZGAME_GAME_VOL:-zahlen-game-src}
STORE_VOL=${ZGAME_STORE_VOL:-zahlen-game-store}
RELEASE_VOL=${ZGAME_RELEASE_VOL:-zahlen-game-release}
CCACHE_VOL=${ZGAME_CCACHE_VOL:-zahlen-game-ccache}
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/zgame"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/zgame"
PW_FILE="$CONF/password"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$STATE" "$CONF"
for bin in podman openssl; do
    command -v "$bin" >/dev/null || die "$bin not found in PATH"
done

password() {
    if [[ -n ${ZGAME_PASSWORD:-} ]]; then printf '%s' "$ZGAME_PASSWORD"; return; fi
    if [[ ! -s "$PW_FILE" ]]; then
        umask 077
        openssl rand -base64 18 | tr -d '/+=\n' > "$PW_FILE"
        log "generated a password at $PW_FILE" >&2
    fi
    cat "$PW_FILE"
}

build() {
    [[ -f "$CONTAINERFILE" ]] || die "Containerfile not found: $CONTAINERFILE"
    log "building $IMAGE from $(dirname "$CONTAINERFILE")"
    local args=(--build-arg "FRIEND_PASSWORD=$(password)")
    [[ -n ${ZGAME_SSH_PUBKEY:-} ]] && args+=(--build-arg "SSH_PUBKEY=$ZGAME_SSH_PUBKEY")
    [[ -n ${ZGAME_RELEASE_TAG:-} ]] && args+=(--build-arg "ZHLN_RELEASE_TAG=$ZGAME_RELEASE_TAG")
    podman build -f "$CONTAINERFILE" -t "$IMAGE" "${args[@]}" "$(dirname "$CONTAINERFILE")"
}

up() {
    podman image exists "$IMAGE" || build
    for v in "$GAME_VOL" "$STORE_VOL" "$RELEASE_VOL" "$CCACHE_VOL"; do
        podman volume exists "$v" || podman volume create "$v" >/dev/null
    done
    podman rm -f "$NAME" >/dev/null 2>&1 || true

    local env_args=() extra=() gpu=()
    [[ -n ${GH_TOKEN:-} ]]        && env_args+=(-e "GH_TOKEN=$GH_TOKEN")
    [[ -n ${GITHUB_TOKEN:-} ]]    && env_args+=(-e "GITHUB_TOKEN=$GITHUB_TOKEN")
    [[ -n ${ZGAME_RELEASE_TAG:-} ]]  && env_args+=(-e "ZHLN_RELEASE_TAG=$ZGAME_RELEASE_TAG")
    [[ -n ${ZGAME_RELEASE_REPO:-} ]] && env_args+=(-e "ZHLN_RELEASE_REPO=$ZGAME_RELEASE_REPO")
    if [[ -n ${ZGAME_RUN_ARGS:-} ]]; then read -r -a extra <<< "$ZGAME_RUN_ARGS"; fi
    if [[ $(uname -s) == Linux ]] && command -v nvidia-ctk >/dev/null 2>&1 \
       && nvidia-ctk cdi list 2>/dev/null | grep -q 'nvidia.com/gpu'; then
        gpu=(--device nvidia.com/gpu=all --security-opt=label=disable)
        log 'enabling NVIDIA GPU passthrough via CDI'
    fi

    # The entrypoint downloads and verifies the release before dropbear starts,
    # so a first boot can take a few minutes. Watch with: scripts/zgame.sh logs
    log "starting $NAME (installing the release on first boot)"
    podman run -d --name "$NAME" \
        -p "127.0.0.1:${PORT}:22" \
        -v "$GAME_VOL:/home/frienduser/Projects" \
        -v "$STORE_VOL:/home/frienduser/.local/share/zahlen" \
        -v "$RELEASE_VOL:/home/frienduser/.cache/zahlen-release" \
        -v "$CCACHE_VOL:/home/frienduser/.ccache" \
        "${env_args[@]}" "${extra[@]}" "${gpu[@]}" \
        "$IMAGE" >/dev/null

    for _ in $(seq 1 180); do
        if podman logs "$NAME" 2>&1 | grep -q 'Not backgrounding'; then
            log "ready: ssh -p $PORT frienduser@127.0.0.1"
            log "starter project: ~/Projects/game (inside the container)"
            return 0
        fi
        [[ $(podman inspect -f '{{.State.Running}}' "$NAME" 2>/dev/null || echo false) == true ]] \
            || die "container exited; see: $0 logs"
        sleep 2
    done
    die "dropbear did not start within 6 minutes; see: $0 logs"
}

usage() {
    cat <<EOF
usage: $(basename "$0") <command>

  up        build the image if missing, then recreate the container
  rebuild   rebuild the image and recreate the container (volumes are kept)
  down      stop and remove the container (volumes are kept)
  shell     interactive shell as frienduser in the running container
  logs      follow container logs (release fetch + dropbear)
  status    container state and volumes
  password  print the SSH password baked into the image
  nuke      remove container, image and all four volumes (asks first)

Both containers can run at once: zdev.sh stays on port 2222, zgame uses 2224.
Environment: ZGAME_PORT, ZGAME_IMAGE, ZGAME_CONTAINER, ZGAME_CONTAINERFILE,
ZGAME_PASSWORD, ZGAME_SSH_PUBKEY, ZGAME_RELEASE_TAG, ZGAME_RELEASE_REPO,
ZGAME_RUN_ARGS (extra podman run flags), plus GH_TOKEN / GITHUB_TOKEN.
EOF
}

case "${1:-}" in
    up)       up ;;
    rebuild)  build; up ;;
    down)     podman rm -f "$NAME" >/dev/null 2>&1 || true; log stopped ;;
    shell)    podman exec -it -u frienduser "$NAME" bash ;;
    logs)     podman logs -f "$NAME" ;;
    status)   podman ps -a --filter "name=^${NAME}\$" --format 'container: {{.Names}} {{.Status}}' || true
              printf 'volumes:  %s\n' "$GAME_VOL $STORE_VOL $RELEASE_VOL $CCACHE_VOL" ;;
    password) password; echo ;;
    nuke)
        read -r -p "Delete $NAME, $IMAGE and all four volumes? type 'yes': " a
        [[ $a == yes ]] || die aborted
        podman rm -f "$NAME" >/dev/null 2>&1 || true
        podman rmi -f "$IMAGE" >/dev/null 2>&1 || true
        podman volume rm -f "$GAME_VOL" "$STORE_VOL" "$RELEASE_VOL" "$CCACHE_VOL" >/dev/null 2>&1 || true ;;
    *) usage ;;
esac
