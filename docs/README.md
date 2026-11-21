# Documentation

Documentation here describes the current revision, not every released version.
For a released toolchain, read these files at its release tag and use that
release's assets together.

## Choose an audience

| Audience / task | Canonical document |
|---|---|
| Game developer: install, link, run | [Consuming Zahlen](CONSUMING.md) |
| Engine contributor: source builds, containers | [Engine development](../DEVELOPMENT.md) |
| Package maintainer: exports, manifests, ABI | [Packaging](../packaging/README.md) |
| Package-manager user / contributor: commands, bootstrap, trust | [zpkg](../utils/zpkg/README.md) |
| Engine architecture | [Architecture](../include/ARCHITECTURE.md) |
| Renderer architecture and Vulkan implementation | [Rendering subsystem](../src/vulkan/RENDER.md) |
| Application API and lifecycle | [COMApp](COMApp.md) |
| Tests and golden images | [Tests](../tests/README.md) |
| Engine tools | [Host programs](../utils/README.md) |
| Containers | [Docker](../docker/README.md) |

Other `COM*.md` pages are subsystem/design references, not alternative engine
installation guides. Files under `todo/` and `*_TODO.md` describe planning work,
not setup instructions.
