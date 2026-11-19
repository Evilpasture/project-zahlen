// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// COM benchmark/telemetry panels. The host owns its GUI context, font atlas,
// input consumption and UI submission. This library installs no callbacks.
#pragma once

#include <Benchmark/Benchmark.hpp>
#include <Benchmark/Viewer/FrameTelemetry.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Common.h>
#include <UI/GUI.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace ZHLN::Bench {

class ZHLN_STATIC_API Viewer {
  public:
    // The simulated step, and the cap on how many of them one frame may run.
    // Eight substeps is a frame budget of 133 ms of simulation: past that the
    // accumulator is dropped rather than carried, because a panel that is
    // measuring the engine must not become the thing that stalls the host.
    static constexpr float kFixedStep        = 1.0f / 60.0f;
    static constexpr int   kMaxTicksPerFrame = 8;

    Viewer();
    ~Viewer();

    Viewer(const Viewer&)                    = delete;
    auto operator=(const Viewer&) -> Viewer& = delete;

    // Binds the panel to an Engine the caller owns: the benchmark's World is
    // created here, and the Engine is asked for exactly one thing -- whether
    // there is a window to draw into. A headless host keeps the metrics and
    // loses the overlay.
    void Attach(COM::IEngine& engine, size_t initialEntities = 250);

    // The benchmark panel inside a frame the host owns, for a host with a HUD
    // of its own: call it on the active GUI::Frame token between BeginFrame and
    // std::move(frame).End(), and let it be one of the frame's authors.
    void DrawPanel(GUI::Frame& ui, COM::IEngine& engine);

    // The same, for the telemetry panel: FPS and frame-time lows for the host's
    // own frames, then every GPU stage and CPU scope the engine's profiler
    // holds, on the right-hand side of the window.
    void DrawTelemetryPanel(GUI::Frame& ui, COM::IEngine& engine);

    // Both panels, in one call, for a host that shows either -- what the sample
    // uses. Whichever is hidden is skipped; the frame is still the host's.
    void DrawPanels(GUI::Frame& ui, COM::IEngine& engine);
    // Borrowed until replaced or the viewer is destroyed. Caller keeps the
    // atlas alive across drawing calls. Null uses
    // conservative text metrics (drawing still uses the host frame's font).
    void SetFontAtlas(const FontAtlas* atlas) noexcept;

    // One frame's bookkeeping and no drawing: samples the frame interval, reads
    // the engine's profiler, and advances the benchmark while its panel is up.
    // The three drawing entry points call it themselves; a host calls it to keep
    // the telemetry window warm while the panels are hidden -- otherwise the
    // first second after F4 shows a window that starts on that frame.
    //
    // Idempotent per engine frame (the frame counter is the tick's identity), so
    // calling it beside a drawing entry point in the same frame is not a double
    // count, and it is cheap when nothing is shown: a clock read, one pass over
    // the profiler's metrics, and no benchmark world -- the viewer attaches
    // lazily in the drawing entry points, and stepping happens only while the
    // benchmark panel is visible.
    void Tick(COM::IEngine& engine);

    // The telemetry half: the frame window, the profiler rows and the capture,
    // for a host that wants the numbers without the panel -- a logger, a
    // regression harness, or a screenshot tool's footer.
    [[nodiscard]] auto GetFrameTelemetry() noexcept -> FrameTelemetry&;
    [[nodiscard]] auto GetFrameTelemetry() const noexcept -> const FrameTelemetry&;

    // Visibility and run state, so a host can bind its own key to them. The
    // panels start hidden: a host shows them (SetVisible/SetTelemetryVisible/
    // ToggleVisible), which keeps an app's F3/F4 out of the viewer's business
    // and the viewer's UI out of an app that never asked for it.
    void               SetVisible(bool visible) noexcept;
    [[nodiscard]] auto IsVisible() const noexcept -> bool;
    void               ToggleVisible() noexcept;

    void               SetTelemetryVisible(bool visible) noexcept;
    [[nodiscard]] auto IsTelemetryVisible() const noexcept -> bool;
    void               ToggleTelemetryVisible() noexcept;

    // Running as soon as it is attached, which is the state the panel's own
    // checkbox shows; this is the same switch, for a host that binds a key.
    void               SetRunning(bool running) noexcept;
    [[nodiscard]] auto IsRunning() const noexcept -> bool;

    // The headless half, for a host that wants the metrics without the panel.
    // Null until an Attach() or the first DrawPanel call
    // succeeded.
    [[nodiscard]] auto GetBenchmark() noexcept -> LogicBenchmark*;
    [[nodiscard]] auto GetBenchmark() const noexcept -> const LogicBenchmark*;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ZHLN::Bench
