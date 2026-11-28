# From an empty host to a first scene

This tutorial continues from the host in the [consumer setup guide](CONSUMING.md).
It adds game logic as an application layer.

## 1. Start from the sample

Copy [`samples/FirstGameSample.cpp`](../samples/FirstGameSample.cpp) from the
release tag matching your installed engine to your game directory as `main.cpp`.
The sample positions the camera, creates a floor and a falling box, and closes
on Escape.

Rebuild and run:

```sh
cmake --build build
./build/my-game
```

You should see a blue box fall onto a gray plane under a lit sky. Escape or
closing the window exits. To inspect a frame without a window, run
`./build/my-game --headless --frames 120 --capture frame.png`; the PNG is
written relative to the working directory. Headless still needs a suitable
Vulkan driver; see [requirements](REQUIREMENTS.md). For available options run
`./build/my-game --help`.

## 2. Understand where your code goes

`App` creates and owns the engine and runs the loop. `AddLayer(Game{})` stores a
copy/move of your layer; its fields persist across frames. `Start` is called once
after engine creation. `Tick` is called after events are pumped and before the
simulation/render frame; `dt` is in seconds. Put per-frame game logic there,
not in a new loop around `Main`. You can also implement `BeforeRender`,
`AfterTick`, and `Stop` when needed. Layers start and tick in registration order
and stop in reverse order while the engine still exists. See
[App lifecycle and errors](COMApp.md#layers-and-dynamic-comiapplayer-plugins)
for supported signatures and error returns.

The separate capability headers are needed where their methods are called:
`IEntities` manages entities and components, `IAssetLibrary` spawns assets,
`IGraphicsDevice` controls graphics settings, and `IActionContext` reads input.
`IEngine` exposes these capabilities; they are not standalone globals. Do not
keep a borrowed engine/capability pointer beyond the app's lifetime. If a layer
owns resources needing explicit release, release them in `Stop`.

## 3. Make it yours

* **Change the scene:** `SpawnBox` takes *half* extents; its spawn parameters set
  position, color and physics. With `createPhysics = true`, a static floor and
  dynamic box interact through the simulation. The convenience spawn methods
  return `Entity::Null()` on failure; the lower-level `SpawnPrimitive` returns
  a result with an error. Do not directly animate a dynamic physics body's
  transform each frame; use the simulation capability for physics-driven motion.
* **Keep game state:** add fields to `Game`, such as elapsed time or an entity
  handle, set them in `Start`, and use them in `Tick`. Before using a retained
  handle, check `engine.Entities()->IsAlive(handle)`. Use `Get<T>`, `Patch<T>`,
  `Attach<T>` and `Despawn` through `IEntities`; check pointer/bool results.
  Consumer-defined components must be standard-layout and trivially copyable;
  they are data, not automatically running systems. Engine component definitions
  are in `Zahlen/ECS/Components.hpp`.
* **Read input:** `KeyPressed` is an edge, while `KeyDown` tests held state.
  Both take a player seat; the example uses `kPrimarySeat`. For configurable
  bindings, see `IActionContext.hpp` and
  [runtime controls](COMRuntimeControls.md). Headless capture is not an
  interactive input test.
* **Try a movable camera:** append `.TryLoadPlugin("freecam")` before `.Main`.
  It is optional and silently skipped if absent; when installed, the sandbox
  uses WASD, right mouse look and Shift sprint. See [plugin loading](COMApp.md#layers-and-dynamic-comiapplayer-plugins)
  for search paths and required plugins.
* **Load assets:** for glTF, see [glTF CPU documents and capability authoring](COMGLTFAssets.md),
  including its upload and texture ownership rules. Optional modules need their
  own installed packages and CMake dependencies.

For a larger working example with camera, physics, input and audio, read
[`app/main.cpp`](../app/main.cpp). The [documentation index](README.md) lists
other capability guides. For setup, runtime library lookup and deployment
caveats return to [Consuming Zahlen](CONSUMING.md).
