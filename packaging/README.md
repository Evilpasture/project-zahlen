# Zahlen installed SDK

Enable with `-DZHLN_INSTALL_SDK=ON`. This first packaging profile supports
single-config Linux Release builds with the system libstdc++; the engine
remains shared and the existing optional libraries remain static. It does not
add a static-engine build mode. The system simdjson must provide a CMake CONFIG
package. SDK installation is opt-in outside Docker; the CI builder enables it.

## Packages

Each `Zahlen<Name>` install component has its own CMake config, version file,
exported targets, library, and development headers. For example:

```sh
cmake --install build --prefix /opt/zahlen --component ZahlenEngine
cmake --install build --prefix /opt/zahlen --component ZahlenScreenshot
```

A package manager must install dependencies too: CMake `find_dependency` locates
packages, it does not download them. CI produces one `usr/` tar archive per
component from a **single** builder image and uploads them as `zahlen-sdk-release`
and `zahlen-sdk-asan` artifacts. The Arch split PKGBUILD repackages the release
archives without recompiling. Set a unique `pkgver` for each SDK build and run
`updpkgsums` before publishing; the supplied recipe is not an authenticated
binary repository. Keep sanitized builds in a separate repository/prefix rather
than overwriting release files. GHCR builder images remain the reproducible
source of the artifacts; no extra GHCR credentials or mutable tag is needed by
the test.

| Component / config suffix | Installed target | Dependency packages |
|---|---|---|
| Engine | Engine, Jolt | distro runtime libraries |
| CommandLine | CommandLine | system C++ runtime |
| Signal | Signal | Engine (public diagnostic/reflection headers) |
| HostRunner | HostRunner | Engine, CommandLine, Signal |
| Application | Application | Engine, CommandLine, HostRunner |
| UISchema | UISchema | Engine |
| Serialization | Serialization | Engine, UISchema, system simdjson |
| GLTF | GLTF | Engine, Serialization |
| Screenshot | Screenshot | Engine |
| Netpbm | Netpbm (header-only) | Engine |
| FreeCam | FreeCam | Engine |
| Camera | Camera | Engine, FreeCam |
| CharacterController | CharacterController | Engine, FreeCam |
| SkeletalAnimation | SkeletalAnimation | Engine |
| Animation | Animation | Engine, SkeletalAnimation, CharacterController, Camera, FreeCam |
| Data | (no link target) | optional runtime data |

All targets above use the `Zahlen::` namespace. Helper components are chiefly
transitive Application dependencies. Extra targets not listed here are not yet
part of the installed SDK contract. CMake's native exports preserve static
PRIVATE edges as `LINK_ONLY`; in particular GLTF finds Serialization even though
its public API does not mention JSON. Named C++ module sources/file sets of
exported optional targets are installed; prebuilt BMIs are not portable SDK
artifacts. The separate in-tree `zahlen_module` core umbrella is not exported.

## Consumer

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_game LANGUAGES CXX)
find_package(ZahlenApplication CONFIG REQUIRED)
find_package(ZahlenGLTF CONFIG REQUIRED)
find_package(ZahlenScreenshot CONFIG REQUIRED)
add_executable(my_game main.cpp)
target_link_libraries(my_game PRIVATE
    Zahlen::Application Zahlen::GLTF Zahlen::Screenshot)
```

Configure with `-DCMAKE_PREFIX_PATH=/opt/zahlen -DCMAKE_BUILD_TYPE=Release`.
Application **no longer auto-installs optional modules found in the engine
build**. This is a deliberate behavior change for existing applications, too.
Link and compose what the application uses:

```cpp
#include <Zahlen/App.hpp>
#include <CharacterController/CharacterController.hpp>
// Also find_package(ZahlenCharacterController) and link its target.
// application.Install([](ZHLN::Engine& e) { ZHLN::Character::Install(e); });
```

`WithDefaultModules` remains source-compatible but the base default pass is
empty. Existing games which relied on implicit fonts, terrain, interaction,
cameras, animation, or authoring tools must explicitly install those systems.
No convenience-defaults package is introduced by this patch.

## ABI and tooling

Each config checks compiler ID and exact version, architecture, libstdc++ header
version, dual ABI, debug-container mode, Release configuration, and an explicit
sanitizer profile. A shared build fingerprint includes the engine revision,
compiler flags, Jolt public ABI definitions/options, and annotation setting;
mixing fingerprints is rejected even when CMake package versions match.
Jolt is bundled in Engine with its own public usage requirements, not resolved
against an arbitrary system Jolt. Reflection flags propagate from the existing
targets. The SDK assumes its exact reflection-capable compiler remains installed;
it is not a compiler distribution or a promise of stable ABI across revisions.

Sanitized consumers must pass `-DZahlen_SANITIZER_PROFILE=asan-ubsan`; ASan/UBSan
compile and link requirements propagate. There is no automatic switch from a
release SDK to an instrumented one. Development-only sanitizer hooks embedding
checkout suppression paths are not exported to installed executables. Use your
application's sanitizer environment/suppression policy instead.

## Runtime data and validation

`ZahlenData` installs `share/zahlen/data/base.pak` and
`share/zahlen/resources/`. At runtime set `ZHLN_DATA_DIR=<prefix>/share/zahlen`
(the override is the directory **containing** `data/`, not `data/` itself), or
stage `data/` beside the executable using the existing runtime path policy.
For loose shader observation/loading, stage `resources/` in the game's working
directory as in the existing runtime container. Shared libraries live in `lib/`
and use sibling `$ORIGIN` lookup; an application/container must still configure
its loader path or install under the OS loader's standard prefix. Data and
libraries do not require the source checkout. This does not redesign all
existing development-mode resource hot-reload paths.

CI's `scripts/test-installed-sdk.sh` runs against the exact resolved builder:

1. Component-install and archive each package once.
2. Stage only the self-contained project under `tests/installed/`. It owns its
   source files and prefab scene; no engine application source is copied.
3. Extract only Application, Netpbm, their real transitive dependencies, and Data. Require
   the installed asset pack to be a nonempty regular file, not a cache symlink.
4. Move `/workspace` away, hiding both the checkout and engine build tree.
5. Build the standalone consumer against installed packages only. Its own
   `main.cpp` calls the public `PrefabFactory::CreateBox` API. There are no
   source-tree include paths, engine source files, or build-tree targets.
6. Force Lavapipe, run eight warm-up ticks, then one measured tick and capture.
   Keep the application's default AA. Fail on tick errors or Vulkan validation
   errors, including errors during teardown. A 180-second timeout bounds hangs.
7. Check extent/buffer consistency, finite positive mean luminance, lit pixels,
   and nonconstant image content. Encode the artifact through the installed `Zahlen::Netpbm` plugin using
   `ZHLN::Netpbm::EncodePPM`, without a color-specific assertion. Preserve a valid readback even if content checks
   fail, and upload `cube.ppm` and `consumer-render.log` with the package archives.
8. Require a wrong-profile configuration to fail.

The consumer is independent of `app/cube.cpp`, which remains the original
four-line example. No smoke-test macro or test header is added to that example.
The consumer owns its headless lifecycle, warm-up, capture helper, and scene.
This tests installed Application/Engine/Netpbm integration and nonblank output,
not execution of the engine repository's example, exact geometry/color, golden
pixels, or performance. GLTF and Screenshot are packaged/published but are not
exercised by this prefab test. There is no generated glTF fixture, Python helper,
duplicated PPM encoder, hue assertion, or dependency on an engine source file.

The earlier release execution validated the same installed APIs, Netpbm capture,
and warm-up with a copied application scene. This correction removes that copied
source dependency. Its patch applicability and shell syntax were checked; a new
full container execution of this exact standalone revision has not been run.
