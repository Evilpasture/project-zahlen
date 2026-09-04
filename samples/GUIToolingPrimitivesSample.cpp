// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/CommandLine.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <array>
#include <cstdlib>
#include <string_view>

namespace {

struct RenderSettings {
    bool            enableVsync    = true;
    bool            showWireframe  = false;
    bool            showGrid       = true;
    float           fov            = 75.0f;
    float           exposure       = 1.0f;
    float           bloomIntensity = 0.15f;
    ZHLN::String256 profileName    = ZHLN::String256("Default");
    int             qualityPreset  = 1;
};

constexpr std::array<std::string_view, 4> kQualityPresets = {"Low", "Medium", "High", "Ultra"};

void DrawRenderSettingsWindow(ZHLN::GUI::Context& ui, RenderSettings& s) {
    ui.Panel(
        "RenderSettings",
        ZHLN::GUI::PanelConfig {
            .width   = 400.0f,
            .height  = 0.0f,
            // Docked 24px in from the LEFT edge. The browser docks 24px in
            // from the right one, so the two windows can never overlap no
            // matter how narrow the user's window is -- a centered window
            // collided with the browser the moment the viewport went below
            // ~1200px wide, and the two panels' contents interleaved.
            .x       = 24.0f,
            .y       = 0.0f,
            .anchorMinX = 0.0f,
            .anchorMaxX = 0.0f,
            .gap     = 6.0f,
            .padding = 14.0f,
        },
        [&]() -> void {
            ui.Label(
                "Render Settings", ZHLN::GUI::LabelConfig {
                                       .scale         = 1.05f,
                                       .color         = {0.40f, 0.72f, 1.00f, 1.0f},
                                       .align         = ZHLN::GUI::TextAlignment::Center,
                                       .verticalAlign = ZHLN::GUI::TextVerticalAlignment::Center,
                                       .height        = 28.0f,
                                   }
            );

            ui.CollapsingHeader("Display", true, [&]() -> void {
                ui.Checkbox("VSync", "Enable VSync", s.enableVsync);
                ui.Checkbox("Wireframe", "Wireframe Overlay", s.showWireframe);
                ui.Checkbox("Grid", "Show Grid", s.showGrid);
                // Same fixed label/value slots on every row: the tracks all
                // start and end on the same x instead of flexing with the
                // label ("FOV" vs "Exposure") and the value text mid-drag.
                ZHLN::GUI::SliderConfig sliderCfg = {.labelWidth = 72.0f, .valueWidth = 40.0f};
                ui.DragFloat("FOV", s.fov, 30.0f, 120.0f, 0.5f, sliderCfg);
                ui.Slider("Exposure", s.exposure, 0.1f, 5.0f, 0.01f, sliderCfg);
                ui.Slider("Bloom", s.bloomIntensity, 0.0f, 1.0f, 0.01f, sliderCfg);
            });

            ui.CollapsingHeader("Quality", false, [&]() -> void {
                int idx = s.qualityPreset;
                ui.Dropdown("Preset", "Quality Preset", idx, std::span<const std::string_view>(kQualityPresets));
                s.qualityPreset = idx;
            });

            ui.CollapsingHeader("Profile", true, [&]() -> void { ui.TextInput("ProfileName", "Profile Name", s.profileName); });

            float split = 0.55f;
            ui.Columns(
                "PreviewSplit", ZHLN::GUI::SplitDirection::Horizontal, split,
                [&]() -> void {
                    ui.Box(
                        ZHLN::GUI::BoxConfig {
                            .height     = 100.0f,
                            .color      = {0.06f, 0.09f, 0.14f, 0.85f},
                            .direction  = ZHLN::GUI::FlexDirection::Column,
                            .justify    = ZHLN::GUI::FlexJustify::Center,
                            .alignItems = ZHLN::GUI::FlexAlign::Center,
                            .padding    = 8.0f,
                        },
                        [&]() -> void {
                            ui.Label(
                                "Preview", ZHLN::GUI::LabelConfig {
                                               .scale         = 0.90f,
                                               .color         = {0.55f, 0.70f, 0.90f, 0.60f},
                                               .align         = ZHLN::GUI::TextAlignment::Center,
                                               .verticalAlign = ZHLN::GUI::TextVerticalAlignment::Center,
                                           }
                            );
                        }
                    );
                },
                [&]() -> void {
                    ui.Label(
                        "Stats", ZHLN::GUI::LabelConfig {
                                     .scale  = 0.80f,
                                     .color  = {0.70f, 0.80f, 0.95f, 1.0f},
                                     .height = 20.0f,
                                 }
                    );
                    ui.Label("FPS: 142", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
                    ui.Label("Draws: 312", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
                    ui.Label("VSync: on", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
                }
            );
        }
    );
}

// ============================================================================
// CONTENT BROWSER — the primitives a tool window actually needs
// ============================================================================
// This second window exists to be dogfooded: a long list that does not fit the
// screen (ScrollBox), rows you can select and double-click (Selectable), a
// hierarchy that expands (TreeNode), an icon and a sprite slice (Image), a
// paragraph that has to reflow to the panel width (wrapped Label), and hover
// hints (Tooltip).

struct BrowserState {
    int  selectedIndex     = 3;
    bool materialsOpen     = true;
    bool meshesOpen        = false;
    bool matRockSelected   = false;
    bool matWoodSelected   = false;
    bool meshRockSelected  = false;
};

constexpr std::array<std::string_view, 24> kAssets = {
    "SM_Rock_Large",  "SM_Rock_Small",   "SM_Boulder_01",  "T_Rock_Albedo",  "T_Rock_Normal",  "T_Rock_Roughness",
    "M_Rock_Base",    "M_Rock_Wet",      "SM_Tree_Pine",   "SM_Tree_Oak",    "T_Bark_Albedo",  "M_Bark_Base",
    "SM_Grass_Tuft",  "T_Grass_Albedo",  "M_Grass_Base",   "SM_Fence_Post",  "SM_Fence_Rail",  "T_Wood_Albedo",
    "M_Wood_Base",    "SFX_Footstep",    "SFX_Impact",     "AUD_Music_Loop", "VFX_Dust_Puff",  "VFX_Splash"
};

void DrawContentBrowser(ZHLN::GUI::Context& ui, BrowserState& state) {
    ui.Panel(
        "ContentBrowser",
        ZHLN::GUI::PanelConfig {
            .width      = 360.0f,
            .height     = 520.0f,
            // Anchor 1.0 is the RIGHT edge and `x` is measured from it, so a
            // 24px inset from the right is -(width + 24), not +24 (which parks
            // the whole panel off-screen).
            .x          = -384.0f,
            .y          = 24.0f,
            .anchorMinX = 1.0f,
            .anchorMinY = 0.0f,
            .anchorMaxX = 1.0f,
            .anchorMaxY = 0.0f,
            .gap        = 8.0f,
            .padding    = 14.0f,
        },
        [&]() -> void {
            ui.Label(
                "Content Browser", ZHLN::GUI::LabelConfig {
                                       .scale         = 1.00f,
                                       .color         = {0.40f, 0.72f, 1.00f, 1.0f},
                                       .verticalAlign = ZHLN::GUI::TextVerticalAlignment::Center,
                                       .height        = 26.0f,
                                   }
            );

            // A paragraph that reflows to the panel width instead of running
            // off the right edge. height = 0 lets the label grow by lines.
            ui.Label(
                "Scroll the list with the mouse wheel. Click a row to select it, "
                "double-click to open the asset, and hover anything for a hint.",
                ZHLN::GUI::LabelConfig {.scale = 0.78f, .color = {0.62f, 0.72f, 0.88f, 0.85f}, .height = 0.0f, .wrap = true}
            );

            // Icon + sprite-sheet slice side by side.
            ui.Box(
                "IconRow", ZHLN::GUI::BoxConfig {
                               .height     = 40.0f,
                               // The strip must keep its height: the thumbnails
                               // inside are 28px and would hang out of a squeezed
                               // row. AssetList below absorbs the overflow.
                               .flexShrink = 0.0f,
                               .direction  = ZHLN::GUI::FlexDirection::Row,
                               .gap       = 10.0f,
                               .padding   = 4.0f,
                           },
                [&]() -> void {
                    // TextureHandle::Invalid is the engine's white fallback
                    // slot (TextureManager::GetBindlessIndex). Do NOT pass
                    // ZHLN::SystemTextures::White: TextureHandle values are
                    // hashed asset ids, so that constant is not registered and
                    // only logs "TextureHandle 0x2 was not found in registry".
                    ui.Icon("IconThumb", ZHLN::TextureHandle::Invalid, 28.0f);
                    ui.Tooltip("Thumbnail placeholder (built-in white texture)");

                    ui.Image(
                        "AtlasSlice", ZHLN::TextureHandle::Invalid,
                        ZHLN::GUI::ImageConfig {
                            .width  = 28.0f,
                            .height = 28.0f,
                            .mode   = ZHLN::GUI::ImageScaleMode::Tile,
                            .uv0x   = 0.0f,
                            .uv0y   = 0.0f,
                            .uv1x   = 0.25f,
                            .uv1y   = 0.25f,
                            .sourceWidth  = 8.0f,
                            .sourceHeight = 8.0f,
                        }
                    );
                    ui.Tooltip("A tiled quarter of the atlas");
                }
            );

            // The asset list: `.flexGrow = 1` lets it take the panel's leftover
            // space and shrink back when the hierarchy below it opens, instead
            // of pushing the rows under it out of the panel.
            ui.ScrollBox(
                "AssetList", ZHLN::GUI::ScrollBoxConfig {.height = 300.0f, .gap = 1.0f, .padding = 2.0f, .flexGrow = 1.0f, .scrollbarWidth = 8.0f},
                [&]() -> void {
                    for (int i = 0; i < static_cast<int>(kAssets.size()); ++i) {
                        // One bool per row, kept in the state struct so the
                        // selection survives between frames without the sample
                        // having to own an ECS view of it.
                        const int  index = i;
                        const bool wasSelected = (index == state.selectedIndex);
                        bool       rowSelected = wasSelected;

                        std::array<char, 32> idBuf {};
                        const std::string_view id = ZHLN::FormatTo(idBuf, "Asset_{:02}", index);

                        bool opened = false;
                        ui.Selectable(
                            id, kAssets[static_cast<size_t>(index)], rowSelected, ZHLN::GUI::SelectableConfig {},
                            [&](bool nowSelected) -> void {
                                // Single click: this row becomes the selection.
                                state.selectedIndex = nowSelected ? index : -1;
                            },
                            [&]() -> void { opened = true; }
                        );
                        if (opened) {
                            ZHLN::Log("[ContentBrowser] Open asset: {}", kAssets[static_cast<size_t>(index)]);
                        }
                    }
                }
            );

            // A hierarchy: selection and expansion are independent.
            ui.CollapsingHeader("Hierarchy", true, [&]() -> void {
                ui.TreeNode(
                    "Materials", "Materials", state.materialsOpen,
                    [&]() -> void {
                        ui.Selectable("Mat_Rock", "M_Rock_Base", state.matRockSelected);
                        ui.Selectable("Mat_Wood", "M_Wood_Base", state.matWoodSelected);
                    }
                );
                ui.TreeNode(
                    "Meshes", "Meshes", state.meshesOpen,
                    [&]() -> void { ui.Selectable("Mesh_Rock", "SM_Rock_Large", state.meshRockSelected); }
                );
            });
        }
    );
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    return ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
        .and_then([](const ZHLN::CommandLineOptions& options) -> std::expected<void, ZHLN::Error> {
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return {};
            }

            ZHLN::SetLogLevel(options.logLevel);
            ZHLN::DefaultPreset::SetDisabled(true); // Suppress missing libgameplay.so fallback

            RenderSettings settings;
            BrowserState   browser;

            // ZHLN::Engine::Run handles Platform::Init, resize events, frame pacing,
            // and executes your UI callback in Phase::UI before rendering.
            return ZHLN::Engine::Run(options, [&settings, &browser](ZHLN::Engine& engine) {
                ZHLN::GUI::Context ui(engine.GetRegistry(), engine.GetCurrentFrame());
                DrawRenderSettingsWindow(ui, settings);
                DrawContentBrowser(ui, browser);
            });
        })
        .transform([]() -> int { return EXIT_SUCCESS; })
        .or_else([](const ZHLN::Error& err) -> std::expected<int, ZHLN::Error> {
            ZHLN::Log("Fatal Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .value();
}
