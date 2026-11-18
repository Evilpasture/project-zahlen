# COM host cut

## Production host ownership

`Application::RunConfigured` now owns a `COM::Ref<COM::IEngine>`, acquired only
through `CreateEngineRef`. ConfigCli, process commands, task/signal lifetime,
frame pacing and AddLayer remain in app/. Every production Application hook
uses `COM::IEngine&`, not the concrete Engine or a native compatibility adapter.
The default `zahlen` sandbox and `zahlen_ui_editor` use capabilities too.
The application umbrella `Zahlen/Zahlen.hpp` no longer exports concrete Engine,
registry, renderer, window, native module or prefab-authoring interfaces.

Application owns `HostUI`, which owns GUI::Context, the CPU baked font and its
FontAtlas/texture. It clears prior per-window submissions/capture at the start
of the host frame and invokes the UI phase explicitly before Tick. The engine
only copies and renders supplied geometry. HostUI has no SetCallback/Update
callback registry, and UIEditor has no SetWindowDrawCallback, WindowRenderView,
FrameScope or GPU-frame drawing callback. Its primary and preview contexts are
composed in the host UI phase and submitted by WindowID.

Layer hooks may be:

```cpp
void Start(COM::IEngine&, HostUI&); // or Start(COM::IEngine&)
void Tick(COM::IEngine&, float);
void UI(COM::IEngine&, HostUI&);    // or UI(COM::IEngine&)
void Stop(COM::IEngine&);
```

HostUI borrows the retained engine; a layer must release its HostUI borrow by
Stop. Stop runs in reverse layer order while UI and engine resources are alive.
HostUI releases its current-device atlas texture before Application releases
the engine. Device generation changes invalidate the old texture handle and
cause re-upload from retained CPU data, without an engine-owned callback.

UI hooks and Run callbacks may return void or `std::expected<void, ErrorCode>`. A typed host
failure or Tick failure stops layers and preserves the actual error. CLI early
exits still occur before process/device setup and preparation callbacks, and
host-injected UI input still copies caller spans.

UIEditor accepts `--frames N` for finite headless host tests; zero remains
interactive. This follows RequestClose's normal next-event-pass semantics.
Native window/preview interaction still requires a desktop integration test.

## Narrow boundary additions

The existing capabilities cover scene interaction, drawing and assets. The
remaining host-specific reads now cross the boundary explicitly:

- `IEngine::ReadHostInput`: reflected fixed-layout HostInputInfo, including
  per-window physical mouse/buttons/key state and bounded queued key/text
  arrays. Successful consume clears only that window's queued UI input.
  Unknown windows, null chains and invalid chains fail before consuming input.
  It exposes no router, injection, frame evaluation or backend pointer.
- `IGraphicsDevice::PacedDeltaTime`: fixed-layout optional presentation timing.
- `IState::TraceDiagnostics`: delegates crash diagnostics through a capability;
  ProcessScope no longer accepts a concrete Engine pointer.
- `DiagnosticsInfo`: copied crash-dump path as an independent factory
  chain slice. Effective EngineConfig publication is internal to the factory,
  not a service-container escape available to host code.

IEntities gets consumer-side typed convenience templates for singleton lookup,
component mutation and entity creation. They add no vtable entries or registry
access. Core/COM protocol files and Engine's public method set are unchanged.
Rebuild all consumers because the host-read capability vtables have changed.

## Fonts

Fonts has no Engine, RenderContext or AssetManager dependency and no installed
loader/user-pointer/teardown hook. `LoadBakedFont` explicitly reads a supplied
IAssetLibrary's VFS (then disk if allowed), preserves cooked-container versus
fontbm-pair preference, decodes bytes and returns caller-owned CPU data.
`UploadFontAtlas` explicitly uploads through IGraphicsDevice::CreateTexture.
The caller owns and unloads the returned texture. UIEditor supplies CPU font
bytes to its Application-owned compositor; no global loader is installed.

## Deliberately not represented as migrated

This is a host cut, not completion of the full migration roadmap:

- glTF/BSP, scene orchestration, the world editor, diagnostic scene host,
  gameplay modules, samples, and animation/physics/VFX extensions have all been
  migrated to COM, and the transitional `extensions/NativeHostUI` target has
  been deleted.
- Native scene and ProjectLight module diagnostics drive explicit standalone
  native loops until their Scene/Character/Camera/ProjectLight dependencies are
  ported. They do not route native Engine callbacks through Application.
- Native frame-graph/module tests use a test-only NativeEngineHost fixture.
  Those tests are not evidence for the production Application API; the new
  Application/HostUI contracts are exercised through COM in Extras_Application.
- There is no KernelApp implementation/header in this checkout; old architecture
  prose naming it is historical, not an additional runner silently left native.

The next cuts are glTF/BSP authoring, consumer-side Scene extraction and
instantiation, then host-driven extension layers. CharacterController and
Animation will use the agreed Option B initially (core-owned, data-driven
subsystems driven through published POD components). No Registry escape or
bulk mapping API is added without profiling evidence.
