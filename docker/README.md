# docker/ — images, not build contexts

| File | What it is | Built by |
| :--- | :--- | :--- |
| `ci.Dockerfile` | Multi-stage Arch image: `base` → `builder` → `test` / `runner` | `.github/workflows/ci.yml`, `ci-sanitizers.yml`, `local-ci.yml` |
| `release-jammy.Dockerfile` | Ubuntu 22.04 glibc-2.35 release toolchain | `.github/workflows/release.yml` |
| `dev.Dockerfile` | Fedora SSH development box (dropbear) | `scripts/zdev.sh` |

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
`vulkan-headers`, `shader-slang` and LuaJIT come from `pacman` in `ci.Dockerfile`
and their CMake probes prefer the system copy. `extern/slang` is cloned only
when `ZHLN_SLANG_VENDORED=ON`.

The `runner` stage still has two `COPY --from=payload` instructions. Those copy
between **stages**, not from the host tree, so `.dockerignore` never applied to
them; keeping `COPY` there also means the file works with a plain `docker build`
without a BuildKit-only `RUN --mount`.

## Building locally

The context only needs to contain the Dockerfile. An empty directory is enough,
which avoids sending the whole working tree to the daemon:

```bash
mkdir -p /tmp/empty-context
docker build -f docker/ci.Dockerfile \
  --target builder \
  --build-arg ZHLN_GIT_URL="$(git remote get-url origin)" \
  --build-arg ZHLN_GIT_REF="$(git rev-parse HEAD)" \
  -t zahlen-builder:local /tmp/empty-context
```

`docker build -f docker/ci.Dockerfile --target builder .` also works — the
context is simply ignored. For the release toolchain:

```bash
docker build -f docker/release-jammy.Dockerfile \
  --build-arg ZHLN_GIT_REF="$(git rev-parse HEAD)" \
  -t zahlen-release-builder:jammy /tmp/empty-context
```

For the SSH dev box, `scripts/zdev.sh rebuild` finds `docker/dev.Dockerfile`
itself (override with `ZDEV_DOCKERFILE`). Its source lives in the `zahlen-src`
named volume, which the script mounts at `/home/frienduser/Projects`.
