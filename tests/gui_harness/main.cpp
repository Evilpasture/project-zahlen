// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-only harness for ZHLN::GUI::Context button interaction.
//
// This links the REAL src/gui/GUIContext.cpp against the REAL extern/clay/clay.h
// (the submodule commit the repo pins). Nothing here re-implements or copies the
// widget logic: Context::Box / Context::Button / Context::IsItemHovered are the
// shipped functions, driven from a bare ZHLN::ECS::Registry with no Engine, no
// Vulkan and no window -- so the whole thing runs on a machine with no GPU.
//
// Usage:
//   gui_harness                        replay tests/render/TestUI.cpp::
//                                      button_click_interaction_and_states
//   gui_harness --probe                dump every widget's laid-out rect and
//                                      sweep the pointer across it
//   gui_harness --fault=<name>         inject one engine-side breakage and show
//                                      the failure signature it produces
//   gui_harness --viewport=WxH         override the layout viewport
//   gui_harness --font=<name>          install a UISettingsComponent font atlas
//                                      (fallback|realistic|nan|huge|zero)
//
// Build: tests/gui_harness/build.sh

#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <clay.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;
int g_checks   = 0;

void Expect(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("      [FAIL] %s\n", what);
    }
}

// Same id derivation Context::Button uses internally, so the harness can ask
// Clay about exactly the element the real Button declared.
Clay_ElementId WidgetId(std::string_view label) {
    const uint32_t idNum = static_cast<uint32_t>(ZHLN::HashCreativeWorkPath(label));
    return Clay_GetElementIdWithIndex(
        Clay_String {.isStaticallyAllocated = false, .length = static_cast<int32_t>(label.size()), .chars = label.data()}, idNum);
}

void DumpWidget(std::string_view label) {
    const Clay_ElementData d = Clay_GetElementData(WidgetId(label));
    if (!d.found) {
        std::printf("      %-12s : NOT FOUND (no layout produced yet)\n", std::string(label).c_str());
        return;
    }
    std::printf("      %-12s : x[%.1f, %.1f] y[%.1f, %.1f]  (%.1f x %.1f)\n", std::string(label).c_str(), d.boundingBox.x,
                d.boundingBox.x + d.boundingBox.width, d.boundingBox.y, d.boundingBox.y + d.boundingBox.height,
                d.boundingBox.width, d.boundingBox.height);
}

int32_t PointerOverCount() {
    return Clay_GetCurrentContext() == nullptr ? -1 : Clay_GetPointerOverIds().length;
}

struct ProbeResult {
    bool     wasClickedA = false;
    bool     hoveredA    = false;
    bool     activeA     = false;
    bool     wasClickedB = false;
    bool     hoveredB    = false;
    bool     activeB     = false;
    uint32_t clicksA     = 0;
    uint32_t hoversA     = 0;
    uint32_t clicksB     = 0;
};

// The UI body, mirroring TestUI.cpp's callback. EndFrame() instead of
// EndFrameAndRender(): both run Clay_EndLayout(), and hit-testing happens in
// Button() before either, so the interaction path is identical.
void BuildTestUI(ZHLN::GUI::Context& ui, ProbeResult& out) {
    ui.BeginFrame(0.016f);

    ui.Box(
        "Container",
        ZHLN::GUI::BoxConfig {
            .width     = {.fixed = 300.0f},
            .height    = {.fixed = 200.0f},
            .padding   = 20.0f,
            .gap       = 10.0f,
            .direction = ZHLN::GUI::Direction::Column
        },
        [&]() {
            out.wasClickedA = ui.Button("ButtonA", [&]() { out.clicksA++; }, [&]() { out.hoversA++; });
            out.hoveredA    = ui.IsItemHovered();
            out.activeA     = ui.IsItemActive();

            out.wasClickedB = ui.Button("ButtonB", [&]() { out.clicksB++; });
            out.hoveredB    = ui.IsItemHovered();
            out.activeB     = ui.IsItemActive();
        }
    );

    ui.EndFrame();
}

// Engine-side breakages, injected one at a time. Each prints the failure
// signature it produces, so a ctest failure can be matched to a cause.
enum class Fault {
    None,
    NoCallback,    // the host UI callback is never invoked
    NoInput,       // the GUI's GetSingleton<InputStateComponent>() finds nothing
    StaleMouse,    // mouse position never reaches InputStateComponent
    NoMouseButton, // mouse position arrives, LButton never does
};

const char* FaultName(Fault f) {
    switch (f) {
        case Fault::None: return "none";
        case Fault::NoCallback: return "NoCallback";
        case Fault::NoInput: return "NoInput";
        case Fault::StaleMouse: return "StaleMouse";
        case Fault::NoMouseButton: return "NoMouseButton";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    bool        probe   = false;
    uint32_t    vpW     = 640;
    uint32_t    vpH     = 480;
    const char* font    = "fallback";
    Fault       fault   = Fault::None;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--probe") == 0) {
            probe = true;
        } else if (std::strncmp(argv[i], "--viewport=", 11) == 0) {
            std::sscanf(argv[i] + 11, "%ux%u", &vpW, &vpH);
        } else if (std::strncmp(argv[i], "--font=", 7) == 0) {
            font = argv[i] + 7;
        } else if (std::strncmp(argv[i], "--fault=", 8) == 0) {
            const char* v = argv[i] + 8;
            if (std::strcmp(v, "none") == 0)                 fault = Fault::None;
            else if (std::strcmp(v, "nocallback") == 0)      fault = Fault::NoCallback;
            else if (std::strcmp(v, "noinput") == 0)         fault = Fault::NoInput;
            else if (std::strcmp(v, "stalemouse") == 0)      fault = Fault::StaleMouse;
            else if (std::strcmp(v, "nomousebutton") == 0)   fault = Fault::NoMouseButton;
            else { std::fprintf(stderr, "unknown --fault=%s\n", v); return 2; }
        }
    }

    ZHLN::ECS::Registry registry;

    // The real engine has a UISettingsComponent singleton carrying a baked SDF
    // font atlas; Context::BeginFrame then measures text with those glyph
    // advances instead of the built-in 18px fallback. Text metrics decide the
    // button's fit-to-content width, so this is part of the real geometry.
    if (std::strcmp(font, "fallback") != 0) {
        auto& settings = registry.GetOrEmplaceSingleton<ZHLN::GUI::UIComponents::UISettingsComponent>();
        for (int i = 0; i < 96; ++i) {
            float adv = 14.0f; // ~DejaVuSans baked at 32px
            if (std::strcmp(font, "nan") == 0)  adv = std::nanf("");
            if (std::strcmp(font, "huge") == 0) adv = 1.0e9f;
            if (std::strcmp(font, "zero") == 0) adv = 0.0f;
            settings.fontAtlas.glyphs[i] =
                ZHLN::GlyphMetric {.x0 = 0, .y0 = 0, .x1 = 16, .y1 = 32, .xoff = 0, .yoff = 0, .xadvance = adv};
        }
    }

    ProbeResult out;
    ZHLN::GUI::Context ui(registry, ZHLN::Extent2D {vpW, vpH});

    // Exactly what TestUI.cpp's setInput does: re-query the singleton every
    // time, never hold a reference across a frame.
    auto setInput = [&](float mx, float my, bool lbutton) {
        auto& in = registry.GetOrEmplaceSingleton<ZHLN::Components::InputStateComponent>();
        if (fault == Fault::StaleMouse) {
            in.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), lbutton);
            return; // position write lost
        }
        in.mouseX = mx;
        in.mouseY = my;
        if (fault != Fault::NoMouseButton) {
            in.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), lbutton);
        }
    };

    std::printf("== GUI button harness ==\n");
    std::printf("viewport=%ux%u  font=%s  injected fault=%s\n", vpW, vpH, font, FaultName(fault));
    std::printf("InputStateComponent key bitset width = %zu (KeyCode enumerators; LButton = %u)\n",
                registry.GetOrEmplaceSingleton<ZHLN::Components::InputStateComponent>().keys.size(),
                static_cast<unsigned>(ZHLN::KeyCode::LButton));

    if (probe) {
        std::printf("\n-- layout warm-up (pointer at -1,-1) --\n");
        for (int f = 0; f < 3; ++f) {
            setInput(-1.0f, -1.0f, false);
            BuildTestUI(ui, out);
        }
        std::printf("    Container    : ");
        DumpWidget("Container");
        DumpWidget("ButtonA");
        DumpWidget("ButtonB");

        std::printf("\n-- pointer sweep across y at x=50 --\n");
        for (float y = 0.0f; y <= 140.0f; y += 5.0f) {
            setInput(50.0f, y, false);
            BuildTestUI(ui, out);
            std::printf("    y=%6.1f  hoveredA=%d activeA=%d  hoveredB=%d activeB=%d  clayOverCount=%d\n", y,
                        out.hoveredA ? 1 : 0, out.activeA ? 1 : 0, out.hoveredB ? 1 : 0, out.activeB ? 1 : 0,
                        PointerOverCount());
        }

        std::printf("\n-- pointer sweep across x at y=35 --\n");
        for (float x = 0.0f; x <= 320.0f; x += 10.0f) {
            setInput(x, 35.0f, false);
            BuildTestUI(ui, out);
            std::printf("    x=%6.1f  hoveredA=%d activeA=%d  clayOverCount=%d\n", x, out.hoveredA ? 1 : 0,
                        out.activeA ? 1 : 0, PointerOverCount());
        }
        std::printf("\n(probe mode: no pass/fail)\n");
        return 0;
    }

    auto frame = [&](const char* name, float mx, float my, bool down) {
        setInput(mx, my, down);

        // What the GUI will actually be able to see this frame.
        auto* seen = registry.GetSingleton<ZHLN::Components::InputStateComponent>();
        std::printf("      [diag] GUI-visible InputStateComponent=%p mouse=(%.1f,%.1f) LButton=%d\n",
                    static_cast<const void*>(seen), seen ? seen->mouseX : -1.0f, seen ? seen->mouseY : -1.0f,
                    seen ? (seen->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton)) ? 1 : 0) : -1);

        if (fault == Fault::NoInput) {
            registry.Remove<ZHLN::Components::InputStateComponent>(
                registry.SingletonEntity<ZHLN::Components::InputStateComponent>());
            seen = registry.GetSingleton<ZHLN::Components::InputStateComponent>();
            std::printf("      [diag] fault: removed it -> GUI now sees %p\n", static_cast<const void*>(seen));
        }
        if (fault == Fault::NoCallback) {
            return; // host UI callback never runs this frame
        }

        BuildTestUI(ui, out);
        std::printf("  frame %-24s mouse=(%.0f,%.0f) down=%d | A: hover=%d active=%d click=%d (n=%u hoverCb=%u) | B: "
                    "hover=%d active=%d click=%d (n=%u) | clayOver=%d\n",
                    name, mx, my, down ? 1 : 0, out.hoveredA ? 1 : 0, out.activeA ? 1 : 0, out.wasClickedA ? 1 : 0,
                    out.clicksA, out.hoversA, out.hoveredB ? 1 : 0, out.activeB ? 1 : 0, out.wasClickedB ? 1 : 0,
                    out.clicksB, PointerOverCount());
        DumpWidget("ButtonA");
        DumpWidget("ButtonB");
    };

    std::printf("\n-- Frame 1: initial layout (0,0 unpressed) --\n");
    frame("1 warm-up", 0.0f, 0.0f, false);
    Expect(!out.wasClickedA, "F1 !wasClickedA");
    Expect(!out.wasClickedB, "F1 !wasClickedB");
    Expect(out.clicksA == 0u, "F1 clickCountA == 0");
    Expect(out.clicksB == 0u, "F1 clickCountB == 0");

    std::printf("\n-- Frame 2: hover ButtonA (50,35) --\n");
    frame("2 hover A", 50.0f, 35.0f, false);
    Expect(!out.wasClickedA, "F2 !wasClickedA");
    Expect(out.hoveredA, "F2 isHoveredA");
    Expect(!out.activeA, "F2 !isActiveA");
    Expect(!out.wasClickedB, "F2 !wasClickedB");
    Expect(!out.hoveredB, "F2 !isHoveredB");
    Expect(!out.activeB, "F2 !isActiveB");
    Expect(out.clicksA == 0u, "F2 clickCountA == 0");
    Expect(out.hoversA > 0u, "F2 hoverCountA > 0");

    std::printf("\n-- Frame 3: press on ButtonA (50,35) --\n");
    frame("3 press A", 50.0f, 35.0f, true);
    Expect(out.wasClickedA, "F3 wasClickedA");
    Expect(out.hoveredA, "F3 isHoveredA");
    Expect(out.activeA, "F3 isActiveA");
    Expect(!out.wasClickedB, "F3 !wasClickedB");
    Expect(out.clicksA == 1u, "F3 clickCountA == 1");
    Expect(out.clicksB == 0u, "F3 clickCountB == 0");

    std::printf("\n-- Frame 4: hold on ButtonA --\n");
    frame("4 hold A", 50.0f, 35.0f, true);
    Expect(!out.wasClickedA, "F4 !wasClickedA");
    Expect(out.hoveredA, "F4 isHoveredA");
    Expect(out.activeA, "F4 isActiveA");
    Expect(!out.wasClickedB, "F4 !wasClickedB");
    Expect(out.clicksA == 1u, "F4 clickCountA == 1");

    std::printf("\n-- Frame 5: release on ButtonA --\n");
    frame("5 release A", 50.0f, 35.0f, false);
    Expect(!out.wasClickedA, "F5 !wasClickedA");
    Expect(out.hoveredA, "F5 isHoveredA");
    Expect(!out.activeA, "F5 !isActiveA");
    Expect(out.clicksA == 1u, "F5 clickCountA == 1");

    std::printf("\n-- Frame 6: press outside, drag onto ButtonB --\n");
    frame("6 press outside", 500.0f, 400.0f, true);
    frame("6 drag onto B", 50.0f, 85.0f, true);
    Expect(!out.wasClickedB, "F6 !wasClickedB (drag-into must not click)");
    Expect(out.clicksB == 0u, "F6 clickCountB == 0");

    std::printf("\n-- Frame 7: release on B, press on B --\n");
    frame("7 release B", 50.0f, 85.0f, false);
    frame("7 press B", 50.0f, 85.0f, true);
    Expect(out.wasClickedB, "F7 wasClickedB");
    Expect(out.clicksA == 1u, "F7 clickCountA == 1");
    Expect(out.clicksB == 1u, "F7 clickCountB == 1");

    std::printf("\n== %d checks, %d failures ==\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
