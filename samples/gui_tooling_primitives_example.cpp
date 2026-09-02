// samples/gui_tooling_primitives_example.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Demonstrates the six new Tooling Primitive widgets:
//
//   ui.Checkbox(label, bool&)
//   ui.DragFloat / Slider(label, float&, min, max, step)
//   ui.TextInput(label, FixedString/string&)
//   ui.Dropdown(label, int&, span<string_view>)
//   ui.CollapsingHeader(label, defaultOpen, fn)
//   ui.Columns / Splitter(direction, ratio, leftFn, rightFn)
//
// With these primitives you can code any tool window in 20-30 lines of C++.
//
// Builds a "Render Settings" inspector window you would see in a game editor.

#include <Zahlen/GUI.hpp>
#include <Zahlen/Core/String.hpp>
#include <array>
#include <string_view>

namespace {

struct RenderSettings {
    bool              enableVsync   = true;
    bool              showWireframe = false;
    float             fov           = 75.0f;
    float             exposure      = 1.0f;
    ZHLN::String256   profileName;
    int               qualityPreset = 1; // 0=Low 1=Medium 2=High 3=Ultra
};

void BuildRenderSettingsWindow(ZHLN::GUI::Context& ui, RenderSettings& s) {
    static constexpr std::array<std::string_view, 4> kPresets = {
        "Low", "Medium", "High", "Ultra"
    };

    auto scope = ui.Panel("RenderSettings", ZHLN::GUI::PanelConfig {
        .width    = 360.0f,
        .height   = 0.0f,
        .gap      = 6.0f,
        .padding  = 12.0f
    });
    (void)scope;

    ui.Label("Render Settings", ZHLN::GUI::LabelConfig {.scale = 1.0f, .height = 28.0f});

    (void)ui.CollapsingHeader("Display", true, [&]() -> void {
        ui.Checkbox("VSync", "Enable VSync", s.enableVsync);
        ui.Checkbox("Wireframe", "Wireframe Overlay", s.showWireframe);
        ui.DragFloat("FOV", s.fov, 30.0f, 120.0f, 0.5f);
        ui.Slider("Exposure", s.exposure, 0.1f, 5.0f, 0.01f);
    });

    (void)ui.CollapsingHeader("Quality", false, [&]() -> void {
        int idx = s.qualityPreset;
        ui.Dropdown("Preset", "Quality Preset", idx, std::span<const std::string_view>(kPresets));
        s.qualityPreset = idx;
    });

    (void)ui.CollapsingHeader("Profile", true, [&]() -> void {
        ui.TextInput("ProfileName", "Profile Name", s.profileName);
    });

    // Two-column inspector using Splitter/Columns:
    float ratio = 0.4f;
    auto split = ui.Columns("Split", ZHLN::GUI::SplitDirection::Horizontal, ratio,
        [&]() -> void {
            ui.Label("Preview", ZHLN::GUI::LabelConfig {.height = 120.0f});
        },
        [&]() -> void {
            ui.Label("Stats", ZHLN::GUI::LabelConfig {.height = 24.0f});
            ui.Label("FPS: 142", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 20.0f});
            ui.Label("Draws: 312", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 20.0f});
        }
    );
    (void)split;
}

} // namespace
