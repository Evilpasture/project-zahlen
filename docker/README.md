# Containers

| Containerfile | Use |
|---|---|
| `ci/Containerfile` | Engine builder, tests, and runtime |
| `release-jammy/Containerfile` | Native release toolchain |
| `dev/Containerfile` | SSH source-development environment |
| `gamedev/Containerfile` | SSH game-development environment using released packages |

## Build locally

Build images fetch the selected Git ref. Uncommitted local changes are not
included.

```sh
podman build --target builder \
  --build-arg ZHLN_GIT_URL="$(git remote get-url origin)" \
  --build-arg ZHLN_GIT_REF="$(git rev-parse HEAD)" \
  -t zahlen-builder:local docker/ci

podman build \
  --build-arg ZHLN_GIT_REF="$(git rev-parse HEAD)" \
  -t zahlen-release-builder:jammy docker/release-jammy
```

For local `ci/Containerfile` builds, select sanitizers with
`--build-arg ZHLN_USE_SANITIZERS=ON` or `OFF`.

## Prebuilt entry-point environment

The builder image is `ghcr.io/evilpasture/project-zahlen-builder:release`.
Pin an image digest for reproducible use:

```sh
export ZHLN_DEV_IMAGE=ghcr.io/evilpasture/project-zahlen-builder@sha256:<digest>
scripts/dev-container.sh init
```

See [entry-point iteration](../DEVELOPMENT.md#prebuilt-entry-point-development-container)
for build/run commands. The image contains its own source and configured build;
it may differ from your checkout.

## SSH environments

Use `scripts/zdev.sh` for engine source development and `scripts/zgame.sh` for
released-package game development. They use separate container identities and
persistent volumes. Do not substitute the gamedev Containerfile into `zdev.sh`.

```sh
scripts/zgame.sh rebuild
scripts/zgame.sh logs
scripts/zgame.sh shell
scripts/zgame.sh down
```

The game environment downloads and verifies a release on first boot. Use
`ZGAME_RELEASE_TAG` to select a release and `ZGAME_RELEASE_REPO` to select its
repository. `ZGAME_RUN_ARGS` accepts extra container runtime arguments.
Volumes preserve the game, package store, release cache, and compiler cache
across container recreation.

Inside the game environment, follow the [consumer guide](../docs/CONSUMING.md)
to configure and build your game.
