# Consumer-side COM App

`app/include/Zahlen/COM/App.hpp` provides a header-only `ZHLN::COM::App`. Link the
engine shared library for `ZHLN::COM::CreateEngine(const ChainInNode*)`; do not
link the in-tree application or host-runner libraries. Configuration slices
(`AppConfig`, `WindowConfig`, `RenderConfig`, `PhysicsConfig`)
are forwarded to `CreateEngineRef(COM::InChain(...))`. `Run` returns any factory
or tick error as `ErrorCode`. `App.hpp` has no dependency on `ConfigCli` or
`Screenshot`.

In a source checkout add `app/include` and `include` to the consumer include
paths. With toolchain packaging enabled, install the `ZahlenCOMApp` component
alongside `ZahlenEngine`; the wrapper does not require the `ZahlenApplication`
component or its static libraries.

```cpp
#include <Zahlen/COM/App.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>

int main() {
    return ZHLN::COM::App()
        .Title("My COM Game")
        .Headless(true)
        .MaxFrames(1)
        .OnStart([](ZHLN::COM::IEngine& engine) {
            engine.Assets()->SpawnBox({0.5f, 0.5f, 0.5f});
        })
        .Run() ? 0 : 1;
}
```

Include the relevant capability headers (`<Zahlen/COM/IGraphicsDevice.hpp>`,
`<Zahlen/COM/IEntities.hpp>`, `<Zahlen/COM/ISimulation.hpp>`,
`<Zahlen/COM/IAudioDevice.hpp>`, `<Zahlen/COM/IAssetLibrary.hpp>`,
`<Zahlen/COM/IActionContext.hpp>`, `<Zahlen/COM/IState.hpp>`) separately to call
methods on `engine.Graphics()`, `engine.Entities()`, `engine.Simulation()`,
`engine.Audio()`, `engine.Assets()`, `engine.Actions()`, and `engine.State()`.
They are forward declared by `IEngine`.

Layers may implement any subset of `Start(IEngine&)`, `Tick(IEngine&, float dt)`,
`AfterTick(IEngine&)` (or `AfterTick(IEngine&, float dt)`), and `Stop(IEngine&)`.
Layers start, tick, and run `AfterTick` in registration order; `App::AfterTick`
runs immediately after layer `AfterTick` hooks, with `engine.CurrentFrame()`
already reflecting the completed tick. `MaxFrames(n)` stops cleanly after `n`
completed frames (zero leaves the run unbounded). Layers stop in reverse order
before engine release, including on tick failure. Do not mutate the app from its
callbacks. Copies of an `App` share layer state. The frame cap is client-side
and uses a steady clock; zero disables it. Frame deltas delivered to layers, the
update callback, and the engine are clamped to 0.25 seconds. Pacing uses
`std::this_thread::sleep_for` and is subject to OS scheduler granularity; on
Windows, hosts requiring finer pacing may manage timer resolution themselves.

## Optional CLI & Screenshot Host Harness

Standalone COM executables that also link `zahlen_command_line` and
`zahlen_screenshot` can wrap `COM::App` with `ZHLN::COM::RunApp` from
`app/include/Zahlen/COM/Host.hpp` (still without linking `Application`,
`HostRunner`, or `HostUI`). It accepts `std::span<char* const>`, binds
`--width`, `--height`, `--headless`, `--frames`, and `--capture` via
`ConfigCli`, attaches `ZHLN::Screenshot::CaptureLayer` when `--capture` is set,
and returns `EXIT_SUCCESS` or `EXIT_FAILURE`:

```cpp
#include <Zahlen/COM/App.hpp>
#include <Zahlen/COM/Host.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <cstddef>
#include <span>

int main(int argc, char* argv[]) {
    return ZHLN::COM::RunApp(std::span(argv, static_cast<size_t>(argc)),
        ZHLN::COM::App()
            .Title("Zahlen COM Cube")
            .OnStart([](ZHLN::COM::IEngine& e) { e.Assets()->SpawnBox({1, 1, 1}); }));
}
```

## Direct Factory & Chain Usage

Callers managing their own main loop can invoke `CreateEngineRef` directly with
any subset of configuration slices via `ZHLN::COM::InChain(...)`:

```cpp
#include <Zahlen/COM/EngineFactory.hpp>

auto engine = ZHLN::COM::CreateEngineRef(ZHLN::COM::InChain(
    ZHLN::COM::AppConfig{.appName = ZHLN::String64("CustomHost")},
    ZHLN::COM::WindowConfig{.width = 1920, .height = 1080, .vsync = true}
));
```

**Current SDK notes:** the COM headers are Jolt-free (using `ZHLN::FVec*`,
`ZHLN::DVec*`, `ZHLN::Quat`, and `ZHLN::Mat4x4`), and rely on
`Zahlen/Core/Reflection/*`, which requires C++26 static reflection
(`-freflection`) on a compatible Itanium C++ ABI toolchain.
