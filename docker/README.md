# docker/ — Containerfiles, not build contexts

Four podman `Containerfile`s, each in its own directory so the default name is
enough: `podman build docker/ci` needs no `-f`.

| File | What it is | Built by |
| :--- | :--- | :--- |
| `ci/Containerfile` | Multi-stage Arch image: `base` → `builder` → `test` / `runner` | `.github/workflows/ci.yml`, `ci-sanitizers.yml`, `local-ci.yml` |
| `release-jammy/Containerfile` | Ubuntu 22.04 glibc-2.35 release toolchain | `.github/workflows/release.yml` |
| `dev/Containerfile` | Fedora SSH development box (dropbear) | `scripts/zdev.sh` |
| `gamedev/Containerfile` | Fedora 44 game-dev box: zpkg + app-only workflows | you, locally |

## There is no `.dockerignore`, and no `COPY` from the build context

The two build images `git clone` the repository **inside the container**:

```dockerfile
ARG ZHLN_GIT_URL=https://github.com/Evilpasture/project-zahlen.git
ARG ZHLN_GIT_REF=main
RUN git init -q /workspace && cd /workspace \
 && git remote add origin "${ZHLN_GIT_URL}" \
 && git fetch --depth 1 origin "${ZHLN_GIT_REF}" \
 && git checkout -q --detach FETCH_HEAD \
 && git submodule update --init --depth 1 <the ten unpackaged submodules>
```

`git fetch --depth 1 origin <ref>` accepts a branch, a tag **or** a raw commit
SHA, so CI pins `ZHLN_GIT_REF=${{ github.sha }}` and `ZHLN_GIT_URL` to the
repository under test. A filtered-out file can no longer turn into a configure
error halfway through a build, and the checkout is a real repository — submodule
`.git` files included — which is what Slang's `GitVersion.cmake` expects.

Only the ten submodules with no distro package are fetched. `glfw`, `simdjson`,
`vulkan-headers`, `shader-slang` and LuaJIT come from `pacman` in
`ci/Containerfile` and their CMake probes prefer the system copy.
`extern/slang` is cloned only when `ZHLN_SLANG_VENDORED=ON`.

The `runner` stage still has two `COPY --from=payload` instructions. Those copy
between **stages**, not from the host tree, so `.dockerignore` never applied to
them; keeping `COPY` there also means the file works with a plain `podman build`
without a BuildKit-only `RUN --mount`.

## Building locally with podman

The context only needs to contain the Containerfile. Its own directory is enough,
which avoids sending the whole working tree to the build:

```bash
podman build --target builder \
  --build-arg ZHLN_GIT_URL="$(git remote get-url origin)" \
  --build-arg ZHLN_GIT_REF="$(git rev-parse HEAD)" \
  -t zahlen-builder:local docker/ci
```

For the release toolchain:

```bash
podman build \
  --build-arg ZHLN_GIT_REF="$(git rev-parse HEAD)" \
  -t zahlen-release-builder:jammy docker/release-jammy
```

For the SSH dev box, `scripts/zdev.sh rebuild` finds `docker/dev/Containerfile`
itself (override with `ZDEV_CONTAINERFILE`; the old `ZDEV_DOCKERFILE` name is
still accepted). Its source lives in the `zahlen-src` named volume, which the
script mounts at `/home/frienduser/Projects`.

`-f docker/ci/Containerfile .` also works — the context is simply ignored.

## Game-development box (`gamedev/Containerfile`)

A superset of `dev/Containerfile`: the same Fedora 44 + `dnf` + dropbear +
`frienduser` SSH box, plus the two consumer workflows. No engine checkout, no
submodules, no build-context `COPY`. The entrypoint is the only thing that
touches the network at run time:

1. Resolve `ZHLN_RELEASE_REPO` at `ZHLN_RELEASE_TAG` (`latest` by default) via
   the GitHub Releases API and download every asset except the preview PNG.
2. Verify `SHA256SUMS.tools` before executing the bundled `zpkg`, put the binary
   in `~/.local/bin/zpkg` (owned by `frienduser`), require both the signed key
   and `SHA256SUMS.ed25519`, then `zpkg key add` +
   `zpkg install --require-checksums --require-signatures`. Every `zpkg` call
   runs as `frienduser` and writes only to its per-user store.
3. `zpkg use <id>` from `toolchain.json` (install alone does not switch an
   already-active toolchain), enable shims, and extract
   `cube-consumer-*.tar.gz` into `ZHLN_GAME_DIR` once, so your game is never
   overwritten by a later release.
4. `exec` the CMD, i.e. dropbear. Login shells get `CMAKE_PREFIX_PATH` from the
   store via `/etc/profile.d`, but nothing is installed system-wide.

### Running it

`scripts/zgame.sh` is the short version of the commands below. It has its own
container name, image, port (2224), volumes and password file, so it runs
**alongside** `scripts/zdev.sh` (port 2222) without touching the bare Fedora box
you use to share the VM:

```bash
scripts/zgame.sh rebuild        # build + recreate (volumes are kept)
scripts/zgame.sh logs           # watch the first-boot release install
scripts/zgame.sh shell          # or: ssh -p 2224 frienduser@127.0.0.1
scripts/zgame.sh down
```

Do **not** just set `ZDEV_CONTAINERFILE=docker/gamedev/Containerfile scripts/zdev.sh
rebuild`: `zdev.sh` hardcodes `zahlen-dev` / `zahlen-dev-container` / port 2222 /
`zahlen-src`, so that would replace your existing dev container and mount your
source volume into the game box. Use `zgame.sh`, or override every identity
explicitly with the raw commands:

```bash
podman build --build-arg FRIEND_PASSWORD=... -t zahlen-gamedev docker/gamedev

for v in zahlen-game-src zahlen-game-store zahlen-game-release zahlen-game-ccache; do
  podman volume exists "$v" || podman volume create "$v" >/dev/null
done

podman run -d --name zahlen-game-container \
  -p 127.0.0.1:2224:22 \
  -e ZHLN_RELEASE_TAG=latest \
  -v zahlen-game-src:/home/frienduser/Projects \
  -v zahlen-game-store:/home/frienduser/.local/share/zahlen \
  -v zahlen-game-release:/home/frienduser/.cache/zahlen-release \
  -v zahlen-game-ccache:/home/frienduser/.ccache \
  zahlen-gamedev

podman logs -f zahlen-game-container      # first boot downloads the release
ssh -p 2224 frienduser@127.0.0.1
cmake -S ~/Projects/game -B ~/Projects/game/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(zpkg prefix)"
cmake --build ~/Projects/game/build
```

The volumes are optional but keep the game, the toolchain store, the downloaded
release and ccache across container recreations. Setting `ZHLN_RELEASE_TAG` to a
tag whose assets are already cached skips the API call entirely, so the box
starts offline. `GH_TOKEN` / `GITHUB_TOKEN` raise the anonymous 60 req/hr limit
and open private repos; `ZHLN_RELEASE_REPO` points at a fork. `zgame.sh` forwards
those tokens and `ZGAME_RELEASE_TAG` / `ZGAME_RELEASE_REPO`, and accepts extra
`podman run` flags via `ZGAME_RUN_ARGS`.

**zpkg is not a system package manager here.** The binary, the store, the
trusted keys, the shims and the downloaded release all live under
`/home/frienduser`, and `zpkg` is never invoked as root. There is no
`dnf install zpkg`, no `/usr/bin/zpkg`, no `ZHLN_ROOT` pointed at `/opt` or
`/usr`, and no daemon or database. Root only runs the container entrypoint to
fetch into the cache and `exec` dropbear; the only root-owned file outside the
image's own tooling is the login profile snippet, which just exports
`CMAKE_PREFIX_PATH`.

**Compiler.** The compiler is exactly `gcc`/`g++` from `dnf` — no pinned
prebuilt, no downgrade. Installed package configs gate on compiler family and
`GNU >= 16.1.1` (`ZHLN_GCC_MIN`), not an exact patch version, so Fedora's 16.2.1
satisfies them. The real runtime requirement is the `GLIBCXX_x.y.z` symbol
versions the engine `.so` asks for, which the linker/loader reports precisely if
a distro is too old. **Caveat:** the published `v0.1.0` archives were packaged
before that fix and still carry the old exact `16.2.0` check, so re-publish a
release (or build the toolchain from source with dnf's compiler and install it
with zpkg) before the `cmake -S` above will configure. The entrypoint prints
both versions so this is never a surprise.

**Ubuntu 24.04.** Noble is the release's proven render host: the workflow's
`render` job runs there because kisak-mesa publishes Mesa 26.2 for noble, the
first lavapipe with `VK_EXT_descriptor_heap`. It is *not* a stage in this image;
Fedora's own Mesa renders here. Run the same release on noble when a frame needs
a driver newer than Fedora ships.

## Naming

The directory is still called `docker/` so the workflow `file:` entries,
`scripts/zdev.sh` and the docs keep a stable path; nothing in it needs a Docker
daemon, and every local command above is podman. The GitHub workflows do still
use `docker/build-push-action` and `docker buildx`, because GitHub-hosted
runners ship Docker rather than podman — those tools read Containerfiles
unchanged. Migrating CI itself to podman is a separate change.
