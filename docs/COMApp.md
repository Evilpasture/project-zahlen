# Unified `ZHLN::App` & COM Plugin ABI

`app/include/Zahlen/App.hpp` provides `ZHLN::App`, the single application host
and fluent builder over `libzahlen_engine.so` (`Zahlen::Engine`). Link
`zahlen_application` in-tree, or `find_package(ZahlenApplication CONFIG REQUIRED)`
and link `Zahlen::Application` in an installed toolchain consumer. Neither
`libzahlen_engine.so` nor `ZHLN::App` depends on Clay or `extensions/UI/`.

## Minimal Application

```cpp
#include <Zahlen/App.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>

int main(int argc, char** argv) {
    return ZHLN::App()
        .Title("My Game")
        .Resolution(1280, 720)
        .OnStart([](ZHLN::COM::IEngine& engine) {
            (void)engine.Assets()->SpawnBox({0.5f, 0.5f, 0.5f});
        })
        .Main(argc, argv);
}
```

Include capability headers (`<Zahlen/COM/IGraphicsDevice.hpp>`,
`<Zahlen/COM/IEntities.hpp>`, `<Zahlen/COM/ISimulation.hpp>`,
`<Zahlen/COM/IAudioDevice.hpp>`, `<Zahlen/COM/IAssetLibrary.hpp>`,
`<Zahlen/COM/IActionContext.hpp>`, `<Zahlen/COM/IState.hpp>`) separately for the
facades a translation unit calls.

## Built-in CLI Flags

`App::Run(argv)` and `App::Main(argc, argv)` bind every scalar field of
`EngineConfig` (plus any caller `.WithConfig(cfg)` or `.AddFlag(...)` bindings)
and provide:

- `--help` / `-h`, `--version`, `--print-graph [none|fxaa|smaa]`, `--debug-attach`
- `--frames <N>` — stop cleanly after `N` completed frames (`0`: unlimited)
- `--capture <path.png>` — read back the primary window and write a PNG after
  `--frames` completed frames (default `1` when `--frames` is omitted), then exit
- `--plugin <name|path>` — dynamically load a `COM::IAppLayer` shared library
  (repeatable; preserves registration order)

Programmatic `app.Run()` / `app.Run(update)` bypass CLI parsing and run with the
builder's configured `EngineConfig`, `MaxFrames`, `Capture`, layers, and plugins.

## Layers and Dynamic `COM::IAppLayer` Plugins

C++ layers registered with `.AddLayer(layer)` may implement any subset of
`Start(IEngine&)`, `Tick(IEngine&, float dt)`, `BeforeRender(IEngine&, float dt)`,
`AfterTick(IEngine&)` (or `AfterTick(IEngine&, float dt)`), and `Stop(IEngine&)`,
returning `void` or `std::expected<void, ErrorCode>`.

Dynamically loaded plugins (`<Zahlen/COM/IPlugin.hpp>`) implement
`ZHLN::COM::IAppLayer` and export a single COM-audited C++ factory resolved via
`COM::MangleFunction<&COM::CreateAppLayer>()` (`COM::kCreateAppLayerSymbol`
/ `COM::ResolveSymbol<&COM::CreateAppLayer>(library)`):

```cpp
namespace ZHLN::COM {
Result<IAppLayer*> CreateAppLayer(const ChainInNode* config) noexcept;
}
```

- `app.AddPlugin(COM::Ref<COM::IAppLayer>)` attaches an in-process COM layer.
- `app.LoadPlugin("freecam", COM::InChain(FreeCam::Component{...}))` loads a
  required plugin (`libzahlen_plugin_<name>.so`/`.dylib`/`.dll` or an explicit
  path), failing with `AppError::PluginNotFound` / `PluginLoadFailed` /
  `PluginSymbolMissing` if unavailable.
- `app.TryLoadPlugin("freecam")` loads the plugin when found (`ZHLN_PLUGIN_PATH`,
  next to the executable, `../lib`, or `<engine-lib>/zahlen/plugins`) and skips
  it cleanly if the library file is absent.
- Shipped plugins: `zahlen_plugin_freecam` (`FreeCam::Component` config chain)
  and `zahlen_plugin_screenshot` (`Screenshot::PluginConfig` config chain).
- Shutdown runs `Stop` / `OnStop` in reverse registration order while `IEngine`
  is still alive, destroys all layer objects (releasing `Ref<IAppLayer>`), drops
  `IEngine`, and only then closes plugin `SharedLibrary` handles.

## Optional Immediate-Mode UI (`extensions/UI/`)

Applications and tools that render immediate-mode Clay panels link `zahlen_ui`
(`Zahlen::UI`), include `<UI/HostUI.hpp>`, construct `HostUI(engine)` inside
their layer's `Start`, call `ui.BeginFrame()` + `ui.Submit(...)` in `Tick`, and
destroy `HostUI` in `Stop` before `IEngine` shuts down.
