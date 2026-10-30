// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Viewer/BenchmarkViewer.cpp
#include <Benchmark/Viewer/BenchmarkViewer.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/GUI/UIData.hpp>
#include <Widget/BarStrip.hpp>
#include <Widget/HoverCard.hpp>
#include <Widget/TextFit.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
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

// The machine section: a grid of per-core cells. Cells are as wide as the
// content divided by how many fit across, and the number across is chosen from
// the core count so a 20-core machine is five rows and a 128-thread one is
// still about the same height -- narrowing the cells rather than growing the
// panel. Past kCoreCellsMax the label in a cell no longer fits and the cell
// becomes its own history alone.
inline constexpr size_t kCoreGridRows      = 6; // rows aimed for before the grid widens
inline constexpr size_t kCoreCellsMin      = 4;
inline constexpr size_t kCoreCellsMax      = 10;
inline constexpr float  kCoreCellFontSize  = 10.0f;
inline constexpr float  kCoreCellGap       = 4.0f;
inline constexpr float  kCoreSparkHeight   = 10.0f;
inline constexpr float  kCoreSparkBarGap   = 1.0f;
inline constexpr size_t kCoreSparkBars     = 12;

// The smallest a row's sparkline scales to, in ms. Bars are drawn against the
// row's own peak, so a row whose whole window sits at the noise floor -- a pass
// that costs 0.00 ms -- would otherwise fill every bar to the top and read as
// the busiest thing on the panel. Half a tenth of a millisecond is under the
// measurement noise of a stage that ran, so anything under it is drawn small,
// and anything above it still draws its spikes at their own scale.
inline constexpr float kSparkFloorMs = 0.05f;

// The card a line or a cell opens under the pointer. It carries what the panel
// would otherwise print as a second line under every reading -- the window's
// tails, the clock, the provenance -- so the panel keeps what a glance needs and
// the rest is one hover away. The card itself is drawn by the widget library
// (see <Widget/HoverCard.hpp>, and Impl::CardStyle for these numbers): what is
// left here is the panel's own type size and spacing.
inline constexpr float kCardFontSize   = 11.0f;
inline constexpr float kCardLineHeight = 14.0f; // a line at that size, plus its gap
inline constexpr float kCardPadding    = 6.0f;
inline constexpr float kCardGap        = 4.0f;

// The panel's own element id: a card is placed against the element it belongs to
// and against this, both as they were laid out in the last frame.
inline constexpr const char* kTelemetryPanelId = "FrameTelemetryPanel";

// Load levels for a core cell: green while there is headroom, amber as it fills,
// red when it has none. A core cell is 12 px of screen and cannot carry a
// legend, so the colour is the quick read and the number beside it is the exact
// one.
inline constexpr float kCoreLoadWarm = 50.0f;
inline constexpr float kCoreLoadHot  = 85.0f;

// The telemetry panel's widest case, used before the first frame has measured
// one: two columns of the widest name column, plus its padding.
inline constexpr float kTelemetryWidthDefault = ((kTableNameMax + kTableValueWidth + kTableSparkWidth + (kTableGap * 2.0f)) * 2.0f) + kColumnGap +
                                                (kPanelPadding * 2.0f);

// Per-character advance assumed before the atlas has been measured, when the
// table is laid out for the first time. The rule and its fallback live in
// <Widget/TextFit.hpp>; this is the same ratio, so a first frame and a measured
// frame agree.
inline constexpr float kFallbackAdvanceRatio = ZHLN::GUI::Widget::kFallbackAdvanceRatio;

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

// The advance of one character, the fit that keeps a line inside its column and
// the slack it leaves are the widget library's now: the panel asks
// <Widget/TextFit.hpp> for all three instead of carrying its own copy of rules
// that every panel needs.
using ZHLN::GUI::Widget::AdvancePerChar;
using ZHLN::GUI::Widget::FitToWidth;
using ZHLN::GUI::Widget::FitsWidth;

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

// A stage row the profiler has stopped hearing from: dimmer than the body ink,
// so a frozen number reads as a number that is not about now.
const JPH::Vec4 kStaleInk = {0.45f, 0.50f, 0.58f, 1.00f};

// The machine section: a core cell's history bar, and the red a core at its
// ceiling is drawn in.
const JPH::Vec4 kCoreSparkInk = {0.36f, 0.78f, 0.68f, 1.00f};
const JPH::Vec4 kHotInk       = {1.00f, 0.34f, 0.30f, 1.00f};

// A hovered line's card: a shade apart from the panel it floats over, so it
// reads as a card on top of the panel rather than as more panel.
const JPH::Vec4 kCardInk = {0.10f, 0.11f, 0.14f, 0.98f};

// The colour a usage reading is drawn in: a level, not a duration, so the same
// value is always the same colour wherever it appears (a core cell, the
// machine-wide line, the GPU line).
[[nodiscard]] auto LoadInk(float percent) -> JPH::Vec4 {
    if (percent >= kCoreLoadHot) {
        return kHotInk;
    }
    return percent >= kCoreLoadWarm ? kSlowBarInk : kFastBarInk;
}

// A core cell's history bars read by the same ramp as its percentage, and the
// bars before the core's first sample are an absence rather than a reading --
// which is the one thing a bar strip cannot decide for itself, so it asks.
[[nodiscard]] auto CoreBarInk(void* /*user*/, [[maybe_unused]] size_t bar, float value, bool absent) -> JPH::Vec4 {
    return absent ? kChartInk : LoadInk(value);
}

// The tick chart's bars: green to orange over the window, so the shape of the
// distribution reads at a glance with the outliers the eye is looking for at the
// top. The ramp is the bar's share of the scale the strip is drawing with, which
// the caller passes by pointer -- the policy is told a bar's value, and the scale
// is the caller's own number.
[[nodiscard]] auto TickBarInk(void* user, [[maybe_unused]] size_t bar, float value, [[maybe_unused]] bool absent) -> JPH::Vec4 {
    const float peak = *static_cast<const float*>(user);
    const float fill = peak > 0.0f ? std::clamp(value / peak, 0.0f, 1.0f) : 0.0f;
    return JPH::Vec4 {
        kFastBarInk.GetX() + ((kSlowBarInk.GetX() - kFastBarInk.GetX()) * fill),
        kFastBarInk.GetY() + ((kSlowBarInk.GetY() - kFastBarInk.GetY()) * fill),
        kFastBarInk.GetZ() + ((kSlowBarInk.GetZ() - kFastBarInk.GetZ()) * fill),
        1.0f
    };
}

// One hardware series as the line the panel prints: the level now, the level
// averaged, and the two tails of the window, all in the series' own unit --
// percent as written, MHz divided down to GHz. The percentiles are labelled
// rather than only shown, because a reader coming from the frame-time lows
// above would otherwise read p99 as a mean of the busiest 1% of samples, which
// is not what a hardware level means (see SystemTelemetry.hpp).
//
// The caller has already checked that the window has a summary to print: an
// unsampled series is an empty optional, and there is no line to draw for it.
[[nodiscard]] auto SeriesLine(std::string_view label, const HardwareSeries& series, float scale, int decimals, std::string_view unit) -> std::string {
    // The 'f' is load-bearing: without a conversion type, a precision is a
    // precision in the general presentation, which is the printf %g rule --
    // shortest of fixed and scientific -- so 10.0 prints as "1e+01", 100.0 as
    // "1e+02", and 4.00 GHz loses its zeros and prints as "4". Every number
    // this panel prints is written plainly.
    const auto at = [scale, decimals, unit](float value) -> std::string { return std::format("{:.{}f}{}", value / scale, decimals, unit); };

    // An empty label is a caller that has already named the thing earlier on the
    // line -- the GPU's device name, say -- so the numbers stand on their own
    // and that caller spaces them.
    return std::format(
        "{}{}{}   avg {}   p1 {}   p99 {}", label, label.empty() ? "" : "  ", at(series.last), at(series.avg), at(series.p1), at(series.p99)
    );
}

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

// The card the pointer opens is the widget library's (see
// <Widget/HoverCard.hpp>): the panel owns one state, whatever the pointer is
// over fills it while the panel is being drawn, and the panel draws it last --
// floating, out of the layout flow, and on top of everything above it. A card
// drawn the moment it is decided sits under every row below it, which is the bug
// the ordering exists to prevent.

struct BenchmarkViewer::Impl {
    std::unique_ptr<LogicBenchmark> benchmark;

    // The viewer's own monotonic clock (`maxDeltaTime = 0.25f`), with built-in
    // pause resync and fixed-step accumulation for stepping the benchmark.
    Clock frameClock {ClockConfig {.maxDeltaTime = 0.25f}};

    // The engine frame this viewer has already been ticked for. Both drawing
    // entry points advance the benchmark and sample the telemetry, and a host
    // is allowed to call two of them in one frame (a HUD panel and this one),
    // so the frame counter is what makes Tick() once-per-frame rather than
    // once-per-call.
    uint64_t lastFrameId = 0;
    bool     frameTicked = false;

    // Whether the telemetry has been told which device the frames are drawn on.
    // Set on the first frame that has a device name to give it.
    bool gpuNameTaken = false;

    // Host-frame telemetry: the frame window, the profiler rows and the CSV
    // capture. Fed by Tick(), drawn by DrawTelemetryPanel().
    FrameTelemetry telemetry;

    // The card the pointer is over, when it is over one: set by the section that
    // owns the element, drawn by the panel once everything else is down. Reset at
    // the top of the panel, so it is one card per frame however many elements
    // recognise the pointer.
    GUI::Widget::HoverState hover;

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

    // The per-character advance at another size, from the one the table measured
    // off the atlas: a line measured at the wrong size is a line that either
    // spills past the panel or is cut shorter than it had to be, and this panel
    // draws at three sizes.
    [[nodiscard]] auto AdvanceAt(float fontSize) const noexcept -> float {
        return ZHLN::GUI::Widget::AdvanceAt(tableCpu.advance, tableCpu.fontSize, fontSize);
    }

    // The card's geometry: a line at 11 px with kCardLineHeight under it, the
    // panel's own content width as the ceiling, and the panel's margin keeping it
    // off the window edge. The advance is the one the table measured this frame,
    // so a card cannot be laid out at a size nothing is drawn at.
    [[nodiscard]] auto CardStyle() const noexcept -> GUI::Widget::HoverCardStyle {
        return GUI::Widget::HoverCardStyle {
            .fontSize     = kCardFontSize,
            .lineHeight   = kCardLineHeight,
            .advance      = AdvanceAt(kCardFontSize),
            .padding      = kCardPadding,
            .anchorGap    = kCardGap,
            .windowMargin = kPanelMargin,
            .maxWidth     = telemetryContent,
            .ink          = kCardInk,
            .textInk      = kBodyInk,
        };
    }

    GUI::Context gui;

    void DrawBenchmarkPanel(GUI::Frame& ui);
    void DrawTelemetryPanel(GUI::Frame& ui, ZHLN::Engine& engine);

    // The machine under the frames: CPU usage and clock (machine-wide, then
    // every core as a cell), and the GPU's utilization and clock. Reads the
    // SystemTelemetry the frame telemetry owns; nothing here names a platform.
    void DrawHardwareSection(GUI::Frame& ui, const SystemSnapshot& system, SystemTelemetry& counters);
    void DrawCoreGrid(GUI::Frame& ui, std::span<const CoreTelemetry> cores, SystemTelemetry& counters);
    void DrawCoreCell(GUI::Frame& ui, const CoreTelemetry& core, size_t cell, float cellWidth, float advance, SystemTelemetry& counters);
    void DrawFrameTimeChart(GUI::Frame& ui, std::span<const float> samples, const FrameTelemetrySnapshot& stats);
    void DrawStageColumns(GUI::Frame& ui, std::span<const FrameStageRow> rows, const TableStyle& style, std::string_view prefix);
    void DrawStageColumn(GUI::Frame& ui, std::span<const FrameStageRow> rows, const TableStyle& style, std::string_view prefix, size_t column);
    void DrawStageRow(GUI::Frame& ui, const FrameStageRow& row, const TableStyle& style, std::string_view prefix, size_t column, size_t index);
    void ToggleCapture();

    // `part` tells the row's boxes apart (0 name, 1 value, 2 spark box, 3+ spark
    // bars); `column` the two runs of rows.
    [[nodiscard]] auto TableId(std::string_view prefix, size_t column, size_t index, size_t part) -> std::string_view {
        char* out = std::format_to(idBuffer.data(), "{}-{}-{}-{}", prefix, column, index, part);
        return std::string_view {idBuffer.data(), static_cast<size_t>(out - idBuffer.data())};
    }
};

void BenchmarkViewer::Impl::Advance() noexcept {
    frameClock.SetPaused(!running || benchmark == nullptr);
    frameClock.Tick();

    if (benchmark == nullptr || !running) {
        frameClock.ResetFixedAccumulator();
        return;
    }

    const FixedStepBatch batch = frameClock.ConsumeFixedSteps(kFixedStep, static_cast<uint32_t>(kMaxTicksPerFrame));
    for (uint32_t step = 0; step < batch.steps; ++step) {
        benchmark->Step(kFixedStep);
    }

    if (batch.steps >= static_cast<uint32_t>(kMaxTicksPerFrame)) {
        // The host's frame was longer than the whole budget. Drop the backlog
        // instead of carrying it: a measuring tool that stutters the thing it
        // is measuring has stopped measuring it.
        frameClock.ResetFixedAccumulator();
    }
}

void BenchmarkViewer::Impl::BuildTableStyles(ZHLN::Engine& engine, std::span<const FrameStageRow> gpu, std::span<const FrameStageRow> cpu) {
    // The atlas the Context will draw with, so the columns are sized against the
    // font that is actually on screen rather than a guess at its metrics.
    const ZHLN::FontAtlas& font    = engine.GetFontAtlas();
    const ZHLN::FontAtlas* atlas   = (font.glyphCount > 0) ? &font : nullptr;
    const float            advance = AdvancePerChar(atlas, kTableFontSize);

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

void BenchmarkViewer::Impl::DrawBenchmarkPanel(GUI::Frame& ui) {
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
            const size_t shown = std::min(history.size(), kChartBars);
            if (shown > 0) {
                // The scale is the run's worst tick, floored so an empty history
                // cannot divide by zero: the window's own tallest tick is at most
                // that, so a bar scrolling out does not rescale the bars left in.
                float       worst   = std::max(metrics.maxTickTimeMs, 0.001f);
                const auto  samples = std::span<const float>(history).subspan(history.size() - shown);

                ui.Box(
                    "BenchmarkChart",
                    GUI::BoxConfig {.width = {.fixed = kPanelContentWidth}, .height = {.fixed = kChartHeight}, .color = kChartInk},
                    [&]() -> void {
                        GUI::Widget::BarStrip(ui, samples, GUI::Widget::BarStripStyle {
                            .width        = kPanelContentWidth,
                            .height       = kChartHeight,
                            .gap          = kBarGap,
                            .minBarHeight = kMinBarHeight,
                            .scale        = GUI::Widget::BarScale::OwnPeak,
                            .peakFloor    = worst,
                            .inkFor       = TickBarInk,
                            .inkUser      = &worst,
                            .idBase       = "BenchmarkBar",
                        });
                    }
                );
            }

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

void BenchmarkViewer::Impl::DrawFrameTimeChart(GUI::Frame& ui, std::span<const float> samples, const FrameTelemetrySnapshot& stats) {
    if (samples.empty()) {
        return;
    }

    // The chart is scaled to the frame time that is normal, not to the worst
    // one: a window holding one 200 ms stall would otherwise flatten every other
    // bar against the floor. Bars past the top of the scale are clipped, and the
    // lows they come from are printed directly above the chart. Green below the
    // 1% low, orange at or above it: the tail the percentiles are computed from
    // is the visible top of the chart.
    //
    // The strip draws its own box, so the chart's background goes on the box
    // behind it -- same width, same height, and the bars fill it exactly.
    ui.Box(
        "TelemetryChart", GUI::BoxConfig {.width = {.fixed = telemetryContent}, .height = {.fixed = kTelemetryChartHeight}, .color = kChartInk},
        [&]() -> void {
            GUI::Widget::BarStrip(ui, samples, GUI::Widget::BarStripStyle {
                .width        = telemetryContent,
                .height       = kTelemetryChartHeight,
                .gap          = kBarGap,
                .minBarHeight = kMinBarHeight,
                .scale        = GUI::Widget::BarScale::Shared,
                .sharedPeak   = std::max({stats.frameMsAvg * kChartScaleAverage, stats.frameMsLow1 * kChartScaleLows, 1.0f}),
                .threshold    = stats.frameMsLow1,
                .ink          = kFastBarInk,
                .hotInk       = kSlowBarInk,
                .idBase       = "tchart",
            });
        }
    );
}

void BenchmarkViewer::Impl::DrawStageColumns(GUI::Frame& ui, std::span<const FrameStageRow> rows, const TableStyle& style, std::string_view prefix) {
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
    GUI::Frame&                    ui,
    std::span<const FrameStageRow> rows,
    const TableStyle&              style,
    std::string_view               prefix,
    size_t                         column
) {
    if (rows.empty()) {
        return;
    }

    // Fit-height rather than ui.Column: a grow-height child inside a scroll
    // container reports content at least as tall as the container, which would
    // make the panel's scroll area look unscrollable however long the list is.
    ui.Box("", GUI::BoxConfig {.gap = 2.0f, .direction = GUI::Direction::Column}, [&]() -> void {
        for (size_t i = 0; i < rows.size(); ++i) {
            DrawStageRow(ui, rows[i], style, prefix, column, i);
        }
    });
}

void BenchmarkViewer::Impl::DrawStageRow(
    GUI::Frame&          ui,
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
    // A row the profiler has stopped hearing from is drawn as history: its name
    // and its number dimmed, because that number is the last one it had rather
    // than what the frame is paying now (see FrameStageRow::IsStale).
    const bool      stale    = row.IsStale();
    const JPH::Vec4 rowInk   = stale ? kStaleInk : (row.gpuStage ? kBodyInk : kCpuNameInk);
    const JPH::Vec4 valueInk = stale ? kStaleInk : kBodyInk;

    // The row is one hover target, so its card opens wherever on it the pointer
    // is rather than only over its number or its bars. Its id is past the bars'
    // own run (0, 1 and 2 are the cells, 3.. the bars), so hovering a bar and
    // hovering the row are the same element rather than two ids colliding.
    const std::string_view rowId = TableId(prefix, column, index, kStageSparkBars + 4);

    ui.Box(
        rowId, GUI::BoxConfig {.width = {.fixed = style.ColumnWidth()}, .gap = style.gap, .direction = GUI::Direction::Row, .alignCross = GUI::Alignment::Center},
        [&]() -> void {
            ui.Box(
                TableId(prefix, column, index, 0), GUI::BoxConfig {.width = {.fixed = style.nameWidth}, .alignCross = GUI::Alignment::Center},
                [&]() -> void { ui.Text(FitToWidth(row.name, style.nameWidth, style.advance), style.fontSize, rowInk); }
            );

            ui.Box(
                TableId(prefix, column, index, 1),
                GUI::BoxConfig {.width = {.fixed = style.valueWidth}, .alignMain = GUI::Alignment::End, .alignCross = GUI::Alignment::Center},
                [&]() -> void { ui.Text(std::format("{:.2f}", row.lastMs), style.fontSize, valueInk); }
            );

            // The sparkline is the metric's own history, scaled to the row's own
            // peak: the question a row answers is "is this metric spiking", and a
            // shared scale would flatten every stage that is not the heaviest one.
            // The scale has a floor, so a row whose samples are all noise draws
            // small bars rather than a full block that cannot move.
            const size_t blank = row.spark.size() - std::min(row.filled, row.spark.size());

            GUI::Widget::BarStrip(ui, row.spark, GUI::Widget::BarStripStyle {
                .width        = style.sparkWidth,
                .height       = kStageSparkHeight,
                .gap          = kSparkBarGap,
                .minBarHeight = kMinBarHeight,
                .scale        = GUI::Widget::BarScale::OwnPeak,
                .peakFloor    = kSparkFloorMs,
                .absentBars   = blank,
                .ink          = row.gpuStage ? kGpuSparkInk : kCpuSparkInk,
                .absentInk    = kChartInk,
                .idBase       = TableId(prefix, column, index, 2),
            });
        }
    );

    // What the row is, and what its number does not say: the peak of the window
    // the bars draw rather than only the sample it happens to end on, and -- when
    // the profiler has stopped hearing from the metric -- how long it has been.
    if (ui.IsPointerOver(rowId)) {
        GUI::Widget::SetHover(hover, rowId);

        float windowPeak = 0.0f;
        for (const float sample: row.spark) {
            windowPeak = std::max(windowPeak, sample);
        }

        GUI::Widget::AddHoverLine(hover, row.name);
        if (row.filled > 0) {
            GUI::Widget::AddHoverLine(
                hover, std::format("last {:.2f} ms   highest of the last {} frames {:.2f} ms", row.lastMs, row.filled, windowPeak)
            );
        } else {
            GUI::Widget::AddHoverLine(hover, std::format("last {:.2f} ms", row.lastMs));
        }
        if (stale) {
            GUI::Widget::AddHoverLine(hover, std::format("not reported for {} frames", row.quietTicks));
        }
    }
}

void BenchmarkViewer::Impl::DrawHardwareSection(GUI::Frame& ui, const SystemSnapshot& system, SystemTelemetry& counters) {
    // Nothing was read, and the reason is the only useful thing left to draw. It
    // is a note, not an error: a machine whose platform exposes no counters is
    // running the frames perfectly well, and the frame numbers above are
    // unaffected.
    if (!system.unavailableReason.empty()) {
        ui.Text(FitToWidth(std::format("Machine  {}", system.unavailableReason), telemetryContent, AdvanceAt(12.0f)), 12.0f, kCpuNameInk);
        return;
    }

    // What was read and where it came from, then the readings themselves: one
    // line each, carrying the value now and nothing else. The window behind that
    // value -- its average, its tails, its clock -- is the card the line opens,
    // which is where a panel stops being a column of numbers in three tenses.
    const std::string_view backend = counters.BackendName();
    {
        const std::string_view id = TableId("hw", 0, 0, 0);
        ui.Box(id, GUI::BoxConfig {.direction = GUI::Direction::Column}, [&]() -> void {
            ui.Text(FitToWidth(std::format("Machine  {}", backend.empty() ? std::string_view("no counters on this platform") : backend), telemetryContent, AdvanceAt(12.0f)), 12.0f, kTitleInk);
        });
        if (ui.IsPointerOver(id)) {
            GUI::Widget::SetHover(hover, id);
            GUI::Widget::AddHoverLine(
                hover, std::format("backend  {}", backend.empty() ? std::string("none compiled in for this platform") : std::string(backend))
            );
            GUI::Widget::AddHoverLine(hover, std::format("{} samples over {:.0f} s of host frames", system.samples, system.windowSeconds));
            GUI::Widget::AddHoverLine(hover, std::format("one reading every {} ms", SystemTelemetry::kSampleInterval.count()));
        }
    }

    {
        std::string line = "CPU";
        if (system.cpuUsagePct) {
            line.append(std::format("   usage {:.1f}%", system.cpuUsagePct->last));
        }
        if (system.cpuClockMHz) {
            line.append(std::format("   clock {:.2f} GHz", system.cpuClockMHz->last / 1000.0f));
        }

        const std::string_view id = TableId("hw", 0, 1, 0);
        ui.Box(id, GUI::BoxConfig {.direction = GUI::Direction::Column}, [&]() -> void {
            ui.Text(
                FitToWidth(line, telemetryContent, AdvanceAt(12.0f)), 12.0f,
                system.cpuUsagePct ? LoadInk(system.cpuUsagePct->last) : kCpuNameInk
            );
        });

        if (ui.IsPointerOver(id) && (system.cpuUsagePct || system.cpuClockMHz)) {
            GUI::Widget::SetHover(hover, id);
            if (system.cpuUsagePct) {
                GUI::Widget::AddHoverLine(hover, SeriesLine("cpu usage", *system.cpuUsagePct, 1.0f, 1, "%"));
            }
            if (system.cpuClockMHz) {
                GUI::Widget::AddHoverLine(hover, SeriesLine("cpu clock", *system.cpuClockMHz, 1000.0f, 2, " GHz"));
            }
        }
    }

    const std::span<const CoreTelemetry> cores = counters.Cores();
    if (!cores.empty()) {
        DrawCoreGrid(ui, cores, counters);
    }

    // The GPU's own counters. Unavailable is a line rather than a blank: on a
    // machine whose driver exposes no utilization or clock -- an integrated GPU,
    // or a vendor the probe could not match -- the panel says which it was.
    if (system.gpuUsagePct || system.gpuClockMHz) {
        // The device, its load and its clock on one line when they fit: the name
        // is what the reading is about. On a card whose name is the whole retail
        // model they do not fit, and the readings move to their own line rather
        // than being cut off beside the name -- one extra line is cheap, a load
        // printed as ".." is not a reading.
        const std::string name = system.gpuName.empty() ? std::string("device unknown") : system.gpuName;

        std::string readings;
        if (system.gpuUsagePct) {
            readings.append(std::format("   usage {:.1f}%", system.gpuUsagePct->last));
        }
        if (system.gpuClockMHz) {
            readings.append(std::format("   clock {:.2f} GHz", system.gpuClockMHz->last / 1000.0f));
        }

        const std::string      all     = std::format("GPU  {}{}", name, readings);
        const float            advance = AdvanceAt(12.0f);
        const JPH::Vec4        ink     = system.gpuUsagePct ? LoadInk(system.gpuUsagePct->last) : kTitleInk;
        const std::string_view id      = TableId("hw", 0, 2, 0);

        ui.Box(id, GUI::BoxConfig {.direction = GUI::Direction::Column}, [&]() -> void {
            if (FitsWidth(all, telemetryContent, advance)) {
                ui.Text(all, 12.0f, ink);
            } else {
                ui.Text(FitToWidth(std::format("GPU  {}", name), telemetryContent, advance), 12.0f, kTitleInk);
                ui.Text(FitToWidth(std::format("GPU{}", readings), telemetryContent, AdvanceAt(11.0f)), 11.0f, ink);
            }
        });

        if (ui.IsPointerOver(id)) {
            GUI::Widget::SetHover(hover, id);
            if (system.gpuUsagePct) {
                GUI::Widget::AddHoverLine(hover, SeriesLine("gpu usage", *system.gpuUsagePct, 1.0f, 1, "%"));
            }
            if (system.gpuClockMHz) {
                GUI::Widget::AddHoverLine(hover, SeriesLine("gpu clock", *system.gpuClockMHz, 1000.0f, 2, " GHz"));
            }
        }
    } else {
        const std::string_view reason =
            system.gpuUnavailableReason.empty() ? std::string_view("no utilization or clock counters are exposed on this machine")
                                                : std::string_view(system.gpuUnavailableReason);
        ui.Text(FitToWidth(std::format("GPU counters  {}", reason), telemetryContent, AdvanceAt(11.0f)), 11.0f, kCpuNameInk);
    }
}

void BenchmarkViewer::Impl::DrawCoreGrid(GUI::Frame& ui, std::span<const CoreTelemetry> cores, SystemTelemetry& counters) {
    if (cores.empty()) {
        return;
    }

    // How many cells fit across is what the core count decides, not a constant:
    // the grid aims for kCoreGridRows rows and widens from there, so a thread
    // count far past a desktop's makes shorter cells rather than a taller panel.
    const size_t cells     = std::clamp((cores.size() + kCoreGridRows - 1) / kCoreGridRows, kCoreCellsMin, kCoreCellsMax);
    const float  cellWidth = (telemetryContent - (kCoreCellGap * static_cast<float>(cells - 1))) / static_cast<float>(cells);

    // The advance at the cell's font size: a cell that trusts a ratio instead
    // draws its label over its neighbour's.
    const float advance = AdvanceAt(kCoreCellFontSize);

    for (size_t start = 0; start < cores.size(); start += cells) {
        const size_t count = std::min(cells, cores.size() - start);
        ui.Box(
            TableId("cgrid", 0, start, 0),
            GUI::BoxConfig {.width = {.fixed = telemetryContent}, .gap = kCoreCellGap, .direction = GUI::Direction::Row, .alignCross = GUI::Alignment::Start},
            [&]() -> void {
                for (size_t i = 0; i < count; ++i) {
                    DrawCoreCell(ui, cores[start + i], start + i, cellWidth, advance, counters);
                }
            }
        );
    }

    // A core whose clock the platform does not expose keeps its clock series
    // invalid rather than reporting a zero, which is a real clock reading on a
    // parked core. Worth one line, because it changes what the cells mean.
    size_t withClock = 0;
    size_t withUsage = 0;
    for (const CoreTelemetry& core: cores) {
        withClock += core.clockMHz ? 1u : 0u;
        withUsage += core.usagePct ? 1u : 0u;
    }

    if (withUsage < cores.size() || (withClock > 0 && withClock < cores.size())) {
        std::string note = std::format("{} cores   {} reporting usage", cores.size(), withUsage);
        if (withClock > 0 && withClock < cores.size()) {
            note.append(std::format("   {} with a clock", withClock));
        }
        ui.Text(FitToWidth(note, telemetryContent, AdvanceAt(10.0f)), 10.0f, kCpuNameInk);
    }
}

void BenchmarkViewer::Impl::DrawCoreCell(GUI::Frame& ui, const CoreTelemetry& core, size_t cell, float cellWidth, float advance, SystemTelemetry& counters) {
    const std::string_view id = TableId("core", 0, cell, 0);

    ui.Box(
        id,
        GUI::BoxConfig {.width = {.fixed = cellWidth}, .gap = 2.0f, .direction = GUI::Direction::Column, .alignCross = GUI::Alignment::Start},
        [&]() -> void {
            // "c07 91% 2.13G" -- the core, its usage now, and its clock -- is
            // what a cell says when it is wide enough to say it; below that the
            // history alone is drawn, because a truncated number is worse than
            // no number.
            const bool withLabel = core.usagePct.has_value() && (11.0f * advance) <= cellWidth;
            if (withLabel) {
                ui.Box(
                    TableId("core", 0, cell, 1),
                    GUI::BoxConfig {.width = {.fixed = cellWidth}, .gap = 3.0f, .direction = GUI::Direction::Row, .alignCross = GUI::Alignment::Center},
                    [&]() -> void {
                        ui.Text(std::format("c{:02}", core.index), kCoreCellFontSize, kCpuNameInk);
                        ui.Text(std::format("{:.0f}%", core.usagePct->last), kCoreCellFontSize, LoadInk(core.usagePct->last));
                        if (core.clockMHz) {
                            ui.Text(std::format("{:.2f}G", core.clockMHz->last / 1000.0f), kCoreCellFontSize, kCpuNameInk);
                        }
                    }
                );
            }

            // The history, on the machine's own scale rather than the core's:
            // these bars are read against each other -- which core has been
            // pinned -- and a per-core scale would draw an idle core and a
            // saturated one as the same shape.
            // A view into the core's window, oldest first, and shorter than the
            // cell while the window is still filling: the bars before it are the
            // floor, not readings.
            const std::span<const float> history = counters.CoreUsageHistory(core.index, kCoreSparkBars);
            const size_t                 blank   = kCoreSparkBars - history.size();

            // The bars before this core's first sample are a floor, not a
            // reading: the strip draws them in the dim ink, so a young machine
            // does not look idle and busy at once, and the rest take the load
            // ramp the cell's own percentage uses.
            GUI::Widget::BarStrip(ui, history, GUI::Widget::BarStripStyle {
                .width        = cellWidth,
                .height       = kCoreSparkHeight,
                .gap          = kCoreSparkBarGap,
                .minBarHeight = kMinBarHeight,
                .scale        = GUI::Widget::BarScale::Shared,
                .sharedPeak   = 100.0f,
                .absentBars   = blank,
                .absentInk    = kChartInk,
                .inkFor       = CoreBarInk,
                .idBase       = TableId("core", 0, cell, 2),
            });
        }
    );

    // The pointer over a cell opens this core's window: the same card the
    // machine's own lines open, for the core it is on. A cell has no room for
    // three tenses of a number, and the panel has no room for twelve of them.
    if (ui.IsPointerOver(id)) {
        GUI::Widget::SetHover(hover, id);
        if (core.usagePct) {
            GUI::Widget::AddHoverLine(hover, SeriesLine(std::format("core {:02} usage", core.index), *core.usagePct, 1.0f, 1, "%"));
        }
        if (core.clockMHz) {
            GUI::Widget::AddHoverLine(hover, SeriesLine("clock", *core.clockMHz, 1000.0f, 2, " GHz"));
        }
        if (hover.lines.empty()) {
            GUI::Widget::ResetHover(hover); // a core with neither series has nothing to open
        }
    }
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

void BenchmarkViewer::Impl::DrawTelemetryPanel(GUI::Frame& ui, ZHLN::Engine& engine) {
    const FrameTelemetrySnapshot&        stats    = telemetry.Snapshot();
    SystemTelemetry&                     system   = telemetry.System();
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
            GUI::Widget::ResetHover(hover); // this frame's card is set by whatever the pointer is over

            // The panel is taller than most windows once its tables grow (the
            // GPU list alone is 35 stages today). Rather than budgeting the
            // leftover height with a measured constant that goes stale whenever
            // a section grows, the panel scrolls as a whole; the framework's bar
            // on the right edge is the affordance, and it tracks whatever is
            // below the fold.
            const float panelScrollH =
                std::max(240.0f, static_cast<float>(viewport.height) - (2.0f * kPanelMargin) - (2.0f * kPanelPadding));
            ui.ScrollArea(
                "TelemetryPanelScroll",
                GUI::BoxConfig {
                    .width     = {.grow = 1.0f},
                    .height    = {.fixed = panelScrollH},
                    .gap       = 6.0f,
                    .direction = GUI::Direction::Column,
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

                // What the capture button writes, for the pointer that asks: the
                // button is named for the file, and the file's path is not worth a
                // line of the panel for the rest of the session.
                const std::string_view captureLabel = telemetry.IsCapturing() ? std::string_view("Stop capture") : std::string_view("Capture CSV");
                if (ui.IsPointerOver(captureLabel)) {
                    GUI::Widget::SetHover(hover, captureLabel);
                    GUI::Widget::AddHoverLine(hover, "one CSV row per frame: the frame columns, then the machine columns");
                    GUI::Widget::AddHoverLine(hover, "written under build/cache/captures/");
                }

                if (telemetry.IsCapturing()) {
                    ui.Text(std::format("capturing {}   ({} frames)", telemetry.CapturePath().filename().string(), telemetry.CapturedFrames()), 11.0f, kFastBarInk);
                } else if (!telemetry.CapturePath().empty()) {
                    ui.Text(
                        std::format("last capture {}   ({} frames)", telemetry.CapturePath().filename().string(), telemetry.CapturedFrames()), 11.0f, kCpuNameInk
                    );
                }

                // The machine the frames ran on, between the host's own numbers and
                // the profiler's tables: the frame text above says how fast, these
                // say what the machine was busy with while it was fast.
                DrawHardwareSection(ui, system.Snapshot(), system);

                ui.Text(std::format("GPU  {:.2f} ms total   ({} stages)", stats.gpuTotalMs, gpuCount), 13.0f, kTitleInk);
                DrawStageColumns(ui, stages.first(gpuCount), tableGpu, "g");

                if (stages.size() > gpuCount) {
                    const size_t cpuCount = stages.size() - gpuCount;

                    // The scope list has no natural bound -- it grows with every
                    // registered scope -- but the panel's own scroll area now owns
                    // the viewport budget, so the rows just flow and the panel
                    // scrolls when they overflow. Rows go in directly rather than
                    // through ui.Column for the same reason as the GPU table's.
                    ui.Text(std::format("CPU  ({} scopes)", cpuCount), 13.0f, kTitleInk);
                    for (size_t i = 0; i < cpuCount; ++i) {
                        DrawStageRow(ui, stages[gpuCount + i], tableCpu, "c", 0, i);
                    }
                }

                }
            );

            // Last of all, so that it is on top of everything above it: out of
            // the layout flow, beside the panel it belongs to.
            GUI::Widget::DrawHoverCard(ui, hover, viewport, CardStyle(), kTelemetryPanelId);
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
    const ZHLN::EngineConfig runConfig {
        .appName = "Zahlen Benchmark Viewer",
    };

    BenchmarkViewer* self = this;
    ZHLN::Clock      uiClock;
    m_impl->pendingEntities  = initialEntities;
    m_impl->visible          = true;
    m_impl->telemetryVisible = true; // the standalone tool is the window for both panels

    auto ran = ZHLN::Engine::Run(
        runConfig,
        [self, &uiClock](ZHLN::Engine& engine) -> void { self->UpdateAndRenderUI(engine, uiClock.Tick()); }
    );

    // The engine is gone, and with it the scheduler the benchmark's World waits
    // on at teardown (see the lifetime note in the header). Release the
    // benchmark here, while that is the only thing left to do, rather than
    // leaving a World for whoever destroys the viewer afterwards. The viewer is
    // empty again and a later Attach() re-arms it.
    m_impl->benchmark.reset();
    m_impl->frameClock.ResetFixedAccumulator();

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

    m_impl->frameClock.Reset();
    ZHLN::Log("[Bench] viewer attached to the engine: {} entities, panel {}.", initialEntities, m_impl->visible ? "shown" : "hidden");
}

void BenchmarkViewer::Tick(ZHLN::Engine& engine) {
    const uint64_t frameId = engine.GetCurrentFrame();
    if (m_impl->frameTicked && frameId == m_impl->lastFrameId) {
        return; // this engine frame has been ticked already: one frame, one sample
    }
    m_impl->frameTicked = true;
    m_impl->lastFrameId = frameId;

    // Which device the frames are being drawn on, told to the telemetry before
    // it has taken its first sample: the platform probe matches its GPU counters
    // to that device rather than to whichever adapter the machine lists first,
    // and the probe reads the name when it is built, on that first sample. Taken
    // once -- RenderContext::GetInfo assembles its struct on each call, and the
    // device does not change under a running viewer -- but retried until a name
    // actually appears, so a host that ticked before its device was up is still
    // matched on the frame it comes up.
    if (!m_impl->gpuNameTaken) {
        if (const std::string_view device = engine.GetRenderContext().GetInfo().gpuName; !device.empty()) {
            m_impl->gpuNameTaken = true;
            m_impl->telemetry.SetGpuNameHint(device);
        }
    }

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

    m_impl->gui.SetFontAtlas(engine.GetFontAtlas());
    GUI::Frame ui = m_impl->gui.BeginFrame(engine.ConsumeUIInput(dt));
    if (m_impl->visible && m_impl->benchmark != nullptr) {
        m_impl->DrawBenchmarkPanel(ui);
    }
    if (m_impl->telemetryVisible) {
        m_impl->DrawTelemetryPanel(ui, engine);
    }
    engine.SubmitUI(std::move(ui).End());
}

void BenchmarkViewer::DrawPanel(GUI::Frame& ui, ZHLN::Engine& engine) {
    if (m_impl->benchmark == nullptr) {
        Attach(engine, m_impl->pendingEntities);
    }
    Tick(engine);
    if (m_impl->benchmark != nullptr) {
        m_impl->DrawBenchmarkPanel(ui);
    }
}

void BenchmarkViewer::DrawTelemetryPanel(GUI::Frame& ui, ZHLN::Engine& engine) {
    Tick(engine);
    m_impl->DrawTelemetryPanel(ui, engine);
}

void BenchmarkViewer::DrawPanels(GUI::Frame& ui, ZHLN::Engine& engine) {
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
        m_impl->frameClock.ResetFixedAccumulator();
    }
    m_impl->running = running;
    m_impl->frameClock.SetPaused(!running);
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
