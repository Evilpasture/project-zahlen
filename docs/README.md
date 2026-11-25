# Documentation

Documentation here describes the current revision, not every released version.
For a released toolchain, read these files at its release tag and use that
release's assets together.

## Choose an audience

| Audience / task | Canonical document |
|---|---|
| Build and runtime requirements | [Requirements](REQUIREMENTS.md) |
| Game developer: install, link, run | [Consuming Zahlen](CONSUMING.md) |
| Game developer: upgrade an installed toolchain | [Upgrading](UPGRADING.md) |
| Game developer: create a scene and add gameplay | [First game](FIRST_GAME.md) |
| Engine contributor: source builds, containers | [Engine development](DEVELOPMENT.md) |
| Package maintainer: exports, manifests, ABI | [Packaging](../packaging/README.md) |
| Package-manager user / contributor: commands, bootstrap, trust | [zpkg](../utils/zpkg/README.md) |
| Engine architecture | [Architecture](../include/ARCHITECTURE.md) |
| Renderer orchestration, algorithms, and shaders | [Renderer](../src/render/README.md) |
| Vulkan backend and resource lifetimes | [Vulkan](../src/vulkan/README.md) |
| Application API and lifecycle | [COMApp](COMApp.md) |
| Tests and golden images | [Tests](../tests/README.md) |
| Engine tools | [Host programs](../utils/README.md) |
| Containers | [Docker](../docker/README.md) |

Other `COM*.md` pages are subsystem/design references, not alternative engine
installation guides. Files under `todo/` and `*_TODO.md` describe planning work,
not setup instructions.
