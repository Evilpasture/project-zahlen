// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/BenchmarkViewer.cpp
#include <Benchmark/Viewer/BenchmarkViewer.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::Bench {

namespace {

// Panel geometry, in pixels. The width is fixed and the height fits its
// content; every offset below is relative to the screen's top-left corner,
// which is the only anchor Clay offers a floating box (see BeginBox).
inline constexpr float  kPanelMargin  = 16.0f;
inline constexpr float  kPanelWidth   = 320.0f;
inline constexpr float  kPanelPadding = 10.0f;
inline constexpr float  kChartHeight  = 56.0f;
inline constexpr float  kBarGap       = 1.0f;
inline constexpr size_t kChartBars    = 64;

// The widest the panel's content can be, which is what the chart and every
// text line inside it are laid out against.
inline constexpr float kPanelContentWidth = kPanelWidth - (kPanelPadding * 2.0f);

// The telemetry panel carries a table -- two columns of stage rows, each a
// name, the last measurement and a sparkline -- so its width is not a constant:
// it is what the columns add up to, measured against the font the engine is
// actually drawing with (see BuildTableStyles). The geometry below is what one
// row is made of.
inline constexpr float kTelemetryChartHeight = 44.0f;
inline constexpr float kColumnGap            = 10.0f;
inline constexpr float kTableFontSize        = 11.0f;
inline constexpr float kTableValueWidth      = 42.0f;
inline constexpr float kTableSparkWidth      = 34.0f;
inline constexpr float kTableGap             = 4.0f;
inline constexpr float kTableNameMin         = 70.0f;
inline constexpr float kTableNameMax         = 150.0f;
inline constexpr float kStageSparkHeight     = 12.0f;
inline constexpr float kSparkBarGap          = 1.0f;

// The telemetry panel's widest case, used before the first frame has measured
// one: two columns of the widest name column, plus its padding.
inline constexpr float kTelemetryWidthDefault = ((kTableNameMax + kTableValueWidth + kTableSparkWidth + (kTableGap * 2.0f)) * 2.0f) + kColumnGap +
                                                (kPanelPadding * 2.0f);

// Per-character advance assumed when the engine has no font asset and is drawing
// with its embedded fallback. That fallback is 18/32 = 0.5625 em per glyph, and
// JetBrains Mono -- the font this tree vendors -- is 0.6 em, which is also what
// the panel was measured against on screen: a 30-character 16 px title is 288 px
// of ink. The larger of the two, so the layout is never narrower than the text.
inline constexpr float kFallbackAdvanceRatio = 0.6f;

// A row of the profiler table: what a name column, a value column and a
// sparkline are laid out as. Rebuilt per frame from the font in use, so a longer
// metric name widens its column instead of running into the value beside it.
struct TableStyle {
    float fontSize   = kTableFontSize;
    float advance    = kFallbackAdvanceRatio * kTableFontSize;
    float nameWidth  = kTableNameMax;
    float valueWidth = kTableValueWidth;
    float sparkWidth = kTableSparkWidth;
    float gap        = kTableGap;

    [[nodiscard]] auto ColumnWidth() const noexcept -> float {
        return nameWidth + valueWidth + sparkWidth + (gap * 2.0f);
    }
};

// The advance of one character at `fontSize`, straight from the atlas the
// Context will draw with -- the product GUIContext's own measure function uses.
// Absent or empty glyph data falls back to the ratio above.
[[nodiscard]] auto AdvancePerChar(const ZHLN::FontAtlas* atlas, float fontSize) noexcept -> float {
    if (atlas != nullptr && atlas->glyphCount > 0 && atlas->fontSize > 0.0f) {
        // Every font this tree ships is monospaced, so one advance is all of
        // them; in a proportional one 'M' is the widest, which is the safe
        // choice either way.
        const float advance = atlas->GlyphFor(static_cast<uint32_t>('M')).xadvance * atlas->ScaleFor(fontSize);
        if (advance > 0.0f) {
            return advance;
        }
    }
    return kFallbackAdvanceRatio * fontSize;
}

// `text` cut to the longest prefix whose ink fits `maxWidth`, with a two-dot
// ellipsis when anything was dropped. A text element wider than its box does not
// clip -- Clay draws past the edge -- so a name column too narrow for its
// content is a name drawn over the value beside it.
[[nodiscard]] auto FitToWidth(std::string_view text, float maxWidth, float advance) -> std::string {
    if (advance <= 0.0f || maxWidth <= 0.0f) {
        return std::string(text);
    }

    const size_t fits = static_cast<size_t>(maxWidth / advance);
    if (text.size() <= fits) {
        return std::string(text);
    }
    if (fits <= 3) {
        return std::string(text.substr(0, std::min(text.size(), fits)));
    }
    return std::format("{}..", text.substr(0, fits - 2));
}

// The widest name in a run of rows, in pixels. Not a measurement of the glyphs
// (that would be a pass per row per frame for a number that only moves when the
// table does): the per-character advance times the longest name is exact for a
// monospaced font and generous for a proportional one.
[[nodiscard]] auto WidestName(std::span<const FrameStageRow> rows, float advance) noexcept -> float {
    size_t longest = 0;
    for (const FrameStageRow& row: rows) {
        longest = std::max(longest, row.name.size());
    }
    return static_cast<float>(longest) * advance;
}

// Ink. Muted enough to sit over a running scene, opaque enough to read.
const JPH::Vec4 kPanelInk   = {0.05f, 0.06f, 0.09f, 0.92f};
const JPH::Vec4 kChartInk   = {0.02f, 0.03f, 0.05f, 0.85f};
const JPH::Vec4 kTitleInk   = {0.92f, 0.96f, 1.00f, 1.00f};
const JPH::Vec4 kBodyInk    = {0.78f, 0.84f, 0.92f, 1.00f};
const JPH::Vec4 kFastBarInk = {0.35f, 0.82f, 0.55f, 1.00f};
const JPH::Vec4 kSlowBarInk = {1.00f, 0.55f, 0.30f, 1.00f};

// The telemetry table: a GPU stage's sparkline, a CPU scope's, and the dimmer
// body ink a CPU scope's name is drawn in.
const JPH::Vec4 kGpuSparkInk = {0.42f, 0.66f, 0.92f, 1.00f};
const JPH::Vec4 kCpuSparkInk = {0.62f, 0.55f, 0.92f, 1.00f};
const JPH::Vec4 kCpuNameInk  = {0.66f, 0.72f, 0.80f, 1.00f};

// A bar is this much of the chart at minimum, so an idle frame still leaves a
// baseline to read the shape against.
inline constexpr float kMinBarHeight = 2.0f;

// How the frame-time chart picks its top: the window's average times the first
// is the scale when the picture is steady, the 1% low times the second is the
// floor under it, and whichever is taller wins. Scaling to the window's worst
// frame instead would let a single stall flatten everything else against the
// bottom; bars past the top are clipped, and the numbers they come from are
// printed above the chart.
inline constexpr float kChartScaleAverage = 2.5f;
inline constexpr float kChartScaleLows    = 1.5f;

} // namespace

struct BenchmarkViewer::Impl {
    std::unique_ptr<LogicBenchmark> benchmark;

    // The viewer's own wall clock: the accumulator that turns a host's frame
    // rate into the benchmark's fixed one. It is read every frame even while
    // paused, because Clock::GetDeltaTime is what moves the clock forward --
    // one that fell behind would hand the first frame after a resume a
    // backlog of ticks.
    Clock frameClock;
    float accumulatorSeconds = 0.0f;

    // The engine frame this viewer has already been ticked for. Both drawing
    // entry points advance the benchmark and sample the telemetry, and a host
    // is allowed to call two of them in one frame (a HUD panel and this one),
    // so the frame counter is what makes Tick() once-per-frame rather than
    // once-per-call.
    uint64_t lastFrameId = 0;
    bool     frameTicked = false;

    // Host-frame telemetry: the frame window, the profiler rows and the CSV
    // capture. Fed by Tick(), drawn by DrawTelemetryPanel().
    FrameTelemetry telemetry;

    size_t pendingEntities  = 250;
    bool   visible          = false;
    bool   telemetryVisible = false;
    bool   running          = true;

    // Box ids for the telemetry table, built into one buffer as the frame is
    // laid out. Clay interns the string at the call, so a single scratch is
    // enough for every id in a frame -- and the table is around 900 elements,
    // which is not a place to spend a std::format each.
    std::array<char, 48> idBuffer {};

    // One frame's worth of the fixed-step drive. Called from Tick(), so it runs
    // at most once per engine frame however many entry points the host calls.
    void Advance() noexcept;

    // The table's geometry for this frame, and the panel width it implies.
    // Built before the panel opens: a floating box's width has to be known when
    // it is declared, and the columns are what decide it.
    TableStyle tableGpu;
    TableStyle tableCpu;
    float      telemetryWidth   = kTelemetryWidthDefault;
    float      telemetryContent = kTelemetryWidthDefault - (kPanelPadding * 2.0f);

    void BuildTableStyles(ZHLN::Engine& engine, std::span<const FrameStageRow> gpu, std::span<const FrameStageRow> cpu);

    void DrawBenchmarkPanel(GUI::Context& ui);
    void DrawTelemetryPanel(GUI::Context& ui, ZHLN::Engine& engine);
    void DrawFrameTimeChart(GUI::Context& ui, std::span<const float> samples, const FrameTelemetrySnapshot& stats);
    void DrawStageColumns(GUI::Context& ui, std::span<const FrameStageRow> rows, const TableStyle& style, std::string_view prefix);
    void DrawStageColumn(GUI::Context& ui, std::span<const FrameStageRow> rows, const TableStyle& style, std::string_view prefix, size_t column);
    void DrawStageRow(GUI::Context& ui, const FrameStageRow& row, const TableStyle& style, std::string_view prefix, size_t column, size_t index);
    void ToggleCapture();

    // `part` tells the row's boxes apart (0 name, 1 value, 2 spark box, 3+ spark
    // bars); `column` the two runs of rows.
    [[nodiscard]] auto TableId(std::string_view prefix, size_t column, size_t index, size_t part) -> std::string_view {
        char* out = std::format_to(idBuffer.data(), "{}-{}-{}-{}", prefix, column, index, part);
        return std::string_view {idBuffer.data(), static_cast<size_t>(out - idBuffer.data())};
    }
};

void BenchmarkViewer::Impl::Advance() noexcept {
    const float dt = std::min(frameClock.GetDeltaTime(), 0.25f);

    if (benchmark == nullptr || !running) {
        accumulatorSeconds = 0.0f;
        return;
    }

    accumulatorSeconds += dt;

    int steps = 0;
    while (accumulatorSeconds >= kFixedStep && steps < kMaxTicksPerFrame) {
        benchmark->Step(kFixedStep);
        accumulatorSeconds -= kFixedStep;
        ++steps;
    }

    if (steps >= kMaxTicksPerFrame) {
        // The host's frame was longer than the whole budget. Drop the backlog
        // instead of carrying it: a measuring tool that stutters the thing it
        // is measuring has stopped measuring it.
        accumulatorSeconds = 0.0f;
    }
}

void BenchmarkViewer::Impl::BuildTableStyles(ZHLN::Engine& engine, std::span<const FrameStageRow> gpu, std::span<const FrameStageRow> cpu) {
    // The atlas the Context will draw with, so the columns are sized against the
    // font that is actually on screen rather than a guess at its metrics.
    const auto             settings = engine.GetRegistry().GetSingleton<ZHLN::GUI::UISettingsComponent>();
    const ZHLN::FontAtlas* atlas    = (settings && settings->fontAtlas.glyphCount > 0) ? &settings->fontAtlas : nullptr;
    const float            advance  = AdvancePerChar(atlas, kTableFontSize);

    // What the screen can give the panel: the window, less the margin it keeps
    // at both edges, less the benchmark panel's half of the left side.
    const ZHLN::Extent2D viewport  = engine.GetPlatformHost().GetSize();
    const float          available = std::max(240.0f, static_cast<float>(viewport.width) - (kPanelMargin * 3.0f) - kPanelWidth);

    // The GPU stages are identifiers: the column is as wide as the longest one,
    // clamped, so "VolumetricLightInject" is drawn in full where it fits and
    // shortened with an ellipsis where it does not.
    tableGpu            = TableStyle {.nameWidth = std::clamp(WidestName(gpu, advance) + kTableGap, kTableNameMin, kTableNameMax)};
    tableGpu.advance    = advance;
    tableGpu.fontSize   = kTableFontSize;
    tableGpu.valueWidth = kTableValueWidth;
    tableGpu.sparkWidth = kTableSparkWidth;
    tableGpu.gap        = kTableGap;

    float content = (tableGpu.ColumnWidth() * 2.0f) + kColumnGap;

    // A table wider than the screen shrinks its own name columns rather than
    // spilling past the window edge -- the two columns share the overflow.
    if (content + (kPanelPadding * 2.0f) > available) {
        const float overflow = (content + (kPanelPadding * 2.0f)) - available;
        tableGpu.nameWidth   = std::clamp(tableGpu.nameWidth - (overflow * 0.5f), kTableNameMin, kTableNameMax);
        content              = (tableGpu.ColumnWidth() * 2.0f) + kColumnGap;
    }

    // The CPU scopes are sentences and there are only a handful of them: one
    // column across the whole content width, which is what lets "ECS System:
    // Native C++ Gameplay Update" be drawn in full.
    tableCpu           = tableGpu;
    tableCpu.nameWidth = std::max(kTableNameMin, content - (tableCpu.valueWidth + tableCpu.sparkWidth + (tableCpu.gap * 2.0f)));

    telemetryContent = content;
    telemetryWidth   = content + (kPanelPadding * 2.0f);
}

void BenchmarkViewer::Impl::DrawBenchmarkPanel(GUI::Context& ui) {
    if (benchmark == nullptr) {
        return;
    }

    const BenchmarkMetrics&   metrics = benchmark->GetMetrics();
    const std::vector<float>& history = benchmark->GetTickHistory();

    // Ticks per second over the run, and the last second of history: the two
    // numbers the chart cannot show.
    const double rate = metrics.totalTimeSec > 0.0 ? static_cast<double>(metrics.totalTicks) / metrics.totalTimeSec : 0.0;

    ui.Box(
        "BenchmarkPanel",
        GUI::BoxConfig {
            .width        = {.fixed = kPanelWidth},
            .height       = {.fit = true},
            .color        = kPanelInk,
            .cornerRadius = {6.0f, 6.0f, 6.0f, 6.0f},
            .padding      = kPanelPadding,
            .gap          = 6.0f,
            .direction    = GUI::Direction::Column,
            .offsetX      = kPanelMargin,
            .offsetY      = kPanelMargin,
        },
        [&]() -> void {
            // The title is 34 characters: at 13 px against a 300 px content width
            // it fits with room to spare, and at 16 it would wrap.
            ui.Text("Logic Benchmark  (zahlen_benchmark)", 13.0f, kTitleInk);

            ui.Row(8.0f, [&]() -> void {
                ui.Checkbox("Running", running);
                if (ui.Button("Step")) {
                    benchmark->Step(kFixedStep);
                }
                if (ui.Button("Reset")) {
                    benchmark->Reset();
                    benchmark->Initialize(pendingEntities);
                }
            });

            // One concern per line, each short enough that it cannot wrap: this
            // panel is 300 px of content, and a wrapped line of numbers is a line
            // nobody reads.
            ui.Text(std::format("entities {:>6}   bodies {:>6}", metrics.entityCount, metrics.activePhysicsBodies), 12.0f, kBodyInk);
            ui.Text(std::format("tick {:.3f} ms   avg {:.3f} ms", metrics.lastTickTimeMs, metrics.avgTickTimeMs), 12.0f, kBodyInk);
            ui.Text(std::format("min {:.3f} ms   max {:.3f} ms", metrics.minTickTimeMs, metrics.maxTickTimeMs), 12.0f, kBodyInk);
            ui.Text(std::format("ticks {}   wall {:.2f} s   {:.0f} t/s", metrics.totalTicks, metrics.totalTimeSec, rate), 12.0f, kBodyInk);

            // The chart: the last kChartBars ticks, each bar scaled against the
            // worst tick in the window. Bars are laid out left to right and
            // bottom-aligned, which is what makes them read as time.
            ui.Box(
                "BenchmarkChart",
                GUI::BoxConfig {
                    .width      = {.fixed = kPanelContentWidth},
                    .height     = {.fixed = kChartHeight},
                    .color      = kChartInk,
                    .gap        = kBarGap,
                    .direction  = GUI::Direction::Row,
                    .alignCross = GUI::Alignment::End,
                },
                [&]() -> void {
                    const size_t shown = std::min(history.size(), kChartBars);
                    if (shown == 0) {
                        return;
                    }

                    const auto  bars    = static_cast<float>(shown);
                    const float barWide = (kPanelContentWidth - ((bars - 1.0f) * kBarGap)) / bars;
                    const float worst   = std::max(metrics.maxTickTimeMs, 0.001f);

                    for (size_t i = history.size() - shown; i < history.size(); ++i) {
                        const float sample = history[i];
                        const float fill   = std::clamp(sample / worst, 0.0f, 1.0f);
                        const float height = std::max(kMinBarHeight, fill * kChartHeight);

                        // Green to orange over the window: the shape of the
                        // distribution at a glance, with the outliers the eye
                        // is looking for at the top.
                        const JPH::Vec4 ink {
                            kFastBarInk.GetX() + ((kSlowBarInk.GetX() - kFastBarInk.GetX()) * fill),
                            kFastBarInk.GetY() + ((kSlowBarInk.GetY() - kFastBarInk.GetY()) * fill),
                            kFastBarInk.GetZ() + ((kSlowBarInk.GetZ() - kFastBarInk.GetZ()) * fill), 1.0f
                        };

                        // A Box with no children still draws: Clay fills the
                        // element's background, and Begin/End is the contract.
                        ui.BeginBox(std::format("BenchmarkBar{}", i), GUI::BoxConfig {.width = {.fixed = barWide}, .height = {.fixed = height}, .color = ink});
                        ui.EndBox();
                    }
                }
            );

            ui.Row(8.0f, [&]() -> void {
                if (ui.Button("+250")) {
                    benchmark->SpawnWorkload(250);
                }
                if (ui.Button("+1000")) {
                    benchmark->SpawnWorkload(1000);
                }
            });
            ui.Text("adds bodies to the live world", 11.0f, kBodyInk);
        }
    );
}

void BenchmarkViewer::Impl::DrawFrameTimeChart(GUI::Context& ui, std::span<const float> samples, const FrameTelemetrySnapshot& stats) {
    ui.Box(
        "TelemetryChart",
        GUI::BoxConfig {
            .width      = {.fixed = telemetryContent},
            .height     = {.fixed = kTelemetryChartHeight},
            .color      = kChartInk,
            .gap        = kBarGap,
            .direction  = GUI::Direction::Row,
            .alignCross = GUI::Alignment::End,
        },
        [&]() -> void {
            if (samples.empty()) {
                return;
            }

            const float bars    = static_cast<float>(samples.size());
            const float barWide = (telemetryContent - ((bars - 1.0f) * kBarGap)) / bars;

            // The chart is scaled to the frame time that is normal, not to the
            // worst one: a window holding one 200 ms stall would otherwise
            // flatten every other bar against the floor. Bars past the top of
            // the scale are clipped, and the lows they come from are printed
            // directly above the chart.
            const float scale = std::max({stats.frameMsAvg * kChartScaleAverage, stats.frameMsLow1 * kChartScaleLows, 1.0f});

            for (size_t i = 0; i < samples.size(); ++i) {
                const float sample = samples[i];
                const float fill   = std::clamp(sample / scale, 0.0f, 1.0f);
                const float height = std::max(kMinBarHeight, fill * kTelemetryChartHeight);

                // Green below the 1% low, orange at or above it: the tail the
                // percentiles are computed from is the visible top of the chart.
                const JPH::Vec4 ink = sample >= stats.frameMsLow1 ? kSlowBarInk : kFastBarInk;

                ui.BeginBox(TableId("tchart", 0, i, 0), GUI::BoxConfig {.width = {.fixed = barWide}, .height = {.fixed = height}, .color = ink});
                ui.EndBox();
            }
        }
    );
}

void BenchmarkViewer::Impl::DrawStageColumns(GUI::Context& ui, std::span<const FrameStageRow> rows, const TableStyle& style, std::string_view prefix) {
    if (rows.empty()) {
        return;
    }

    // Two runs of rows: 27 stages in one column is a panel taller than most
    // windows. The columns share the rows between them, longer half first.
    const size_t half = (rows.size() + 1) / 2;
    ui.Box(
        "", GUI::BoxConfig {.width = {.grow = 1.0f}, .gap = kColumnGap, .direction = GUI::Direction::Row, .alignCross = GUI::Alignment::Start}, [&]() -> void {
            DrawStageColumn(ui, rows.first(half), style, prefix, 0);
            DrawStageColumn(ui, rows.subspan(half), style, prefix, 1);
        }
    );
}

void BenchmarkViewer::Impl::DrawStageColumn(
    GUI::Context&                  ui,
    std::span<const FrameStageRow> rows,
    const TableStyle&              style,
    std::string_view               prefix,
    size_t                         column
) {
    if (rows.empty()) {
        return;
    }

    ui.Column(2.0f, [&]() -> void {
        for (size_t i = 0; i < rows.size(); ++i) {
            DrawStageRow(ui, rows[i], style, prefix, column, i);
        }
    });
}

void BenchmarkViewer::Impl::DrawStageRow(
    GUI::Context&        ui,
    const FrameStageRow& row,
    const TableStyle&    style,
    std::string_view     prefix,
    size_t               column,
    size_t               index
) {
    // Heights are left to fit their content rather than pinned to a row height:
    // a metric whose name is wider than its column then makes its row taller,
    // instead of drawing its second line over the row below. The name itself is
    // cut to the column first, so that is the backstop, not the plan.
    ui.Box(
        "", GUI::BoxConfig {.width = {.fixed = style.ColumnWidth()}, .gap = style.gap, .direction = GUI::Direction::Row, .alignCross = GUI::Alignment::Center},
        [&]() -> void {
            ui.Box(
                TableId(prefix, column, index, 0), GUI::BoxConfig {.width = {.fixed = style.nameWidth}, .alignCross = GUI::Alignment::Center},
                [&]() -> void { ui.Text(FitToWidth(row.name, style.nameWidth, style.advance), style.fontSize, row.gpuStage ? kBodyInk : kCpuNameInk); }
            );

            ui.Box(
                TableId(prefix, column, index, 1),
                GUI::BoxConfig {.width = {.fixed = style.valueWidth}, .alignMain = GUI::Alignment::End, .alignCross = GUI::Alignment::Center},
                [&]() -> void { ui.Text(std::format("{:.2f}", row.lastMs), style.fontSize, kBodyInk); }
            );

            // The sparkline is the metric's own history, scaled to the row's own
            // peak: the question a row answers is "is this metric spiking", and a
            // shared scale would flatten every stage that is not the heaviest one.
            const float bars    = static_cast<float>(row.spark.size());
            const float barWide = (style.sparkWidth - ((bars - 1.0f) * kSparkBarGap)) / bars;
            float       peak    = 0.001f;
            for (const float sample: row.spark) {
                peak = std::max(peak, sample);
            }

            ui.Box(
                TableId(prefix, column, index, 2),
                GUI::BoxConfig {
                    .width      = {.fixed = style.sparkWidth},
                    .height     = {.fixed = kStageSparkHeight},
                    .gap        = kSparkBarGap,
                    .direction  = GUI::Direction::Row,
                    .alignCross = GUI::Alignment::End,
                },
                [&]() -> void {
                    for (size_t bar = 0; bar < row.spark.size(); ++bar) {
                        const float fill   = std::clamp(row.spark[bar] / peak, 0.0f, 1.0f);
                        const float height = std::max(kMinBarHeight, fill * kStageSparkHeight);
                        ui.BeginBox(
                            TableId(prefix, column, index, 3 + bar),
                            GUI::BoxConfig {.width = {.fixed = barWide}, .height = {.fixed = height}, .color = row.gpuStage ? kGpuSparkInk : kCpuSparkInk}
                        );
                        ui.EndBox();
                    }
                }
            );
        }
    );
}

void BenchmarkViewer::Impl::ToggleCapture() {
    if (telemetry.IsCapturing()) {
        telemetry.StopCapture();
        ZHLN::Log("[Bench] frame telemetry capture stopped: {} frames -> {}", telemetry.CapturedFrames(), telemetry.CapturePath().string());
        return;
    }

    if (telemetry.StartCapture()) {
        ZHLN::Log("[Bench] frame telemetry capture started: {}", telemetry.CapturePath().string());
        return;
    }

    ZHLN::LogError("[Bench] frame telemetry capture could not open {}", telemetry.CapturePath().string());
}

void BenchmarkViewer::Impl::DrawTelemetryPanel(GUI::Context& ui, ZHLN::Engine& engine) {
    const FrameTelemetrySnapshot&        stats    = telemetry.Snapshot();
    const std::span<const float>         chart    = telemetry.ChartSamples();
    const std::span<const FrameStageRow> stages   = telemetry.Stages();
    const size_t                         gpuCount = telemetry.GpuStageCount();

    BuildTableStyles(engine, stages.first(gpuCount), stages.subspan(gpuCount));

    // The panel sits on the right-hand side: the benchmark panel owns the left
    // corner, and Clay anchors a floating box at its parent's top-left, so the
    // viewport width is what puts this one at the other edge. Its width is what
    // its columns came to -- a floating box that is narrower than its content is
    // not clipped, it spills.
    const ZHLN::Extent2D viewport = engine.GetPlatformHost().GetSize();
    const float          rightX   = static_cast<float>(viewport.width) - telemetryWidth - kPanelMargin;

    ui.Box(
        "FrameTelemetryPanel",
        GUI::BoxConfig {
            .width        = {.fixed = telemetryWidth},
            .height       = {.fit = true},
            .color        = kPanelInk,
            .cornerRadius = {6.0f, 6.0f, 6.0f, 6.0f},
            .padding      = kPanelPadding,
            .gap          = 6.0f,
            .direction    = GUI::Direction::Column,
            .offsetX      = std::max(kPanelMargin, rightX),
            .offsetY      = kPanelMargin,
        },
        [&]() -> void {
            ui.Text("Frame Telemetry  (host frames)", 16.0f, kTitleInk);

            // Three short lines rather than two long ones: at 13 px these are 27
            // to 45 characters wide against a content width that holds about 66,
            // so none of them can wrap into the ones below.
            ui.Row(10.0f, [&]() -> void {
                ui.Text(std::format("avg {:.1f} fps", stats.avgFps), 17.0f, kTitleInk);
                ui.Text(std::format("now {:.1f}", stats.instantFps), 12.0f, kBodyInk);
                ui.Text(std::format("frame {:.2f} ms", stats.frameMsLast), 12.0f, kBodyInk);
            });

            ui.Text(std::format("1% low {:.2f} ms   0.1% low {:.2f} ms", stats.frameMsLow1, stats.frameMsLow01), 13.0f, kBodyInk);
            ui.Text(std::format("avg {:.2f}  min {:.2f}  max {:.2f} ms", stats.frameMsAvg, stats.frameMsMin, stats.frameMsMax), 12.0f, kCpuNameInk);
            ui.Text(std::format("{} frames   {:.0f} s window", stats.frames, stats.windowSeconds), 12.0f, kCpuNameInk);
            if (stats.clippedSamples > 0) {
                ui.Text(std::format("{} frames over {:.0f} ms counted apart", stats.clippedSamples, FrameTelemetry::kSampleCeilingMs), 11.0f, kSlowBarInk);
            }

            DrawFrameTimeChart(ui, chart, stats);

            ui.Row(6.0f, [&]() -> void {
                if (ui.Button(telemetry.IsCapturing() ? "Stop capture" : "Capture CSV")) {
                    ToggleCapture();
                }
                if (ui.Button("Clear")) {
                    telemetry.Reset();
                }
            });

            if (telemetry.IsCapturing()) {
                ui.Text(std::format("capturing {}   ({} frames)", telemetry.CapturePath().filename().string(), telemetry.CapturedFrames()), 11.0f, kFastBarInk);
            } else if (!telemetry.CapturePath().empty()) {
                ui.Text(
                    std::format("last capture {}   ({} frames)", telemetry.CapturePath().filename().string(), telemetry.CapturedFrames()), 11.0f, kCpuNameInk
                );
            } else {
                ui.Text("capture writes one CSV row per frame under build/cache/captures", 11.0f, kCpuNameInk);
            }

            ui.Text(std::format("GPU  {:.2f} ms total   ({} stages)", stats.gpuTotalMs, gpuCount), 13.0f, kTitleInk);
            DrawStageColumns(ui, stages.first(gpuCount), tableGpu, "g");

            if (stages.size() > gpuCount) {
                ui.Text(std::format("CPU  ({} scopes)", stages.size() - gpuCount), 13.0f, kTitleInk);

                // One column: the scopes are named in prose, and the full width
                // is what lets those names be drawn as written.
                ui.Column(2.0f, [&]() -> void {
                    for (size_t i = 0; i < stages.size() - gpuCount; ++i) {
                        DrawStageRow(ui, stages[gpuCount + i], tableCpu, "c", 0, i);
                    }
                });
            }
        }
    );
}

BenchmarkViewer::BenchmarkViewer(): m_impl(std::make_unique<Impl>()) {
}

BenchmarkViewer::~BenchmarkViewer() = default;

auto BenchmarkViewer::Run(size_t initialEntities) -> std::expected<void, ErrorCode> {
    // Engine::Run owns the platform, the window, the device and the loop; the
    // viewer only has to be the thing its UI phase calls. The options are the
    // defaults (windowed, vsync, validation on) because the CLI is the host's
    // to parse, not the tool's -- see the header.
    ZHLN::CommandLineOptions options {};
    options.driver = ZHLN::GameplayDriver::Cpp;

    // Static storage duration is required: the crash handler keeps the address
    // of this struct in its signal slots. See <Zahlen/Core/CrashState.hpp>.
    static ZHLN::CrashState crashState;

    BenchmarkViewer* self = this;
    ZHLN::Clock      uiClock;
    m_impl->pendingEntities  = initialEntities;
    m_impl->visible          = true;
    m_impl->telemetryVisible = true; // the standalone tool is the window for both panels

    auto ran =
        ZHLN::Engine::Run(options, crashState, [self, &uiClock](ZHLN::Engine& engine) -> void { self->UpdateAndRenderUI(engine, uiClock.GetDeltaTime()); });

    // The engine is gone, and with it the scheduler the benchmark's World waits
    // on at teardown (see the lifetime note in the header). Release the
    // benchmark here, while that is the only thing left to do, rather than
    // leaving a World for whoever destroys the viewer afterwards. The viewer is
    // empty again and a later Attach() re-arms it.
    m_impl->benchmark.reset();
    m_impl->accumulatorSeconds = 0.0f;

    // A capture started from the panel ends with the run: the summary line and
    // the flush belong to this call, not to whichever destructor comes next.
    m_impl->telemetry.StopCapture();
    return ran;
}

void BenchmarkViewer::Attach(ZHLN::Engine& engine, size_t initialEntities) {
    m_impl->pendingEntities = initialEntities;

    // The Engine is consulted for exactly one thing: a headless host has no
    // window to draw a panel into, and saying so here is cheaper than drawing
    // into nothing every frame.
    if (!static_cast<bool>(engine.GetWindow())) {
        m_impl->visible = false;
    }

    m_impl->benchmark = std::make_unique<LogicBenchmark>();
    if (!m_impl->benchmark->Initialize(initialEntities)) {
        ZHLN::LogError("[Bench] viewer: the benchmark world failed to initialize, so there is nothing to draw.");
        m_impl->benchmark.reset();
        return;
    }

    m_impl->accumulatorSeconds = 0.0f;
    ZHLN::Log("[Bench] viewer attached to the engine: {} entities, panel {}.", initialEntities, m_impl->visible ? "shown" : "hidden");
}

void BenchmarkViewer::Tick(ZHLN::Engine& engine) {
    const uint64_t frameId = engine.GetCurrentFrame();
    if (m_impl->frameTicked && frameId == m_impl->lastFrameId) {
        return; // this engine frame has been ticked already: one frame, one sample
    }
    m_impl->frameTicked = true;
    m_impl->lastFrameId = frameId;

    // The telemetry first: its frame interval is the host's frame, and the
    // benchmark's substeps below are a cost inside it.
    m_impl->telemetry.Tick();

    // The benchmark's world is stepped while its panel is up, which is what the
    // panel has always meant: it is a diagnostic, not a background load, and a
    // hidden one must not keep a 2000-body simulation running behind the scene
    // the host is actually drawing. Nothing here decides to create that world --
    // attaching is the drawing entry points' business, so a session that never
    // shows the panel never builds one.
    if (m_impl->visible) {
        m_impl->Advance();
    }
}

void BenchmarkViewer::UpdateAndRenderUI(ZHLN::Engine& engine, float dt) {
    if (m_impl->benchmark == nullptr) {
        Attach(engine, m_impl->pendingEntities);
    }
    Tick(engine);

    if (!m_impl->visible && !m_impl->telemetryVisible) {
        return;
    }

    GUI::Context ui(engine);
    ui.BeginFrame(dt);
    if (m_impl->visible && m_impl->benchmark != nullptr) {
        m_impl->DrawBenchmarkPanel(ui);
    }
    if (m_impl->telemetryVisible) {
        m_impl->DrawTelemetryPanel(ui, engine);
    }
    engine.SetPendingUIData(ui.EndFrame());
}

void BenchmarkViewer::DrawPanel(GUI::Context& ui, ZHLN::Engine& engine) {
    if (m_impl->benchmark == nullptr) {
        Attach(engine, m_impl->pendingEntities);
    }
    Tick(engine);
    if (m_impl->benchmark != nullptr) {
        m_impl->DrawBenchmarkPanel(ui);
    }
}

void BenchmarkViewer::DrawTelemetryPanel(GUI::Context& ui, ZHLN::Engine& engine) {
    Tick(engine);
    m_impl->DrawTelemetryPanel(ui, engine);
}

void BenchmarkViewer::DrawPanels(GUI::Context& ui, ZHLN::Engine& engine) {
    if (m_impl->benchmark == nullptr) {
        Attach(engine, m_impl->pendingEntities);
    }
    Tick(engine);

    if (m_impl->visible && m_impl->benchmark != nullptr) {
        m_impl->DrawBenchmarkPanel(ui);
    }
    if (m_impl->telemetryVisible) {
        m_impl->DrawTelemetryPanel(ui, engine);
    }
}

void BenchmarkViewer::SetVisible(bool visible) noexcept {
    m_impl->visible = visible;
}

auto BenchmarkViewer::IsVisible() const noexcept -> bool {
    return m_impl->visible;
}

void BenchmarkViewer::ToggleVisible() noexcept {
    m_impl->visible = !m_impl->visible;
}

void BenchmarkViewer::SetTelemetryVisible(bool visible) noexcept {
    m_impl->telemetryVisible = visible;
}

auto BenchmarkViewer::IsTelemetryVisible() const noexcept -> bool {
    return m_impl->telemetryVisible;
}

void BenchmarkViewer::ToggleTelemetryVisible() noexcept {
    m_impl->telemetryVisible = !m_impl->telemetryVisible;
}

void BenchmarkViewer::SetRunning(bool running) noexcept {
    if (running && !m_impl->running) {
        // Resume where the pause ended: the clock is read once so the interval
        // just spent paused is not replayed as ticks.
        static_cast<void>(m_impl->frameClock.GetDeltaTime());
        m_impl->accumulatorSeconds = 0.0f;
    }
    m_impl->running = running;
}

auto BenchmarkViewer::IsRunning() const noexcept -> bool {
    return m_impl->running;
}

auto BenchmarkViewer::GetBenchmark() noexcept -> LogicBenchmark* {
    return m_impl->benchmark.get();
}

auto BenchmarkViewer::GetBenchmark() const noexcept -> const LogicBenchmark* {
    return m_impl->benchmark.get();
}

auto BenchmarkViewer::GetFrameTelemetry() noexcept -> FrameTelemetry& {
    return m_impl->telemetry;
}

auto BenchmarkViewer::GetFrameTelemetry() const noexcept -> const FrameTelemetry& {
    return m_impl->telemetry;
}

} // namespace ZHLN::Bench
