# Consuming Zahlen as a game developer

Audience: game developers using released Linux x86_64 packages. This page is the
canonical consumer setup guide for this revision; use the version at your release
tag when consuming an older release.

Install the released Linux x86_64 toolchain with `zpkg`. The Engine package
provides a prebuilt shared library; source packages such as Application are
compiled by CMake as part of your game.

## Prerequisites

See [Requirements](REQUIREMENTS.md) for build tools, runtime libraries,
windowing, and Vulkan driver and hardware support for this release.

## 1. Get zpkg

Download `zpkg-linux-x86_64` from the
[GitHub release](https://github.com/Evilpasture/project-zahlen/releases) you intend
to use, make it executable, and put it on `PATH`. The release supplies
`SHA256SUMS.tools` for the manager binary; verify it before execution.
Checksums detect corruption, but are not an independent source of publisher trust.

To build `zpkg` from source, use a C++23 compiler with simdjson and zlib;
OpenSSL enables signature verification, while zstd and libcurl enable additional
archive formats and HTTPS remotes. See the
[zpkg build reference](../utils/zpkg/README.md#building).

## 2. Install the engine

Official release archives are signed. `zpkg install` verifies their signatures
and archive checksums, then installs Application and its dependencies from the
latest release:

```sh
zpkg install
```

Key rotation releases are dual-signed so stores trusting either key can verify
the transition. See [Upgrading](UPGRADING.md) for the version lifecycle.

To select components or pin a release:

```sh
zpkg install application image
zpkg install application --version <release-tag>
```

The first installed toolchain becomes active. Later installs preserve the active
toolchain; `zpkg sync --activate` installs the same package set from a newer
release and switches it, or `zpkg use <id>` switches explicitly. Repeated installs
keep packages already present. `zpkg list` shows installed toolchains and
`zpkg prefix` prints the active prefix. For reproducible builds, use
`zpkg prefix <id>`. `zpkg --version` reports the manager's CalVer;
`zpkg list` and `zpkg info` report the installed engine toolchain version.

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

This creates a minimal host executable. The
[From an empty host to a first scene](FIRST_GAME.md) guide adds a layer, scene,
physics and input. Optional modules are separate packages: install each module
with `zpkg` and link its corresponding `Zahlen::<Name>` target.

```sh
cmake -S my-game -B build --toolchain="$(zpkg prefix)/share/zahlen/ZahlenToolchain.cmake"
cmake --build build
```

The installed toolchain file uses the system `g++` when it meets the GCC 16.1.1
floor, and falls back to `g++-16` when the default is older. Set
`CMAKE_CXX_COMPILER` or `CXX` to select another compiler; the package config
checks its ABI and reflection support. The toolchain file sets C++26 and
reflection, adds the active prefix to package discovery, and configures runtime
paths to the installed engine library. Imported targets propagate
`-fno-exceptions` and `-fno-rtti` where required.
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

Application supports `--frames`, `--capture`, `--headless`, and `--fps-limit`.
Run `./build/my-game --help` for the complete command-line options.

Installed store libraries carry runtime search paths, and CMake gives
build-tree executables an RPATH to the imported engine library. For a deployed
game, include the engine library, plugins and assets in the deployment layout
and configure the executable's install RPATH for that location. A scoped
`LD_LIBRARY_PATH="$(zpkg prefix)/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"`
can help diagnose library lookup issues.

## License

[GPL-3.0-or-later](../LICENSE.md).
