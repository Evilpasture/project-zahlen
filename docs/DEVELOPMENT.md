# Developing Zahlen itself

Audience: engine contributors working in this repository. Game developers should
start with [Consuming Zahlen](CONSUMING.md), not these source-build steps.
These notes describe the current branch; release consumers must use docs for
their release tag.

## Build requirements

See [Requirements](REQUIREMENTS.md) for compiler and runtime requirements.
Source-build dependencies are listed in the [development container](../docker/README.md)
configuration.

## Build Instructions

For creative works, the build system expects Blender files in `./blender/` depending on your CMakeLists.txt.
Blender itself is optional: `cmake/BlenderProbe.cmake` resolves it from `BLENDER_PATH`, then `PATH`, then the
platform's own install locations, and reports the answer as `ZHLN_HAS_BLENDER`. Without it the cook graph
carries no `.blend` extraction edge at all -- textures, fonts, glTF and the pak still cook -- so a tree with
no `.blend` sources never needs Blender installed. Pass `-DZHLN_HAS_BLENDER=OFF` to skip them on a host that
has Blender, or `-DZHLN_HAS_BLENDER=ON` to make a missing Blender a configure error.
For compiled assets, the build system expects usually in resources/assets/ as an unofficial convention.

Run scripts/build.sh and fix every error you go along the way if you're missing dependencies.

### Configuring the build (TUI)

`ccmake` is not the only way to browse the options. The tree carries a
[Kconfiglib](../https://github.com/ulfalizer/Kconfiglib) menu for the `ZHLN_*`
switches, wired in as a preset:

```sh
pip install -r scripts/requirements-menuconfig.txt    # once; optional
./scripts/menuconfig.sh                                # menu, then configure
```

The script opens the menu and, when it exits,
configures the same build directory from the `.config` it wrote. Pass a
directory to work somewhere other than `build/menuconfig`, or `--no-configure`
to stop after writing the file.

An already-configured directory is reconfigured with whatever generator it
already has; a fresh one uses Ninja (override with `MENUCONFIG_GENERATOR`), which
is what the presets use. `cmake --preset menuconfig` is the same thing in preset
form.

Configuration precedence: explicit `-D` values, then edited `.config` values,
then CMake option defaults.

### Runtime data

See [runtime directories](../include/ARCHITECTURE.md#9-runtime-directories--distribution)
for data and cache paths, and [CDN caching](../extensions/net/CDN/README.md) for
remote-cache configuration.

## Architecture

The optional source tree is split by role: `plugins/` contains asset formats and codecs, `extensions/` contains reusable engine subsystems plus network/platform I/O, and `gameplay/` contains domain-specific systems and project integrations. The existing top-level `modules/` remains reserved for Core C++ module interfaces.

For a detailed breakdown of the engine's architecture, frame loop execution order and deferred render graph topology, see [include/ARCHITECTURE.md](../include/ARCHITECTURE.md).

## Tools

See the [host-program reference](../utils/README.md) for engine tools and the
[test guide](../tests/README.md) for running tests.

## Prebuilt entry-point development container

See [container usage](../docker/README.md) for image selection and local builds.

With Podman or Docker installed, from a checkout of this repository:

```sh
scripts/dev-container.sh init
# Edit .zahlen-dev/main.cpp (initially copied from the image).
scripts/dev-container.sh build
scripts/dev-container.sh run --headless
scripts/dev-container.sh test
```

`build` syncs only `.zahlen-dev/main.cpp`, not engine or CMake edits. `reset`
removes container changes but preserves that local file. Copy any other edits
out before resetting. The container uses host networking; run trusted code only.

### Capture a frame

The `zahlen` executable can capture the sandbox scene to a PNG and exit:

```sh
./build/bin/zahlen --headless --width 320 --height 180 \
  --capture frame.png --frames 10
```

For capture flag semantics, see [Application CLI flags](COMApp.md#built-in-cli-flags).
With the persistent container, capture to `/workspace/frame.png` and copy it out
with `podman cp zahlen-dev:/workspace/frame.png ./frame.png` (or `docker cp`).
