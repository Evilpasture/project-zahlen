# Unified `ZHLN::App` & COM Plugin ABI

Include `<Zahlen/App.hpp>`, find `ZahlenApplication`, and link
`Zahlen::Application`. `ZHLN::App` configures and runs the application host.

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
- `--frames <N>` — stop after `N` completed frames (`0`: unlimited)
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
`ZHLN::COM::IAppLayer` (and optionally additional COM capability interfaces via
`COM::Implements<Impl, COM::IAppLayer, ISecondary...>`) and define the ordinary,
by-value `CreateAppLayerRaw` factory declared in that header. In the same
source file, include `<Zahlen/Core/COM/Contract.hpp>` and export the factory's
witness:

```cpp
template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
```

Plugins must export a matching ABI witness and ship a companion
`zahlen_plugin_<name>.manifest.toml` declaring their name, library stem, API
version (`api = 1`), dependency edges (`requires` / `optional_requires`), and
hot-reload policy. A missing or incompatible witness fails with
`AppError::PluginAbiMismatch`; rebuild the plugin against the installed
toolchain.

- `app.AddPlugin(COM::Ref<COM::IAppLayer>)` attaches an in-process COM layer
  (including a layer opened directly with `SharedLibrary::OpenPlugin` and
  cast to a secondary COM interface).
- `app.LoadPlugin("freecam", COM::InChain(FreeCam::Component{...}))` loads a
  required plugin (`libzahlen_plugin_<name>.so`/`.dylib`/`.dll` or an explicit
  path), resolving required manifest dependencies (for example, `"animation"`
  automatically pulls in `"character_controller"` before itself) and failing
  with `AppError::PluginNotFound` / `PluginLoadFailed` / `PluginSymbolMissing`
  if unavailable.
- `app.LoadPlugin("freecam", nullptr, /*required=*/false)` loads the plugin
  when found (`ZHLN_PLUGIN_PATH`, next to the executable, `../lib`, or
  `<engine-lib>/zahlen/plugins`) and skips it if the library file is absent.
- Runtime COM plugins built with `zahlen_add_plugin`:
  - **Lifecycle layers (Tier 1, installed with toolchain packages):**
    `zahlen_plugin_freecam` (`FreeCam::Component`),
    `zahlen_plugin_screenshot` (`Screenshot::PluginConfig`),
    `zahlen_plugin_character_controller`, and
    `zahlen_plugin_animation` (`requires = ["character_controller"]`).
  - **Lifecycle layers with secondary COM interfaces (Tier 2a, installed with
    toolchain packages):**
    `zahlen_plugin_terrain` (`Terrain::ITerrainLayer` via `<Terrain/ITerrainLayer.hpp>`)
    and `zahlen_plugin_vfx` (`VFX::IVfxLayer` via `<VFX/IVfxLayer.hpp>`).
  - **Heavy-dependency isolator capability plugins (Tier 2b):**
    `zahlen_plugin_gltf` (`COM::IAssetImporter`, installed in `ZahlenGLTF`),
    `zahlen_plugin_bsp` (`COM::IAssetImporter`),
    `zahlen_plugin_svg` (`SVG::ISvgRasterizer`, isolates `resvg`),
    `zahlen_plugin_http` (`HTTP::IHttpClient`, isolates `libcurl`),
    `zahlen_plugin_discord_rpc` (`Discord::IPresenceClient`), and
    `zahlen_plugin_interaction` (`COM::IAppLayer`).
- Shutdown runs `Stop` / `OnStop` in reverse registration order while `IEngine`
  is still alive, destroys all layer objects (releasing `Ref<IAppLayer>`), drops
  `IEngine`, and only then closes plugin `SharedLibrary` handles.

## Optional immediate-mode UI

Applications and tools that render immediate-mode Clay panels find `ZahlenUI`
and link `Zahlen::UI`, include `<UI/HostUI.hpp>`, construct `HostUI(engine)` inside
their layer's `Start`, call `ui.BeginFrame()` + `ui.Submit(...)` in `Tick`, and
destroy `HostUI` in `Stop` before `IEngine` shuts down.
