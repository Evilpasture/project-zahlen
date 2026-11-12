# Consumer-side COM App

`app/include/Zahlen/COM/App.hpp` provides a header-only `ZHLN::COM::App`. Link the
engine shared library for `CreateEngineBuilder`; do not link the in-tree
application or host-runner libraries. Configuration is forwarded to the COM
builder. `Run` returns the factory, build or tick error as `ErrorCode`.

In a source checkout add `app/include` and `include` to the consumer include
paths. With toolchain packaging enabled, install the `ZahlenCOMApp` component
alongside `ZahlenEngine`; the wrapper does not require the `ZahlenApplication`
component or its static libraries.

```cpp
#include <Zahlen/COM/App.hpp>

int main() {
    return ZHLN::COM::App().Headless(true)
        .OnStart([](ZHLN::COM::IEngine& engine) { engine.RequestClose(); })
        .Run() ? 0 : 1;
}
```

Include the relevant capability headers separately to call methods on
`engine.Assets()`, `engine.Actions()`, etc. They are forward declared by
`IEngine`. `RequestClose()` takes effect on the next `ProcessEvents()`, so a
close requested during an update may still be followed by that frame's Tick.
Layers start and tick in registration order and stop in reverse order, before
engine release, including on tick failure. Do not mutate the app from its callbacks.
Copies of an App share layer state. The frame cap is client-side and uses a
steady clock; zero disables it. Frame deltas delivered to layers, the update
callback, and the engine are clamped to 0.25 seconds. Pacing uses
`std::this_thread::sleep_for` and is subject to OS scheduler granularity; on
Windows, hosts requiring finer pacing may manage timer resolution themselves.

**Current SDK limitations:** the existing COM headers transitively include
Jolt headers and `Zahlen/Core/Reflection/Core.hpp`, which unconditionally
requires C++26 static reflection (`-freflection`). Thus this wrapper removes
in-tree application-library coupling, but it does not by itself make the
existing COM SDK usable with arbitrary compilers, without Jolt headers, or
without reflection flags. The engine is still a runtime dependency, and the
existing C++ COM ABI has its documented platform/compiler ABI constraints.
