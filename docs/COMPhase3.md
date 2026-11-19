# COM Phase 3: Consumer-Side Host & Sample Surface

`app/cube.cpp` spawns a rotating lit cube via the unified `ZHLN::App`
(`<Zahlen/App.hpp>`) and `COM::IAssetLibrary`, with built-in CLI flag parsing,
`--frames`, `--capture <path.png>`, and repeatable `--plugin <name|path>`
loading. `app/main.cpp` runs the physics sandbox over the same `ZHLN::App`
surface and loads `libzahlen_plugin_freecam` opportunistically via
`TryLoadPlugin("freecam")` with zero `#if` guards. `app/UIEditor.cpp` runs on
`ZHLN::App` as well, linking `zahlen_ui` explicitly and owning its own
`ZHLN::HostUI` instance; CMake skips `zahlen_ui_editor` cleanly when
`ZHLN_BUILD_EXTRAS=OFF`.

Neither `libzahlen_engine.so` nor `zahlen_application` links Clay or
`zahlen_ui`: only POD submission structs (`<Zahlen/GUI/UIData.hpp>`,
`<Zahlen/GUI/Font.hpp>`) and `IEngine::SubmitUI` remain in core, while Clay,
`GUI::Context`, `TextBuffer`, `DefaultFont.zfont`, and `HostUI` live in
`extensions/UI/`.
