# Consumer-side COM App

`app/include/Zahlen/COM/App.hpp` provides a header-only `ZHLN::COM::App`. Link the
engine shared library for `ZHLN::COM::CreateEngine(const ChainInNode*)`; do not
link the in-tree application or host-runner libraries. Configuration slices
(`EngineAppConfig`, `EngineWindowConfig`, `EngineRenderConfig`, `PhysicsConfig`)
are forwarded to `CreateEngineRef(COM::InChain(...))`. `Run` returns any factory
or tick error as `ErrorCode`.

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
        .DefaultScene(true)
        .OnStart([](ZHLN::COM::IEngine& engine) {
            (void)engine.Assets()->SpawnBox({0.5f, 0.5f, 0.5f});
            engine.RequestClose();
        })
        .Run() ? 0 : 1;
}
```

Include the relevant capability headers (`<Zahlen/COM/IGraphicsDevice.hpp>`,
`<Zahlen/COM/IEntities.hpp>`, `<Zahlen/COM/ISimulation.hpp>`,
`<Zahlen/COM/IAudioDevice.hpp>`, `<Zahlen/COM/IAssetLibrary.hpp>`,
`<Zahlen/COM/IActionContext.hpp>`) separately to call methods on
`engine.Graphics()`, `engine.Entities()`, `engine.Simulation()`,
`engine.Audio()`, `engine.Assets()`, and `engine.Actions()`. They are forward
declared by `IEngine`. `RequestClose()` takes effect on the next
`ProcessEvents()`, so a close requested during an update may still be followed
by that frame's `Tick`. Layers start and tick in
registration order and stop in reverse order, before engine release, including
on tick failure. Do not mutate the app from its callbacks. Copies of an `App`
share layer state. The frame cap is client-side and uses a steady clock; zero
disables it. Frame deltas delivered to layers, the update callback, and the
engine are clamped to 0.25 seconds. Pacing uses `std::this_thread::sleep_for`
and is subject to OS scheduler granularity; on Windows, hosts requiring finer
pacing may manage timer resolution themselves.

## Direct Factory & Chain Usage

Callers managing their own main loop can invoke `CreateEngineRef` directly with
any subset of configuration slices via `ZHLN::COM::InChain(...)`:

```cpp
#include <Zahlen/COM/EngineFactory.hpp>

auto engine = ZHLN::COM::CreateEngineRef(ZHLN::COM::InChain(
    ZHLN::COM::EngineAppConfig{.appName = ZHLN::String64("CustomHost"), .initializeDefaultScene = true},
    ZHLN::COM::EngineWindowConfig{.width = 1920, .height = 1080, .vsync = true}
));
```

**Current SDK notes:** the COM headers are Jolt-free (using `ZHLN::FVec*`,
`ZHLN::DVec*`, `ZHLN::Quat`, and `ZHLN::Mat4x4`), and rely on
`Zahlen/Core/Reflection/*`, which requires C++26 static reflection
(`-freflection`) on a compatible Itanium C++ ABI toolchain.
