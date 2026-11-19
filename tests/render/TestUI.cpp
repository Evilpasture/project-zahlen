#include <UI/HostUI.hpp>
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <UI/GUI.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <expected>
#include <string>
#include <vector>

enum class UITestError : uint8_t {
    EngineInitFailed             ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for UI test."> {}) = 1,
    RenderOutputBlank            ZHLN_ANNOTATION(ZHLN::Description<"Rendered frame is blank or failed to capture."> {}),
    UINotRendered                ZHLN_ANNOTATION(ZHLN::Description<"Automated pixel analysis detected zero UI pixels on screen."> {}),
    ButtonClickInteractionFailed ZHLN_ANNOTATION(ZHLN::Description<"Button click interaction, state transitions, or callback dispatch failed."> {}),
    FrameDriveFailed             ZHLN_ANNOTATION(ZHLN::Description<"RenderContext::BeginFrame or EndFrame failed on the hand-driven UI frame."> {}),
    RenderTextureFailed          ZHLN_ANNOTATION(ZHLN::Description<"RenderContext::CreateRenderTexture failed for the second destination."> {}),
    SharedVertexRange            ZHLN_ANNOTATION(ZHLN::Description<"A UI call's vertices were replaced by another call in the same frame."> {}),
    SecondDestinationReplacedUI  ZHLN_ANNOTATION(ZHLN::Description<"A second destination in the frame replaced the window's own UI geometry."> {}),
    DestinationQueryFailed       ZHLN_ANNOTATION(ZHLN::Description<"RenderContext::GetAcquiredTarget did not answer what the frame had acquired."> {}),
};

namespace {

using ZHLN::Test::Image::MeasureSubRegion;
using ZHLN::Test::Image::RgbImage;

// The dominant-hue counters floor at an absolute 8-bit value, so a gate is a
// share of the sampled pixels rather than an absolute count.
[[nodiscard]] constexpr auto ShareAtLeast(uint32_t count, uint32_t pixels, double share = 0.05) noexcept -> bool {
    return pixels > 0 && static_cast<double>(count) >= share * static_cast<double>(pixels);
}

// One Clay frame's geometry, copied out of the context that built it.
//
// `GUI::Context::EndFrame` returns spans into storage the context owns until
// its next BeginFrame, and these tests hold two payloads at once: the copies
// are what make that legal.
struct SolidPayload {
    std::vector<ZHLN::UIBatch>          batches;
    std::vector<ZHLN::VertexPosition>   positions;
    std::vector<ZHLN::VertexSurface>    surfaces;

    [[nodiscard]] auto View() const noexcept -> ZHLN::UIDrawData {
        return ZHLN::UIDrawData {.batches = batches, .positions = positions, .surfaces = surfaces};
    }
};

// A single-colour box, plus same-colour text so the payload carries geometry
// even if a childless box were laid out to nothing -- the colour is what the
// capture is asked about, and the text never is a different hue than the box.
[[nodiscard]] auto BuildSolidBox(ZHLN::COM::IEngine& engine, const ZHLN::FVec4& color, float size) -> SolidPayload {
    ZHLN::HostUI hostUI(engine);
    ZHLN::GUI::Context gui(hostUI.GetFontAtlas());
    ZHLN::GUI::Frame   ui = gui.BeginFrame(ZHLN::GUI::UIInput {.viewport = engine.WindowExtent(engine.PrimaryWindow()), .dt = 1.0f / 60.0f});
    ui.Box("Solid", ZHLN::GUI::BoxConfig {.width = {.fixed = size}, .height = {.fixed = size}, .color = color, .padding = 12.0f}, [&]() {
        ui.Text("X", 24.0f, color);
    });
    const ZHLN::UIDrawData frame = std::move(ui).End();

    SolidPayload payload;
    payload.batches.assign(frame.batches.begin(), frame.batches.end());
    payload.positions.assign(frame.positions.begin(), frame.positions.end());
    payload.surfaces.assign(frame.surfaces.begin(), frame.surfaces.end());
    return payload;
}

} // namespace

namespace {

struct UITestSuite {
    UITestSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~UITestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> ZHLN::Test::Headless::EngineView {
        return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {.appName = "Headless UI Test", .width = width, .height = height});
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> immediate_mode_ui_rendering() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            auto& rc = *engine->Graphics();

            // Set up a simple UI scene
            ZHLN::HostUI hostUI(*engine);
            ZHLN::GUI::Context gui(hostUI.GetFontAtlas());
            auto composeUI = [&](float frameDt) {
                hostUI.BeginFrame();
                gui.SetFontAtlas(hostUI.GetFontAtlas());
                ZHLN::GUI::Frame ui = gui.BeginFrame(hostUI.ConsumeInput(frameDt));

                ui.Box(
                    "TestBox",
                    ZHLN::GUI::BoxConfig {
                        .width   = {.fixed = 200.0f},
                        .height  = {.fixed = 200.0f},
                        .color   = {0.0f, 1.0f, 0.0f, 1.0f}, // Pure Green Box
                        .padding = 20.0f
                    },
                    [&]() {
                        ui.Text("TEST", 20.0f, {0.0f, 0.0f, 1.0f, 1.0f}); // Pure Blue Text
                    }
                );

                (void) hostUI.Submit(std::move(ui).End());
            };

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 5; ++frame) {
                engine->ProcessEvents();
                const auto status = (composeUI(dt), engine->Tick(dt));
                ZHLN::Test::ExpectTrue(status.HasValue());
            }

            // The frame comes back in memory; the .ppm is only a diagnostic
            // mirror, so the assertions below read the pixels the renderer
            // produced rather than a file this test just wrote.
            const std::string ppmPath = "headless_ui_output.ppm";
            const auto        frame   = ZHLN::Test::Headless::Capture(*engine, ppmPath);
            if (!frame.Valid()) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }
            const std::vector<uint8_t>& pixels = frame.rgb;

            uint32_t greenBoxPixels = 0;
            uint32_t blueTextPixels = 0;

            for (size_t i = 0; i < pixels.size(); i += 3) {
                const uint8_t r = pixels[i + 0];
                const uint8_t g = pixels[i + 1];
                const uint8_t b = pixels[i + 2];

                if (g > 200 && r < 50 && b < 50) {
                    greenBoxPixels++;
                }
                // Blue text might have anti-aliasing/blending, check dominantly blue
                else if (b > 150 && r < 100 && g < 100) {
                    blueTextPixels++;
                }
            }

            ZHLN::Test::ExpectGt(greenBoxPixels, 1000u);
            ZHLN::Test::ExpectGt(blueTextPixels, 10u);

            if (greenBoxPixels < 1000u || blueTextPixels < 10u) {
                return std::unexpected(UITestError::UINotRendered);
            }

            ZHLN::Println("    [PASS] Immediate-mode UI rendered {} green box pixels and {} blue text pixels.", greenBoxPixels, blueTextPixels);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> button_click_interaction_and_states() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            // Drive the GUI the way a host that owns its pointer does: hand the
            // frame's UI input in, then run the frame. This is the real seam
            // (HostUI::SubmitInput), not a fabricated one: ConsumeInput
            // returns exactly this vector for the frame it was submitted for, and
            // the device path is not touched. A host with a compositor or a
            // remote pointer uses the same call.
            ZHLN::HostUI hostUI(*engine);
            auto setInput = [&](float mx, float my, bool lbutton) {
                ZHLN::GUI::UIInput ui {};
                ui.mouseX       = mx;
                ui.mouseY       = my;
                ui.mouseDown[0] = lbutton;
                hostUI.SubmitInput(ui);
            };

            uint32_t clickCountA = 0;
            uint32_t clickCountB = 0;
            uint32_t hoverCountA = 0;
            bool     isHoveredA  = false;
            bool     isHoveredB  = false;
            bool     isActiveA   = false;
            bool     isActiveB   = false;
            bool     wasClickedA = false;
            bool     wasClickedB = false;

            ZHLN::GUI::Context gui(hostUI.GetFontAtlas());
            auto composeUI = [&](float frameDt) {
                hostUI.BeginFrame();
                gui.SetFontAtlas(hostUI.GetFontAtlas());
                ZHLN::GUI::Frame ui = gui.BeginFrame(hostUI.ConsumeInput(frameDt));

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
                        wasClickedA = ui.Button("ButtonA", [&]() { clickCountA++; }, [&]() { hoverCountA++; });
                        isHoveredA  = ui.IsItemHovered();
                        isActiveA   = ui.IsItemActive();

                        wasClickedB = ui.Button("ButtonB", [&]() { clickCountB++; });
                        isHoveredB  = ui.IsItemHovered();
                        isActiveB   = ui.IsItemActive();
                    }
                );

                (void) hostUI.Submit(std::move(ui).End());
            };

            constexpr float dt = 1.0f / 60.0f;

            // Frame 1: Initial layout establishment (mouse at 0, 0, unpressed)
            setInput(0.0f, 0.0f, false);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 0u);
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 2: Mouse hovers over ButtonA (approx x: 50, y: 35), unpressed
            setInput(50.0f, 35.0f, false);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectFalse(isActiveA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectFalse(isHoveredB);
            ZHLN::Test::ExpectFalse(isActiveB);
            ZHLN::Test::ExpectEq(clickCountA, 0u);
            ZHLN::Test::ExpectGt(hoverCountA, 0u);

            // Frame 3: Mouse pressed down this frame over ButtonA
            setInput(50.0f, 35.0f, true);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            ZHLN::Test::ExpectTrue(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectTrue(isActiveA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 1u);
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 4: Button is held down across next frame (single-fire guarantee: should NOT click again)
            setInput(50.0f, 35.0f, true);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectTrue(isActiveA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 1u);

            // Frame 5: Mouse released over ButtonA
            setInput(50.0f, 35.0f, false);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectFalse(isActiveA);
            ZHLN::Test::ExpectEq(clickCountA, 1u);

            // Frame 6: Mouse pressed outside (at 500, 400) and dragged onto ButtonB (at 50, 85)
            // Drag-into-click prevention test
            setInput(500.0f, 400.0f, true);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            // Move mouse over ButtonB while still held down from previous frame
            setInput(50.0f, 85.0f, true);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            ZHLN::Test::ExpectFalse(wasClickedB); // Drag-into-hover must NOT trigger click
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 7: Release mouse over ButtonB, then press again to verify ButtonB clicks cleanly
            setInput(50.0f, 85.0f, false);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            setInput(50.0f, 85.0f, true);
            engine->ProcessEvents();
            (void) (composeUI(dt), engine->Tick(dt));
            ZHLN::Test::ExpectTrue(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 1u);
            ZHLN::Test::ExpectEq(clickCountB, 1u);

            return {};
        }

        // Two RenderUI calls in one frame share the frame's vertex slot. The
        // slot is double-buffered by frame, not by call, and Record used to
        // memcpy every payload to offset 0 and address its batches from there:
        // the second call -- in the editor, the preview window's chrome --
        // replaced the first call's vertices before either command buffer
        // executed, so the window the user was looking at drew the preview's
        // geometry.
        //
        // The invariant belongs to the call, not to the destination: a call
        // owns the vertices it wrote until the frame ends. Two calls into two
        // bands of one window make both halves readable in a single capture,
        // which is what a headless engine can observe (it has no second
        // swapchain to capture).


        std::expected<void, ZHLN::ErrorCode> system_vs_custom_cursor_mode() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            const ZHLN::WindowID win = engine->PrimaryWindow();
            ZHLN::COM::HostInputInfo inputState {};
            ZHLN::Test::ExpectTrue(engine->ReadHostInput(win, ZHLN::COM::OutChain(inputState), false).HasValue());
            ZHLN::Test::ExpectEq(inputState.mouseX, 0.0f);
            ZHLN::Test::ExpectEq(inputState.mouseY, 0.0f);

            const auto isCustomCursor = [&] {
                return engine->GetCursorMode(win) == ZHLN::CursorMode::Custom && !engine->IsMouseCaptured(win);
            };

            if (!ZHLN::Test::ExpectEq(engine->GetCursorMode(win), ZHLN::CursorMode::System) ||
                !ZHLN::Test::ExpectFalse(engine->IsMouseCaptured(win)) ||
                !ZHLN::Test::ExpectFalse(isCustomCursor())) {
                return std::unexpected(UITestError::UINotRendered);
            }

            ZHLN::HostUI hostUI(*engine);
            auto tickHostFrames = [&] {
                for (uint32_t i = 0; i < 2; ++i) {
                    engine->ProcessEvents();
                    hostUI.BeginFrame();
                    (void) hostUI.Submit({});
                    ZHLN::Test::ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
                }
            };
            auto countBrightCursorPixels = [](const RgbImage& img) -> uint32_t {
                uint32_t bright = 0;
                for (int y = 0; y <= 8 && y < img.height; ++y) {
                    for (int x = 0; x <= 8 && x < img.width; ++x) {
                        const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3;
                        if (img.rgb[idx] > 200 && img.rgb[idx + 1] > 200 && img.rgb[idx + 2] > 200) {
                            ++bright;
                        }
                    }
                }
                return bright;
            };

            tickHostFrames();
            const auto systemFrame = ZHLN::Test::Headless::Capture(*engine, "headless_cursor_system.ppm");
            if (!systemFrame.Valid()) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }
            const uint32_t systemBright = countBrightCursorPixels(systemFrame);

            engine->SetCursorMode(win, ZHLN::CursorMode::Custom);
            if (!ZHLN::Test::ExpectEq(engine->GetCursorMode(win), ZHLN::CursorMode::Custom) ||
                !ZHLN::Test::ExpectTrue(isCustomCursor())) {
                return std::unexpected(UITestError::UINotRendered);
            }
            tickHostFrames();
            const auto customFrame = ZHLN::Test::Headless::Capture(*engine, "headless_cursor_custom.ppm");
            if (!customFrame.Valid()) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }
            const uint32_t customBright = countBrightCursorPixels(customFrame);

            engine->CaptureMouse(win, true);
            if (!ZHLN::Test::ExpectTrue(engine->IsMouseCaptured(win)) ||
                !ZHLN::Test::ExpectEq(engine->GetCursorMode(win), ZHLN::CursorMode::Custom) ||
                !ZHLN::Test::ExpectFalse(isCustomCursor())) {
                return std::unexpected(UITestError::UINotRendered);
            }
            tickHostFrames();
            const auto capturedFrame = ZHLN::Test::Headless::Capture(*engine, "headless_cursor_captured.ppm");
            if (!capturedFrame.Valid()) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }
            const uint32_t capturedBright = countBrightCursorPixels(capturedFrame);

            engine->CaptureMouse(win, false);
            const bool restoredCustom = isCustomCursor();
            engine->SetCursorMode(win, ZHLN::CursorMode::System);

            if (!ZHLN::Test::ExpectEq(systemBright, 0u) ||
                !ZHLN::Test::ExpectGt(customBright, 4u) ||
                !ZHLN::Test::ExpectEq(capturedBright, 0u) ||
                !ZHLN::Test::ExpectTrue(restoredCustom)) {
                return std::unexpected(UITestError::UINotRendered);
            }
            return {};
        }
    };
};

} // namespace
