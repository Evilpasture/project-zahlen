# Zahlen toolchains

Audience: engine package maintainers. Game developers should start with the
[consumer guide](../docs/CONSUMING.md); the [zpkg reference](../utils/zpkg/README.md)
owns package-manager command semantics.

A toolchain contains the engine and optional packages installed in a per-user
store by `zpkg`.

Enable packaging with `-DZHLN_INSTALL_TOOLCHAIN_PACKAGES=ON`. This profile
supports single-config Linux Release builds with the system libstdc++; the
engine is shared, optional source packages compile into static libraries in the consumer build, and there is no
static-engine mode. The system simdjson must provide a CMake CONFIG package.
Packaging is opt-in.

## Packages

Each `Zahlen<Name>` install component has its own CMake config, version file,
exported targets, library and headers, plus two metadata files that let `zpkg`
work without a database:

* `share/zahlen/toolchain.json` — the toolchain id, version, commit, compiler,
  processor, profile and ABI hash.
* `share/zahlen/manifests/zahlen-<package>.json` — the package name, its
  component, the packages it requires and the targets it provides.

zpkg parses both files with simdjson and rejects missing required keys or
incorrect types. `cmake/Toolchains.cmake` generates the metadata and dependency
graph.

CI produces one `usr/` archive per component from a **single** builder image and
publishes the release toolchain (sanitized CI builds are not published):

```
zahlen-<component>-<toolchain-id>.tar.gz
zahlen-<component>-<toolchain-id>.tar.gz  # one per component
SHA256SUMS
```

`<toolchain-id>` looks like `0.x.x-xxxxxxx-gcc-16.x.x-x86_64-release` and is
derived from the same inputs as the ABI hash. Use separate toolchain ids for
sanitized builds.

| Component / config suffix | Installed target | Requires |
|---|---|---|
| Engine | Engine (`libzahlen_engine.so`) | distro runtime libraries |
| CommandLine | CommandLine | system C++ runtime |
| Image | Image | system C++ runtime |
| Signal | Signal | Engine |
| Screenshot | Screenshot + `libzahlen_plugin_screenshot.so` + manifest | Engine, Image |
| HostRunner | HostRunner | Engine, CommandLine, Signal |
| Application | Application | Engine, CommandLine, HostRunner, Screenshot |
| UI | UI (`GUI::Context`, `HostUI`, embedded default font) | Engine |
| UISchema | UISchema | Engine, UI |
| Serialization | Serialization | Engine, UISchema, system simdjson |
| GLTF | GLTF + `libzahlen_plugin_gltf.so` + manifest | Engine, Serialization, Image |
| Netpbm | Netpbm (header-only) | Engine |
| Terrain | Terrain + `libzahlen_plugin_terrain.so` + manifest | Engine |
| VFX | VFX + `libzahlen_plugin_vfx.so` + manifest | Engine |
| FreeCam | FreeCam + `libzahlen_plugin_freecam.so` + manifest | Engine |
| Camera | Camera | Engine |
| CharacterController | CharacterController + `libzahlen_plugin_character_controller.so` + manifest | Engine |
| AnimationControls | AnimationControls | Engine |
| Animation | Animation + `libzahlen_plugin_animation.so` + manifest | Engine, AnimationControls, CharacterController, Camera, FreeCam |
| Cooker | zcook (installed as `bin/zcook`) | Engine |

All targets use the `Zahlen::` namespace. The `Requires` column is what the
archives themselves declare; `zpkg info` prints the same thing for what is
installed. CMake's native exports preserve static PRIVATE edges as `LINK_ONLY`;
in particular GLTF finds Serialization even though its public API does not
mention JSON. Named C++ module sources are installed; prebuilt BMIs are not
portable artifacts. The in-tree `zahlen_module` umbrella is not exported.

## Publishing metadata

Create the package index with:

```sh
zpkg index ./toolchain-output --out ./toolchain-output/index.json
```

Sign `SHA256SUMS` and `index.json` before publishing them alongside the package
archives and public key. See the [signature format](../utils/zpkg/README.md#signing).
For package-manager operations, use the [zpkg command reference](../utils/zpkg/README.md#commands).

## Store and runtime paths

See the [zpkg store reference](../utils/zpkg/README.md#store-layout) for layout,
privilege restrictions, active-toolchain selection, and removal behavior.

Installed libraries use `$ORIGIN` runtime search paths. The store-library CI
check runs without `LD_LIBRARY_PATH`. The deployment container uses a different,
flat `/app` layout and sets `LD_LIBRARY_PATH=/app`; custom game deployments must
configure their own library search paths.

## ABI and profiles

`ZahlenEngine` ships a **single** shared library (`libzahlen_engine.so`) and the
stripped public COM/Core headers; `ZahlenCooker` ships `bin/zcook`. Every
optional extension and plugin (`Application`, `HostRunner`, `UI`,
`CommandLine`, `Signal`, `Image`, `GLTF`, `Scene`, `Serialization`, `Fonts`,
`UISchema`, `Screenshot`, `Netpbm`, `FreeCam`, `Camera`, `CharacterController`,
`AnimationControls`, `Animation`, `Terrain`, `VFX`, `Widgets`, `Console`,
`Editor`, `Benchmark`, `BenchmarkViewer`, `Scripting`, `Lua`) ships as a
relocatable **source/header package** under `share/zahlen/src/<pkg>` — zero
prebuilt `.a` archives and zero compiler-specific module BMIs. `ZahlenFreeCam`
and `ZahlenScreenshot` additionally ship dynamically loadable `COM::IAppLayer`
shared libraries (`lib/zahlen/plugins/libzahlen_plugin_{freecam,screenshot}.so`,
exporting `ZHLN::COM::CreateAppLayer` and its ABI witness) for `App::LoadPlugin`,
`App::TryLoadPlugin`, and `--plugin`. Consumers compile
those sources with their own compiler release, flags, and build type (`Debug`,
`RelWithDebInfo`, or `Release`) directly against the single Release
`libzahlen_engine.so`.

Jolt Physics, Vulkan/volk/VMA, miniaudio, meshoptimizer, SPIRV-Reflect,
and vendored GLFW/zstd archives are completely sealed inside
`libzahlen_engine.so` via hidden visibility presets, `-Wl,--exclude-libs,ALL`,
and the linker export allowlist (`cmake/zahlen_engine.map`), audited by
`configure/check_engine_exports.py`; no `libJolt*` library, header, compile
definition, or third-party symbol is installed or leaked to consumers.
`libzahlen_engine.so` does not embed or interpose a custom allocator (first-party
in-tree host executables link `mimalloc` directly on non-Apple, non-sanitizer
builds, while SDK consumers own their process allocator).

Each config checks the compiler family, architecture, libstdc++ dual-ABI switch
(`_GLIBCXX_USE_CXX11_ABI`), absence of `_GLIBCXX_DEBUG`, an explicit sanitizer
profile, and compiles `cmake/probes/pr125280-static-member.cpp` under
`-std=c++26 -freflection`. It does **not** pin a compiler patch version, git
commit hash, `__GLIBCXX__` release date, or consumer `CMAKE_BUILD_TYPE`; COM
input chains verify struct layouts at runtime via `LayoutHash`.

Sanitized consumers pass `-DZahlen_SANITIZER_PROFILE=asan-ubsan`; ASan/UBSan
compile and link requirements propagate and there is no automatic switch from a
release toolchain to an instrumented one. The asan-ubsan toolchain also installs
a compiled hook object (`lib/zahlen/zahlen_sanitizer_options.o`) that
`ZahlenEngineConfig.cmake` attaches to consumer executables. Its rules are
compiled in, so no suppressions file has to be found at run time, and the
configure step fails loudly if the object is missing rather than silently
running without hooks.

## Runtime data

The toolchain ships no content, only the tool that makes it. `ZahlenCooker`
installs `bin/zcook` and `share/zahlen/tools/export_metadata.py`; a project cooks
its own pack:

```sh
cooker=$(zpkg which zcook)
tools="$(zpkg prefix)/share/zahlen/tools"
"$cooker" ninja --out assets.ninja --source "$PWD" --engine-tools "$tools" \
    --self "$cooker" --no-blender
ninja -f assets.ninja          # writes ./data/base.pak
```

The engine then finds `data/base.pak` in the working directory, beside the
executable, or under `data/` of the build tree; `ZHLN_DATA_DIR` overrides the
search with the directory **containing** `data/`.
