# Consuming Zahlen as a game developer

Audience: game developers using released Linux x86_64 packages. This page is the
canonical consumer setup guide for this revision; use the version at your release
tag when consuming an older release.

Install Zahlen with `zpkg` into a directory you own. You do not clone or build the
engine, embed this repository, or run its governance checks. The engine is a
prebuilt shared library; optional packages such as Application ship sources that
CMake compiles as part of your game.

## Prerequisites

See [Requirements](REQUIREMENTS.md) for build tools, runtime libraries,
windowing, and Vulkan driver and hardware support for this release.

## 1. Get zpkg

Download `zpkg-linux-x86_64` from the
[GitHub release](https://github.com/Evilpasture/project-zahlen/releases) you intend
to use, make it executable, and put it on `PATH`. The release supplies
`SHA256SUMS.tools` for the manager binary; verify it before execution.
Checksums detect corruption, but are not an independent source of publisher trust.

Building the manager is an optional maintainer/bootstrap path, not a prerequisite
for using the engine. It needs a C++23 compiler, simdjson and zlib, and OpenSSL
for signature verification. zstd and libcurl enable additional formats and
HTTPS remotes. See the [zpkg build reference](../utils/zpkg/README.md#building)
if you choose that path.

## 2. Install the engine

Download `zahlen-release-key.pub` from your release and verify its fingerprint
through a trusted channel. Trust the publisher once, then install:

```sh
zpkg key add zahlen-release-key.pub --name "zahlen release"
zpkg install
```

`install` selects Application and its dependencies from the latest official
GitHub release. It downloads packages and requires a trusted signature and
matching archive hashes. A changed signing key requires explicit trust setup.

To select components or pin a release:

```sh
zpkg install application image
zpkg install application --version <release-tag>
```

The first installed toolchain becomes active. Later installs preserve the active
toolchain; use `zpkg use <id>` to switch. Repeated installs keep packages already
present. `zpkg list` shows installed toolchains and `zpkg prefix` prints the active
prefix. For reproducible builds, use `zpkg prefix <id>`.

For offline archives and custom remotes, see the
[zpkg reference](../utils/zpkg/README.md#commands).

## 3. Build your game

Create `my-game/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.28)
project(MyGame LANGUAGES CXX)
find_package(ZahlenApplication CONFIG REQUIRED)
add_executable(my-game main.cpp)
target_link_libraries(my-game PRIVATE Zahlen::Application)
```

And `my-game/main.cpp`:

```cpp
#include <Zahlen/App.hpp>

int main(int argc, char* argv[]) {
    return ZHLN::App().Main(argc, argv);
}
```

This minimal host is not a complete game or asset pipeline. After building and
running it below, follow [From an empty host to a first scene](FIRST_GAME.md)
to add a layer, scene, physics and input. Compose optional layers explicitly;
Application does not auto-install every optional module that happened to be
present in the engine builder.

```sh
cmake -S my-game -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-16 \
    -DCMAKE_PREFIX_PATH="$(zpkg prefix)"
cmake --build build
```

Use your compiler's actual executable path if it is not called `g++-16`.
Imported targets propagate C++26, reflection, `-fno-exceptions`, and `-fno-rtti`.
`Zahlen::Engine` refers to the prebuilt `libzahlen_engine.so`;
`Zahlen::Application` builds the application support sources shipped in the store.
Additional APIs require their own `find_package(Zahlen<Name> CONFIG REQUIRED)`
and `Zahlen::<Name>` link dependency.

## 4. Run

```sh
./build/my-game --help
./build/my-game
# Capture after ten completed frames, then exit:
./build/my-game --headless --frames 10 --capture frame.png
```

Application provides `--frames`, `--capture`, `--headless`, and `--fps-limit`
(not `--max-fps`). For the full current contract use `--help` on your game.

Installed store libraries carry runtime search paths, and CMake normally gives
build-tree executables an RPATH to their imported libraries. Do not set a global
`LD_LIBRARY_PATH` as the default setup. If a custom launch or deployment layout
cannot locate `libzahlen_engine.so`, configure its runtime search path or use a
scoped `LD_LIBRARY_PATH="$(zpkg prefix)/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"`
for diagnosis. Shipping a game outside the store needs a deliberate deployment
layout for libraries, plugins and assets; this build-tree example is not an installer.

### Without a display

The window backend is Wayland-only. For software Vulkan, point the loader at
your installed lavapipe ICD (the filename varies by distribution):

```sh
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
./build/my-game --headless
```

See [Vulkan driver and hardware](REQUIREMENTS.md#vulkan-driver-and-hardware)
for the device capability check.

## Do not use the engine-development workflow for a game

* Do not add this repository as a Git submodule or call
  `add_subdirectory(extern/zahlen)`. Use installed CMake packages.
* Do not run engine governance/build scripts to build your game.
* Do not treat `zpkg` as a game build tool. It manages packages; CMake builds
  your game. See the [command reference](../utils/zpkg/README.md#commands).

## License

[GPL-3.0-or-later](../LICENSE.md).
