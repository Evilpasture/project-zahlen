#!/usr/bin/env bash
# Iterate on app/main.cpp against the already-built CI builder image.
set -euo pipefail

runtime=${ZHLN_CONTAINER_RUNTIME:-podman}
image=${ZHLN_DEV_IMAGE:-ghcr.io/evilpasture/project-zahlen-builder:latest}
name=${ZHLN_DEV_CONTAINER:-zahlen-dev}
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dir=${ZHLN_DEV_DIR:-"$root/.zahlen-dev"}
main="$dir/main.cpp"

usage() {
    cat <<'HELP'
Usage: scripts/dev-container.sh {init|build|run|test|shell|reset} [args...]
  init   Pull the image, create a persistent container, and copy out main.cpp.
  build  Copy .zahlen-dev/main.cpp into the container and rebuild only zahlen.
  run    Run the rebuilt app (remaining arguments are passed through).
  test   Run the existing ztest suite (remaining arguments are passed through).
  shell  Open a shell in the container for inspection.
  reset  Remove the container; the local main.cpp is preserved.
Environment: ZHLN_CONTAINER_RUNTIME (podman/docker), ZHLN_DEV_IMAGE,
             ZHLN_DEV_CONTAINER, ZHLN_DEV_DIR.
HELP
}

command -v "$runtime" >/dev/null || { echo "Missing container runtime: $runtime" >&2; exit 1; }
cmd=${1:-}; (($# == 0)) || shift
case "$cmd" in
  init)
    if "$runtime" container inspect "$name" >/dev/null 2>&1; then
      echo "Container $name already exists. Use reset before changing images." >&2
      exit 1
    fi
    mkdir -p "$dir"
    "$runtime" pull "$image"
    # Host networking avoids rootless Podman's /dev/net/tun requirement in Arena.
    # Override the entrypoint defensively; the CI builder normally has none.
    "$runtime" create --name "$name" --network host --entrypoint sleep "$image" infinity >/dev/null
    if [[ ! -e "$main" ]]; then
      "$runtime" cp "$name:/workspace/app/main.cpp" "$main"
    else
      echo "Keeping existing $main (it may be from a different revision)."
    fi
    "$runtime" start "$name" >/dev/null
    echo "Edit $main, then run scripts/dev-container.sh build."
    ;;
  reset)
    if "$runtime" container inspect "$name" >/dev/null 2>&1; then
      "$runtime" rm -f "$name" >/dev/null
    fi
    echo "Container removed; $main was not deleted."
    ;;
  build|run|test|shell)
    if ! "$runtime" container inspect "$name" >/dev/null 2>&1; then
      echo "Run scripts/dev-container.sh init first." >&2; exit 1
    fi
    if [[ $("$runtime" inspect -f '{{.State.Running}}' "$name") != true ]]; then
      "$runtime" start "$name" >/dev/null
    fi
    case "$cmd" in
      build)
        [[ -f "$main" ]] || { echo "Missing $main" >&2; exit 1; }
        # Copy only when the contents differ, so a no-op build stays a no-op.
        if ! "$runtime" exec "$name" cat /workspace/app/main.cpp | cmp -s "$main" -; then
          "$runtime" cp "$main" "$name:/workspace/app/main.cpp"
          # Ensure Ninja sees the change even when the host file has an old mtime.
          "$runtime" exec "$name" touch /workspace/app/main.cpp
        fi
        "$runtime" exec -w /workspace "$name" cmake --build build --target zahlen "$@"
        ;;
      run) "$runtime" exec -w /workspace "$name" ./build/bin/zahlen "$@" ;;
      test) "$runtime" exec -w /workspace "$name" ./build/bin/ztest --test-dir build -V "$@" ;;
      shell)
        if [[ -t 0 && -t 1 ]]; then
          "$runtime" exec -it -w /workspace "$name" /bin/bash "$@"
        else
          "$runtime" exec -i -w /workspace "$name" /bin/bash "$@"
        fi
        ;;
    esac
    ;;
  *) usage; [[ -z "$cmd" || "$cmd" == --help || "$cmd" == -h ]] ;;
esac
