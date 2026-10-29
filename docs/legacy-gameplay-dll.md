# Legacy dynamic gameplay (deprecated)

The `GAMEPLAY_API` / `NativeGameplayUpdate` dynamic-library hook is retained
only for experiments and migration. It is **off by default**, even when an
application calls `App().Run()` without a callback. Normal games compile
C++ gameplay sources directly into their executable and use `App().Run(...)`.

To test the old loader, launch `zahlen-scene-host --legacy-gameplay-dll` (without
`--scene`) or explicitly set `EngineRunConfig::enableNativeGameplay = true`
with `App(config).Run(argc, argv)` / `Engine::Run(config)`. For a bespoke host
using `Engine::Create`, set `EngineConfig::enableNativeGameplay = true`.
Do not also supply an `App().Run(callback)` gameplay function; App rejects that
combination. `--driver=native` selects the C++ frame driver, **not** the DLL.

The adapter watches `scripts/gameplay.dll` on Windows, `scripts/libgameplay.so`
on Linux, or `scripts/libgameplay.dylib` on macOS. This example is intentionally
separate from the normal game entry point:

```cpp
#include <Zahlen/Engine.hpp>
#include <Zahlen/FramePlan.hpp>

#if defined(_WIN32)
#define GAMEPLAY_API extern "C" __declspec(dllexport)
#else
#define GAMEPLAY_API extern "C" [[gnu::visibility("default")]]
#endif

GAMEPLAY_API ZHLN::GameplayStatus NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (engine == nullptr) return ZHLN::GameplayStatus::Error;
    // Compatibility-only update; keep object ownership within one module.
    (void)dt;
    return ZHLN::GameplayStatus::OK;
}
```

No stable ABI is promised for owning allocations, vtables, ECS component
layouts, or reflection/type identities across this library boundary. The
adapter is not suitable for restricted dynamic-loading platforms. Prefer the
compiled `app/main.cpp` sandbox (or your own GameSession and ECS systems) and
iterate on TOML/Lua *data* using the scene tool instead of swapping code.
