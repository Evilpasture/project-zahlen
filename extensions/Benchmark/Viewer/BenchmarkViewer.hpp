// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/BenchmarkViewer.hpp
//
// The interactive half of the benchmark extension: a Clay panel over an
// Engine-hosted window, driving the same LogicBenchmark the headless target
// measures.
//
// The Engine is the host, not a participant. The benchmark's World is its own
// -- no scene, no camera, no render components, nothing the host's world can
// collide with -- so a game or sample that wants the numbers can hang this
// panel on its own frame without giving up its scene, its input or its HUD.
//
// Two entry points, two panels:
//
//   * UpdateAndRenderUI(Engine&, float) owns the UI frame -- BeginFrame, the
//     visible panels, EndFrame, SetPendingUIData. A host whose UI callback *is*
//     the benchmark calls it and nothing else. Run() drives it from a windowed
//     loop for the standalone tool.
//
//   * DrawPanels(GUI::Context&, Engine&) draws into a frame the host already
//     opened: two authors, one Clay frame. This is what a host with a HUD of
//     its own wants -- BSPMurderDronesSample draws its crosshair and these
//     panels between one pair of BeginFrame/EndFrame, because a second frame in
//     the same tick would clear the first one's geometry. DrawPanel and
//     DrawTelemetryPanel are the same thing one panel at a time.
//
// The panels themselves are two, because they measure two different things:
//
//   * the logic benchmark's panel (F3 in the app and the sample) times a World
//     stepped on a fixed timestep -- what the engine costs per tick;
//   * the telemetry panel (F4) times the host's own frames and reads the
//     engine's profiler -- average FPS, the 1% / 0.1% lows, and the GPU stage
//     timings the renderer already records and nobody was reading.
//
// Either way the benchmark advances at its own fixed timestep (see kFixedStep)
// rather than at the host's frame rate, so the numbers do not move when the
// host's dt does, and both panels start hidden: a host shows them.
//
// Lifetime: an attached viewer owns a World, and a World's teardown reaches the
// TaskSystem (PhysicsContext's destructor waits on its job counter before it
// releases anything). Keep the viewer in the same scope as the Engine it was
// attached to -- inside the host's TaskSystem::Scope, which is where the
// engine's own World lives out its life. Run() releases the benchmark itself
// before it returns, so a viewer the host keeps afterwards holds no World;
// BSPMurderDronesSample keeps its panel as a local of main() for the same
// reason, and the app's overlay is owned by the engine's UI callback.
#pragma once

#include <Benchmark/Benchmark.hpp>
#include <Benchmark/Viewer/FrameTelemetry.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace ZHLN::Bench {

class ZHLN_STATIC_API BenchmarkViewer {
  public:
    // The simulated step, and the cap on how many of them one frame may run.
    // Eight substeps is a frame budget of 133 ms of simulation: past that the
    // accumulator is dropped rather than carried, because a panel that is
    // measuring the engine must not become the thing that stalls the host.
    static constexpr float kFixedStep        = 1.0f / 60.0f;
    static constexpr int   kMaxTicksPerFrame = 8;

    BenchmarkViewer();
    ~BenchmarkViewer();

    BenchmarkViewer(const BenchmarkViewer&)                    = delete;
    auto operator=(const BenchmarkViewer&) -> BenchmarkViewer& = delete;

    // The standalone tool: builds an Engine with the CLI defaults (windowed,
    // vsync on, validation on), makes this viewer its UI, and runs until the
    // window closes. argv is deliberately not consulted -- a host that wants
    // --headless or --vsync parsed creates its own Engine and calls Attach(),
    // which is what the samples do with every other extension. The benchmark is
    // released on the way out: see the lifetime note above.
    auto Run(size_t initialEntities = 250) -> std::expected<void, ErrorCode>;

    // Binds the panel to an Engine the caller owns: the benchmark's World is
    // created here, and the Engine is asked for exactly one thing -- whether
    // there is a window to draw into. A headless host keeps the metrics and
    // loses the overlay.
    void Attach(Engine& engine, size_t initialEntities = 250);

    // The whole UI frame: BeginFrame(dt), the visible panels, EndFrame and
    // Engine::SetPendingUIData. Attaches on first use if the host did not call
    // Attach itself, so Run() and a one-callback host need no other setup.
    // Draws nothing while both panels are hidden -- but still advances the
    // benchmark, since a hidden panel is not a paused one.
    void UpdateAndRenderUI(Engine& engine, float dt);

    // The benchmark panel inside a frame the host owns, for a host with a HUD
    // of its own: call it on the active GUI::Frame token between BeginFrame and
    // std::move(frame).End(), and let it be one of the frame's authors.
    void DrawPanel(GUI::Frame& ui, Engine& engine);

    // The same, for the telemetry panel: FPS and frame-time lows for the host's
    // own frames, then every GPU stage and CPU scope the engine's profiler
    // holds, on the right-hand side of the window.
    void DrawTelemetryPanel(GUI::Frame& ui, Engine& engine);

    // Both panels, in one call, for a host that shows either -- what the sample
    // uses. Whichever is hidden is skipped; the frame is still the host's.
    void DrawPanels(GUI::Frame& ui, Engine& engine);

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
    void Tick(Engine& engine);

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
    // Null until an Attach() or the first UpdateAndRenderUI/DrawPanel call
    // succeeded.
    [[nodiscard]] auto GetBenchmark() noexcept -> LogicBenchmark*;
    [[nodiscard]] auto GetBenchmark() const noexcept -> const LogicBenchmark*;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ZHLN::Bench
