// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestGUIWidgets.cpp
//
// Behavioural tests for the three primitives the native GUI was missing on the
// way to replacing ImGui:
//
//   - ui.Gradient: a band of colour stops, the geometry behind a colour
//     picker's hue strip and saturation/value plane.
//   - ui.PlotLines / ui.Histogram: rolling series for profilers and meters.
//   - ui.ColorEdit3 / ui.ColorEdit4: a swatch plus a picker popup, and the
//     HSV <-> RGB sync that keeps the picker honest when a colour is changed
//     from outside the widget.
//
// The vertex emitters are tested directly against a hand-written rect rather
// than through a full layout pass: `computedAbs*` is the only thing they read,
// so standing it in isolates the geometry from yoga and makes an expected
// coordinate arithmetic rather than a snapshot.
//
// Everything here is public API only (Zahlen/GUI.hpp + Zahlen/Components.hpp),
// matching the tests/ tree rule that engine internals stay out of tests.

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace {

using ZHLN::Entity;
using ZHLN::ECS::Registry;
namespace GUI    = ZHLN::GUI;
using Comp       = ZHLN::Components;
using UIComp     = ZHLN::GUI::UIComponents;

constexpr float kEps = 0.01f;

[[nodiscard]] auto Near(float actual, float expected) -> bool {
    return std::fabs(actual - expected) < kEps;
}

// Finds a cached child of `parent` by widget name, the way the engine's own
// compound widgets are keyed ("_ce_swatch", "_sl_track", ...).
[[nodiscard]] auto FindChildNamed(Registry& reg, Entity parent, std::string_view name) -> Entity {
    const auto* cache = reg.Get<UIComp::UIChildCacheComponent>(parent);
    if (cache == nullptr) {
        return Entity::Null();
    }
    Entity found = Entity::Null();
    cache->children.ForEach([&](uint64_t, const UIComp::UIChildCacheComponent::ChildRecord& rec) -> void {
        if (found != Entity::Null() || !reg.IsAlive(rec.entity)) {
            return;
        }
        if (const auto* n = reg.Get<Comp::NameComponent>(rec.entity)) {
            if (std::string_view(n->name) == name) {
                found = rec.entity;
            }
        }
    });
    return found;
}

// The first live entity carrying `Comp`. Used for the picker's own widgets,
// which are root-cached rather than parented under the field.
template <typename C>
[[nodiscard]] auto FindFirstWith(Registry& reg) -> Entity {
    for (Entity e: reg.GetEntitiesWith<C>()) {
        return e;
    }
    return Entity::Null();
}

// Gives a rect an explicit on-screen box, standing in for a layout pass.
void SetComputedRect(Registry& reg, Entity e, float x0, float y0, float x1, float y1) {
    reg.Patch<UIComp::UIRectComponent>(e, [&](auto& r) -> auto {
        r.computedAbsMinX = x0;
        r.computedAbsMinY = y0;
        r.computedAbsMaxX = x1;
        r.computedAbsMaxY = y1;
    });
}

// An empty rect with an explicit box, for the emitters (which take the rect by
// reference and never look at the registry).
[[nodiscard]] auto MakeRect(float x0, float y0, float x1, float y1) -> UIComp::UIRectComponent {
    UIComp::UIRectComponent r {};
    r.computedAbsMinX = x0;
    r.computedAbsMinY = y0;
    r.computedAbsMaxX = x1;
    r.computedAbsMaxY = y1;
    return r;
}

struct Geometry {
    std::vector<ZHLN::VertexPosition>   positions;
    std::vector<ZHLN::VertexAttributes> attributes;
};

// Runs an emitter with a comfortably oversized scratch buffer and returns only
// the vertices it actually wrote, so a test can assert on coordinates without
// re-deriving the count first.
[[nodiscard]] auto EmitGradient(const UIComp::UIRectComponent& rect, const UIComp::UIGradientComponent& g) -> Geometry {
    Geometry out {};
    out.positions.resize(256);
    out.attributes.resize(256);
    const uint32_t written = GUI::AppendGradientVertices(out.positions.data(), out.attributes.data(), rect, g);
    out.positions.resize(written);
    out.attributes.resize(written);
    return out;
}

[[nodiscard]] auto EmitPlot(const UIComp::UIRectComponent& rect, const UIComp::UIPlotComponent& p) -> Geometry {
    Geometry out {};
    out.positions.resize(8192);
    out.attributes.resize(8192);
    const uint32_t written = GUI::AppendPlotVertices(out.positions.data(), out.attributes.data(), rect, p);
    out.positions.resize(written);
    out.attributes.resize(written);
    return out;
}

// Bounding box of an emitted run, which is what most of these tests care
// about: where the geometry landed, not the winding of its triangles.
struct Extent {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

[[nodiscard]] auto BoundsOf(const Geometry& g) -> Extent {
    Extent e {};
    if (g.positions.empty()) {
        return e;
    }
    e.minX = e.maxX = g.positions[0].position[0];
    e.minY = e.maxY = g.positions[0].position[1];
    for (const auto& v: g.positions) {
        e.minX = std::min(e.minX, v.position[0]);
        e.maxX = std::max(e.maxX, v.position[0]);
        e.minY = std::min(e.minY, v.position[1]);
        e.maxY = std::max(e.maxY, v.position[1]);
    }
    return e;
}

// Publishes a test font the way the engine does, so ResolveFontTexture() has
// something to return.
void InstallFont(Registry& reg) {
    if (!reg.GetEntitiesWith<UIComp::UISettingsComponent>().empty()) {
        return;
    }
    ZHLN::FontAtlas font {};
    for (auto& g: font.glyphs) {
        g.x0       = 0.0f;
        g.y0       = 0.0f;
        g.x1       = 8.0f;
        g.y1       = 12.0f;
        g.xoff     = 0.0f;
        g.yoff     = -20.0f;
        g.xadvance = 10.0f;
    }
    reg.Create(UIComp::UISettingsComponent {.fontAtlas = font});
}

} // namespace

struct GUIWidgetsTestSuite {
    struct Tests {
        // ==================================================================
        // GRADIENT
        // ==================================================================

        auto gradient_stores_its_stops_and_axis() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            InstallFont(reg);

            const JPH::Vec4 stops[3] = {
                JPH::Vec4(1.0f, 0.0f, 0.0f, 1.0f), JPH::Vec4(0.0f, 1.0f, 0.0f, 1.0f), JPH::Vec4(0.0f, 0.0f, 1.0f, 1.0f)};

            Entity e = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 200.0f, .height = 100.0f}, [&]() -> void {
                    e = gui.Gradient(
                        "g", GUI::GradientConfig {.width = 100.0f, .height = 20.0f, .axis = GUI::UIGradientAxis::Vertical,
                             .stops = std::span<const JPH::Vec4>(stops, 3)}
                    );
                });
            }

            const auto* g = reg.Get<UIComp::UIGradientComponent>(e);
            ZHLN::Test::ExpectTrue(g != nullptr);
            if (g == nullptr) {
                return {};
            }

            ZHLN::Test::ExpectEq(g->stopCount, 3u);
            ZHLN::Test::ExpectTrue(g->axis == GUI::UIGradientAxis::Vertical);
            ZHLN::Test::ExpectTrue(Near(g->stops[0].GetX(), 1.0f) && Near(g->stops[0].GetY(), 0.0f));
            ZHLN::Test::ExpectTrue(Near(g->stops[2].GetZ(), 1.0f));

            // The stops are a per-frame visual, so rebuilding with fewer of
            // them must not leave stale colours past the new end.
            const JPH::Vec4 two[2] = {JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f), JPH::Vec4(1.0f, 1.0f, 1.0f, 1.0f)};
            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {.width = 200.0f, .height = 100.0f}, [&]() -> void {
                    e = gui.Gradient("g", GUI::GradientConfig {.width = 100.0f, .stops = std::span<const JPH::Vec4>(two, 2)});
                });
            }
            ZHLN::Test::ExpectEq(reg.Get<UIComp::UIGradientComponent>(e)->stopCount, 2u);
            return {};
        }

        auto gradient_emits_one_quad_per_stop_pair() -> std::expected<void, ZHLN::Error> {
            UIComp::UIGradientComponent g {};
            g.stopCount = 4;
            const UIComp::UIRectComponent rect = MakeRect(0.0f, 0.0f, 100.0f, 30.0f);

            ZHLN::Test::ExpectEq(GUI::CountGradientVertices(rect, g), 18u);
            ZHLN::Test::ExpectEq(EmitGradient(rect, g).positions.size(), std::size_t {18});

            // One stop is a flat fill with nothing to interpolate between, and
            // zero stops is a widget that has not been configured yet. Both
            // paint nothing rather than degenerating into a garbage quad.
            g.stopCount = 1;
            ZHLN::Test::ExpectEq(GUI::CountGradientVertices(rect, g), 0u);
            g.stopCount = 0;
            ZHLN::Test::ExpectEq(GUI::CountGradientVertices(rect, g), 0u);

            // A collapsed rect paints nothing either: the layout pass has not
            // run yet (or the widget was squeezed to zero), and emitting
            // degenerate triangles would just burn vertex budget.
            g.stopCount = 4;
            const UIComp::UIRectComponent empty = MakeRect(10.0f, 10.0f, 10.0f, 10.0f);
            ZHLN::Test::ExpectEq(GUI::CountGradientVertices(empty, g), 0u);
            return {};
        }

        auto gradient_spans_the_whole_rect_on_its_axis() -> std::expected<void, ZHLN::Error> {
            UIComp::UIGradientComponent g {};
            g.stopCount = 3;
            g.stops[0]  = JPH::Vec4(1.0f, 0.0f, 0.0f, 1.0f);
            g.stops[1]  = JPH::Vec4(0.0f, 1.0f, 0.0f, 1.0f);
            g.stops[2]  = JPH::Vec4(0.0f, 0.0f, 1.0f, 1.0f);

            const UIComp::UIRectComponent rect = MakeRect(20.0f, 40.0f, 120.0f, 70.0f);

            // Horizontal: fills the width, every quad is the full height.
            g.axis = GUI::UIGradientAxis::Horizontal;
            const Extent h = BoundsOf(EmitGradient(rect, g));
            ZHLN_CHECK(Near(h.minX, 20.0f) && Near(h.maxX, 120.0f), "horizontal gradient spans the width", "x=[{:.1f},{:.1f}]", h.minX, h.maxX);
            ZHLN_CHECK(Near(h.minY, 40.0f) && Near(h.maxY, 70.0f), "horizontal gradient fills the height", "y=[{:.1f},{:.1f}]", h.minY, h.maxY);

            // Vertical: the same rect, transposed.
            g.axis = GUI::UIGradientAxis::Vertical;
            const Extent v = BoundsOf(EmitGradient(rect, g));
            ZHLN_CHECK(Near(v.minX, 20.0f) && Near(v.maxX, 120.0f), "vertical gradient fills the width", "x=[{:.1f},{:.1f}]", v.minX, v.maxX);
            ZHLN_CHECK(Near(v.minY, 40.0f) && Near(v.maxY, 70.0f), "vertical gradient spans the height", "y=[{:.1f},{:.1f}]", v.minY, v.maxY);
            return {};
        }

        // ==================================================================
        // PLOT
        // ==================================================================

        auto plot_copies_the_series_and_the_range() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            InstallFont(reg);

            const std::array<float, 4> series = {1.0f, 2.0f, 3.0f, 4.0f};
            Entity                     e      = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 200.0f, .height = 100.0f}, [&]() -> void {
                    e = gui.PlotLines(
                        "p", std::span<const float>(series),
                        GUI::PlotConfig {.height = 40.0f, .minValue = 0.0f, .maxValue = 8.0f}
                    );
                });
            }

            const auto* p = reg.Get<UIComp::UIPlotComponent>(e);
            ZHLN::Test::ExpectTrue(p != nullptr);
            if (p == nullptr) {
                return {};
            }

            ZHLN::Test::ExpectEq(p->values.size(), std::size_t {4});
            ZHLN::Test::ExpectTrue(p->kind == GUI::UIPlotKind::Lines);
            ZHLN::Test::ExpectTrue(Near(p->minValue, 0.0f) && Near(p->maxValue, 8.0f));
            for (int i = 0; i < 4; ++i) {
                ZHLN::Test::ExpectTrue(Near(p->values[i], series[static_cast<std::size_t>(i)]));
            }

            // A ring buffer is normally handed over at a fixed size; the
            // widget must overwrite in place rather than reallocating.
            const std::array<float, 4> next = {5.0f, 6.0f, 7.0f, 8.0f};
            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {.width = 200.0f, .height = 100.0f}, [&]() -> void {
                    e = gui.PlotLines("p", std::span<const float>(next), GUI::PlotConfig {.height = 40.0f});
                });
            }
            ZHLN::Test::ExpectTrue(Near(reg.Get<UIComp::UIPlotComponent>(e)->values[0], 5.0f));
            return {};
        }

        auto plot_emits_a_bounded_number_of_vertices_per_kind() -> std::expected<void, ZHLN::Error> {
            UIComp::UIPlotComponent p {};
            p.values.resize(4);
            p.values[0] = 0.0f;
            p.values[1] = 1.0f;
            p.values[2] = 0.5f;
            p.values[3] = 1.0f;
            p.maxValue  = 1.0f;

            // Lines: one quad (6 vertices) per segment, so n-1 of them.
            p.kind = GUI::UIPlotKind::Lines;
            ZHLN::Test::ExpectEq(GUI::CountPlotVertices(p), 18u);

            // ShadedLines: the same polyline plus a fill quad per segment.
            p.kind = GUI::UIPlotKind::ShadedLines;
            ZHLN::Test::ExpectEq(GUI::CountPlotVertices(p), 36u);

            // Histogram: one bar per sample, not per gap between samples.
            p.kind = GUI::UIPlotKind::Histogram;
            ZHLN::Test::ExpectEq(GUI::CountPlotVertices(p), 24u);

            // A single sample has no segment to draw as a line but is still a
            // perfectly good one-bar histogram.
            p.values.resize(1);
            ZHLN::Test::ExpectEq(GUI::CountPlotVertices(p), 6u);
            p.kind = GUI::UIPlotKind::Lines;
            ZHLN::Test::ExpectEq(GUI::CountPlotVertices(p), 0u);
            return {};
        }

        auto plot_lines_map_the_range_onto_the_widget_height() -> std::expected<void, ZHLN::Error> {
            UIComp::UIPlotComponent p {};
            p.values.resize(2);
            p.values[0] = 0.0f; // bottom edge
            p.values[1] = 1.0f; // top edge
            p.maxValue  = 1.0f;
            p.kind      = GUI::UIPlotKind::Lines;

            // A 100px tall widget spanning y=200..300: value 0 lands on the
            // BOTTOM (300) and value 1 on the top (200), because screen Y grows
            // downward and a plot reads bottom-up.
            const UIComp::UIRectComponent rect = MakeRect(0.0f, 200.0f, 100.0f, 300.0f);
            const Geometry                geo  = EmitPlot(rect, p);
            ZHLN::Test::ExpectEq(geo.positions.size(), std::size_t {6});

            // A stroked line is a quad, so it overhangs its endpoints by half
            // its own thickness — that is the stroke, not a mis-mapped value.
            const float half = std::max(0.5f, p.lineWidth * 0.5f) + kEps;
            const Extent b   = BoundsOf(geo);
            ZHLN_CHECK(Near(b.minY, 200.0f) || (b.minY > 200.0f - half && b.minY < 200.0f + half), "the series maximum reaches the top edge", "minY={:.2f} (tolerance {:.2f})", b.minY, half);
            ZHLN_CHECK(Near(b.maxY, 300.0f) || (b.maxY > 300.0f - half && b.maxY < 300.0f + half), "the series minimum reaches the bottom edge", "maxY={:.2f} (tolerance {:.2f})", b.maxY, half);
            return {};
        }

        auto plot_clamps_samples_outside_the_range() -> std::expected<void, ZHLN::Error> {
            UIComp::UIPlotComponent p {};
            p.values.resize(2);
            p.values[0] = -5.0f; // far below the range
            p.values[1] = 5.0f;  // far above it
            p.minValue  = 0.0f;
            p.maxValue  = 1.0f;
            p.kind      = GUI::UIPlotKind::Lines;

            const UIComp::UIRectComponent rect = MakeRect(0.0f, 0.0f, 100.0f, 100.0f);
            const Extent                  b    = BoundsOf(EmitPlot(rect, p));

            // A spike that overshoots the range is clamped to the edge, not
            // dropped: punching a hole in the strip would make the profiler
            // look like it had gaps in its data. Tolerance is the stroke
            // thickness, for the same reason as the mapping test above.
            const float half = std::max(0.5f, p.lineWidth * 0.5f) + kEps;
            ZHLN_CHECK(b.minY < half, "an overshooting sample pins to the top", "minY={:.2f}", b.minY);
            ZHLN_CHECK(b.maxY > 100.0f - half, "an undershooting sample pins to the bottom", "maxY={:.2f}", b.maxY);
            return {};
        }

        auto plot_paints_nothing_when_the_range_is_degenerate() -> std::expected<void, ZHLN::Error> {
            UIComp::UIPlotComponent p {};
            p.values.resize(3);
            p.values[0] = 0.5f;
            p.values[1] = 0.5f;
            p.values[2] = 0.5f;
            p.minValue  = 1.0f;
            p.maxValue  = 1.0f; // min == max: divide by zero

            const UIComp::UIRectComponent rect = MakeRect(0.0f, 0.0f, 100.0f, 100.0f);
            // CountPlotVertices is an upper bound on a well-formed plot; the
            // emitter is what has to bail out on the degenerate range.
            ZHLN::Test::ExpectEq(GUI::CountPlotVertices(p), 12u);
            ZHLN_CHECK(EmitPlot(rect, p).positions.empty(), "a zero-width range emits nothing rather than NaN geometry", "", "");

            // Every kind has to bail out the same way, not just the default.
            p.kind = GUI::UIPlotKind::Histogram;
            ZHLN_CHECK(EmitPlot(rect, p).positions.empty(), "histograms ignore a zero-width range too", "", "");
            return {};
        }

        auto histogram_bars_grow_from_the_zero_line_for_a_signed_range() -> std::expected<void, ZHLN::Error> {
            UIComp::UIPlotComponent p {};
            p.kind     = GUI::UIPlotKind::Histogram;
            p.minValue = -1.0f;
            p.maxValue = 1.0f;
            p.values.resize(2);
            p.values[0] = -0.5f;
            p.values[1] = 0.5f;

            // y=0 sits at the vertical middle of a 100px widget starting at 0.
            const UIComp::UIRectComponent rect = MakeRect(0.0f, 0.0f, 100.0f, 100.0f);
            const Geometry                geo  = EmitPlot(rect, p);
            ZHLN::Test::ExpectEq(geo.positions.size(), std::size_t {12});

            // Screen Y grows downward, so "above the zero line" means a SMALLER
            // y. Bar 0 (-0.5) hangs from the zero line at y=50 down to y=75;
            // bar 1 (+0.5) rises from y=50 up to y=25. Neither is anchored to
            // the bottom edge, which is what a signed series (a
            // delta-from-target meter) needs in order to read correctly.
            float bar0MinY = geo.positions[0].position[1];
            float bar0MaxY = geo.positions[0].position[1];
            for (int i = 0; i < 6; ++i) {
                bar0MinY = std::min(bar0MinY, geo.positions[static_cast<std::size_t>(i)].position[1]);
                bar0MaxY = std::max(bar0MaxY, geo.positions[static_cast<std::size_t>(i)].position[1]);
            }
            ZHLN_CHECK(Near(bar0MinY, 50.0f) && Near(bar0MaxY, 75.0f), "a negative bar hangs below the zero line", "y=[{:.1f},{:.1f}]", bar0MinY, bar0MaxY);

            float bar1MinY = geo.positions[6].position[1];
            float bar1MaxY = geo.positions[6].position[1];
            for (int i = 6; i < 12; ++i) {
                bar1MinY = std::min(bar1MinY, geo.positions[static_cast<std::size_t>(i)].position[1]);
                bar1MaxY = std::max(bar1MaxY, geo.positions[static_cast<std::size_t>(i)].position[1]);
            }
            ZHLN_CHECK(Near(bar1MinY, 25.0f) && Near(bar1MaxY, 50.0f), "a positive bar rises above the zero line", "y=[{:.1f},{:.1f}]", bar1MinY, bar1MaxY);
            return {};
        }

        auto histogram_stays_inside_the_widget_rect() -> std::expected<void, ZHLN::Error> {
            UIComp::UIPlotComponent p {};
            p.kind     = GUI::UIPlotKind::Histogram;
            p.maxValue = 1.0f;
            p.values.resize(8);
            for (std::size_t i = 0; i < 8; ++i) {
                p.values[i] = static_cast<float>(i) / 7.0f;
            }

            const UIComp::UIRectComponent rect = MakeRect(10.0f, 10.0f, 210.0f, 60.0f);
            const Extent                  b    = BoundsOf(EmitPlot(rect, p));

            // Eight bars across 200px with a 1px gap: the run has to stay
            // inside the widget on every side. The gap insets the first and
            // last bars by half a gap, and a zero-valued sample (the first bar
            // here) is drawn as a 1px sliver — which is the case that used to
            // poke a fringe out past the bottom edge.
            ZHLN_CHECK(b.minX >= 10.0f - kEps && b.maxX <= 210.0f + kEps, "bars stay within the widget width", "x=[{:.1f},{:.1f}]", b.minX, b.maxX);
            ZHLN_CHECK(b.minY >= 10.0f - kEps && b.maxY <= 60.0f + kEps, "bars stay within the widget height", "y=[{:.1f},{:.1f}]", b.minY, b.maxY);
            return {};
        }

        // ==================================================================
        // HSV <-> RGB
        // ==================================================================

        auto hsv_to_rgb_hits_the_primaries() -> std::expected<void, ZHLN::Error> {
            const JPH::Vec4 red   = GUI::HsvToRgb(0.0f, 1.0f, 1.0f);
            const JPH::Vec4 green = GUI::HsvToRgb(120.0f, 1.0f, 1.0f);
            const JPH::Vec4 blue  = GUI::HsvToRgb(240.0f, 1.0f, 1.0f);

            ZHLN_CHECK(Near(red.GetX(), 1.0f) && Near(red.GetY(), 0.0f) && Near(red.GetZ(), 0.0f), "hue 0 is red", "rgb=({:.2f},{:.2f},{:.2f})", red.GetX(), red.GetY(), red.GetZ());
            ZHLN_CHECK(Near(green.GetX(), 0.0f) && Near(green.GetY(), 1.0f) && Near(green.GetZ(), 0.0f), "hue 120 is green", "rgb=({:.2f},{:.2f},{:.2f})", green.GetX(), green.GetY(), green.GetZ());
            ZHLN_CHECK(Near(blue.GetX(), 0.0f) && Near(blue.GetY(), 0.0f) && Near(blue.GetZ(), 1.0f), "hue 240 is blue", "rgb=({:.2f},{:.2f},{:.2f})", blue.GetX(), blue.GetY(), blue.GetZ());

            // 360 is the same colour as 0: the hue strip's last stop wraps.
            const JPH::Vec4 wrapped = GUI::HsvToRgb(360.0f, 1.0f, 1.0f);
            ZHLN_CHECK(Near(wrapped.GetX(), 1.0f) && Near(wrapped.GetZ(), 0.0f), "hue 360 wraps to red", "rgb=({:.2f},{:.2f},{:.2f})", wrapped.GetX(), wrapped.GetY(), wrapped.GetZ());
            return {};
        }

        auto rgb_to_hsv_round_trips_a_colour() -> std::expected<void, ZHLN::Error> {
            const JPH::Vec4 original = GUI::HsvToRgb(210.0f, 0.6f, 0.8f);

            float hue = 0.0f;
            float sat = 0.0f;
            float val = 0.0f;
            GUI::RgbToHsv(original, hue, sat, val);

            ZHLN_CHECK(Near(hue, 210.0f), "hue survives the round trip", "hue={:.1f}", hue);
            ZHLN_CHECK(Near(sat, 0.6f), "saturation survives the round trip", "sat={:.3f}", sat);
            ZHLN_CHECK(Near(val, 0.8f), "value survives the round trip", "val={:.3f}", val);
            return {};
        }

        auto rgb_to_hsv_keeps_the_hue_of_a_grey() -> std::expected<void, ZHLN::Error> {
            // Hue is undefined at the grey axis. Keeping the previous hue is
            // what stops "drag saturation to zero and back" from recolouring a
            // grey to red — which is what a naive max-channel derivation does.
            float hue = 200.0f;
            float sat = 0.0f;
            float val = 0.0f;
            GUI::RgbToHsv(JPH::Vec4(0.5f, 0.5f, 0.5f, 1.0f), hue, sat, val);

            ZHLN_CHECK(Near(hue, 200.0f), "an achromatic colour keeps the previous hue", "hue={:.1f}", hue);
            ZHLN_CHECK(Near(sat, 0.0f), "a grey has no saturation", "sat={:.3f}", sat);
            ZHLN_CHECK(Near(val, 0.5f), "a grey's value is its luminance", "val={:.3f}", val);
            return {};
        }

        // ==================================================================
        // COLOREDIT
        // ==================================================================

        auto color_edit_builds_a_label_and_a_swatch() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            InstallFont(reg);

            float  col[3] = {0.25f, 0.5f, 0.75f};
            Entity e      = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }

            const Entity label  = FindChildNamed(reg, e, "_ce_label");
            const Entity swatch = FindChildNamed(reg, e, "_ce_swatch");
            ZHLN::Test::ExpectTrue(label != Entity::Null());
            ZHLN::Test::ExpectTrue(swatch != Entity::Null());
            if (swatch == Entity::Null()) {
                return {};
            }

            const auto* text = reg.Get<UIComp::TextComponent>(label);
            ZHLN::Test::ExpectTrue(text != nullptr && std::string_view(text->text) == "Tint");

            const auto* panel = reg.Get<UIComp::UIPanelComponent>(swatch);
            ZHLN::Test::ExpectTrue(panel != nullptr);
            if (panel == nullptr) {
                return {};
            }
            ZHLN::Test::ExpectTrue(Near(panel->color.GetX(), 0.25f));
            ZHLN::Test::ExpectTrue(Near(panel->color.GetY(), 0.5f));
            ZHLN::Test::ExpectTrue(Near(panel->color.GetZ(), 0.75f));

            // The picker is closed until the field is clicked.
            ZHLN::Test::ExpectTrue(FindFirstWith<UIComp::UIColorSVComponent>(reg) == Entity::Null());
            return {};
        }

        auto color_edit_swatch_follows_an_external_change() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            InstallFont(reg);

            float  col[3] = {1.0f, 0.0f, 0.0f};
            Entity e      = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }

            // A preset, an undo, or another panel editing the same material:
            // the widget is not the only writer, and the swatch has to follow.
            col[0] = 0.0f;
            col[1] = 1.0f;
            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }

            const Entity swatch = FindChildNamed(reg, e, "_ce_swatch");
            const auto*  panel  = reg.Get<UIComp::UIPanelComponent>(swatch);
            ZHLN::Test::ExpectTrue(panel != nullptr);
            if (panel == nullptr) {
                return {};
            }
            ZHLN_CHECK(Near(panel->color.GetX(), 0.0f) && Near(panel->color.GetY(), 1.0f), "the swatch tracks an external edit", "rgb=({:.2f},{:.2f},{:.2f})", panel->color.GetX(), panel->color.GetY(), panel->color.GetZ());
            return {};
        }

        auto color_edit_opens_the_picker_on_click() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            InstallFont(reg);

            float  col[3] = {0.2f, 0.4f, 0.8f};
            Entity e      = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }

            // ConsumeClick reads the flag the interaction pass sets, so a test
            // can drive the gesture without a window or a real pointer.
            auto* btn = reg.Get<UIComp::UIButtonComponent>(e);
            ZHLN::Test::ExpectTrue(btn != nullptr);
            if (btn == nullptr) {
                return {};
            }
            btn->Set(GUI::UIButton::Clicked, true);

            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }

            ZHLN::Test::ExpectTrue(reg.Get<UIComp::UIColorEditComponent>(e)->expanded);

            // The saturation/value plane exists only while the picker is open.
            const Entity sv = FindFirstWith<UIComp::UIColorSVComponent>(reg);
            ZHLN::Test::ExpectTrue(sv != Entity::Null());

            // Clicking the field again closes it, and the plane goes with it.
            reg.Get<UIComp::UIButtonComponent>(e)->Set(GUI::UIButton::Clicked, true);
            {
                GUI::Context gui(reg, 3);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }
            ZHLN::Test::ExpectFalse(reg.Get<UIComp::UIColorEditComponent>(e)->expanded);
            ZHLN::Test::ExpectTrue(FindFirstWith<UIComp::UIColorSVComponent>(reg) == Entity::Null());
            return {};
        }

        auto color_edit_picker_derives_hsv_from_the_colour() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            InstallFont(reg);

            // A saturated blue: hue 240, full saturation and value.
            float  col[3] = {0.0f, 0.0f, 1.0f};
            Entity e      = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }
            reg.Get<UIComp::UIButtonComponent>(e)->Set(GUI::UIButton::Clicked, true);
            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }

            const auto* ce = reg.Get<UIComp::UIColorEditComponent>(e);
            ZHLN::Test::ExpectTrue(ce != nullptr);
            if (ce == nullptr) {
                return {};
            }
            ZHLN_CHECK(Near(ce->hue, 240.0f), "the picker's hue is derived from the colour", "hue={:.1f}", ce->hue);
            ZHLN_CHECK(Near(ce->sat, 1.0f), "the picker's saturation is derived from the colour", "sat={:.3f}", ce->sat);
            ZHLN_CHECK(Near(ce->val, 1.0f), "the picker's value is derived from the colour", "val={:.3f}", ce->val);

            // The plane is tinted from that hue, so a second picker on a
            // different colour cannot inherit the first one's.
            const auto* sv = reg.Get<UIComp::UIColorSVComponent>(FindFirstWith<UIComp::UIColorSVComponent>(reg));
            ZHLN::Test::ExpectTrue(sv != nullptr && Near(sv->hue, 240.0f));

            // And the colour survives the open/close cycle untouched: opening
            // a picker must not nudge the value it is showing.
            ZHLN_CHECK(Near(col[0], 0.0f) && Near(col[2], 1.0f), "opening the picker does not alter the colour", "rgb=({:.3f},{:.3f},{:.3f})", col[0], col[1], col[2]);
            return {};
        }

        auto color_edit4_exposes_the_alpha_row() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            InstallFont(reg);

            JPH::Vec4 col {0.1f, 0.2f, 0.3f, 0.4f};
            Entity    e = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit4("tint", "Tint", col);
                });
            }
            reg.Get<UIComp::UIButtonComponent>(e)->Set(GUI::UIButton::Clicked, true);
            {
                GUI::Context gui(reg, 2);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit4("tint", "Tint", col);
                });
            }

            const auto* ce = reg.Get<UIComp::UIColorEditComponent>(e);
            ZHLN::Test::ExpectTrue(ce != nullptr);
            if (ce == nullptr) {
                return {};
            }
            ZHLN::Test::ExpectEq(ce->componentCount, 4);
            ZHLN::Test::ExpectTrue(Near(ce->value.GetW(), 0.4f));

            // ColorEdit3 pins alpha to 1 rather than leaving it at whatever
            // the array happened to contain.
            Registry reg2;
            InstallFont(reg2);
            float    rgb[3] = {0.1f, 0.2f, 0.3f};
            Entity   e2     = Entity::Null();
            {
                GUI::Context gui(reg2, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e2 = gui.ColorEdit3("tint", "Tint", rgb);
                });
            }
            ZHLN::Test::ExpectTrue(Near(reg2.Get<UIComp::UIColorEditComponent>(e2)->value.GetW(), 1.0f));
            return {};
        }

        auto color_edit_clamps_channels_into_zero_to_one() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            InstallFont(reg);

            float  col[3] = {5.0f, -2.0f, 0.5f};
            Entity e      = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel("root", GUI::PanelConfig {.width = 300.0f, .height = 200.0f}, [&]() -> void {
                    e = gui.ColorEdit3("tint", "Tint", col);
                });
            }

            ZHLN_CHECK(Near(col[0], 1.0f) && Near(col[1], 0.0f) && Near(col[2], 0.5f), "out-of-range channels are clamped", "rgb=({:.2f},{:.2f},{:.2f})", col[0], col[1], col[2]);
            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which aggregates
// every suite in this directory through Runner::RunDeferred.
auto RunGUIWidgetsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<GUIWidgetsTestSuite>();
}
