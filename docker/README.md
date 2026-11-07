# docker/ — Containerfiles, not build contexts

Three podman `Containerfile`s, each in its own directory so the default name is
enough: `podman build docker/ci` needs no `-f`.

| File | What it is | Built by |
| :--- | :--- | :--- |
| `ci/Containerfile` | Multi-stage Arch image: `base` → `builder` → `test` / `runner` | `.github/workflows/ci.yml`, `ci-sanitizers.yml`, `local-ci.yml` |
| `release-jammy/Containerfile` | Ubuntu 22.04 glibc-2.35 release toolchain | `.github/workflows/release.yml` |
| `dev/Containerfile` | Fedora SSH development box (dropbear) | `scripts/zdev.sh` |

## There is no `.dockerignore`, and no `COPY` from the build context

Both build images `git clone` the repository **inside the container**:

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

## Naming

The directory is still called `docker/` so the workflow `file:` entries,
`scripts/zdev.sh` and the docs keep a stable path; nothing in it needs a Docker
daemon, and every local command above is podman. The GitHub workflows do still
use `docker/build-push-action` and `docker buildx`, because GitHub-hosted
runners ship Docker rather than podman — those tools read Containerfiles
unchanged. Migrating CI itself to podman is a separate change.
