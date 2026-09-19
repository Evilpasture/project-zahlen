// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <expected>
#include <fstream>
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
};

namespace {

using ZHLN::Test::Image::MeasureSubRegion;
using ZHLN::Test::Image::RgbImage;

/// The dominant-hue counters floor at an absolute 8-bit value, so a gate is a
/// share of the sampled pixels rather than an absolute count.
[[nodiscard]] constexpr auto ShareAtLeast(uint32_t count, uint32_t pixels, double share = 0.05) noexcept -> bool {
    return pixels > 0 && static_cast<double>(count) >= share * static_cast<double>(pixels);
}

/// One Clay frame's geometry, copied out of the context that built it.
///
/// `GUI::Context::EndFrame` returns spans into storage the context owns until
/// its next BeginFrame, and these tests hold two payloads at once: the copies
/// are what make that legal.
struct SolidPayload {
    std::vector<ZHLN::UIBatch>          batches;
    std::vector<ZHLN::VertexPosition>   positions;
    std::vector<ZHLN::VertexAttributes> attributes;

    [[nodiscard]] auto View() const noexcept -> ZHLN::UIDrawData {
        return ZHLN::UIDrawData {.batches = batches, .positions = positions, .attributes = attributes};
    }
};

/// A single-colour box, plus same-colour text so the payload carries geometry
/// even if a childless box were laid out to nothing -- the colour is what the
/// capture is asked about, and the text never is a different hue than the box.
[[nodiscard]] auto BuildSolidBox(ZHLN::Engine& engine, const JPH::Vec4& color, float size) -> SolidPayload {
    ZHLN::GUI::Context ui(engine);
    ui.BeginFrame(1.0f / 60.0f);
    ui.Box(
        "Solid", ZHLN::GUI::BoxConfig {.width = {.fixed = size}, .height = {.fixed = size}, .color = color, .padding = 12.0f},
        [&]() { ui.Text("X", 24.0f, color); }
    );
    const ZHLN::UIDrawData frame = ui.EndFrame();

    SolidPayload payload;
    payload.batches.assign(frame.batches.begin(), frame.batches.end());
    payload.positions.assign(frame.positions.begin(), frame.positions.end());
    payload.attributes.assign(frame.attributes.begin(), frame.attributes.end());
    return payload;
}

} // namespace

struct UITestSuite {
    UITestSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~UITestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {.appName = "Headless UI Test", .width = width, .height = height});
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> immediate_mode_ui_rendering() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            auto& rc = engine->GetRenderContext();

            // Set up a simple UI scene
            engine->SetUICallback([&](ZHLN::Engine& eng) {
                ZHLN::GUI::Context ui(eng);
                ui.BeginFrame(0.016f);

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

                eng.SetPendingUIData(ui.EndFrame());
            });

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 5; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            const std::string ppmPath = "headless_ui_output.ppm";
            if (!rc.CaptureScreenshotPPM(ppmPath)) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }

            std::string header;
            int         width = 0, height = 0, maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get();

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

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
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            auto setInput = [&](float mx, float my, bool lbutton) {
                auto& in = engine->GetRegistry().GetOrEmplaceSingleton<ZHLN::Components::InputStateComponent>();
                in.mouseX = mx;
                in.mouseY = my;
                in.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), lbutton);
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

            engine->SetUICallback([&](ZHLN::Engine& eng) {
                ZHLN::GUI::Context ui(eng);
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
                        wasClickedA = ui.Button("ButtonA", [&]() { clickCountA++; }, [&]() { hoverCountA++; });
                        isHoveredA  = ui.IsItemHovered();
                        isActiveA   = ui.IsItemActive();

                        wasClickedB = ui.Button("ButtonB", [&]() { clickCountB++; });
                        isHoveredB  = ui.IsItemHovered();
                        isActiveB   = ui.IsItemActive();
                    }
                );

                eng.SetPendingUIData(ui.EndFrame());
            });

            constexpr float dt = 1.0f / 60.0f;

            // Frame 1: Initial layout establishment (mouse at 0, 0, unpressed)
            setInput(0.0f, 0.0f, false);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 0u);
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 2: Mouse hovers over ButtonA (approx x: 50, y: 35), unpressed
            setInput(50.0f, 35.0f, false);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

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
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectTrue(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectTrue(isActiveA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 1u);
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 4: Button is held down across next frame (single-fire guarantee: should NOT click again)
            setInput(50.0f, 35.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectTrue(isActiveA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 1u);

            // Frame 5: Mouse released over ButtonA
            setInput(50.0f, 35.0f, false);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectFalse(isActiveA);
            ZHLN::Test::ExpectEq(clickCountA, 1u);

            // Frame 6: Mouse pressed outside (at 500, 400) and dragged onto ButtonB (at 50, 85)
            // Drag-into-click prevention test
            setInput(500.0f, 400.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            // Move mouse over ButtonB while still held down from previous frame
            setInput(50.0f, 85.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedB); // Drag-into-hover must NOT trigger click
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 7: Release mouse over ButtonB, then press again to verify ButtonB clicks cleanly
            setInput(50.0f, 85.0f, false);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            setInput(50.0f, 85.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

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
        std::expected<void, ZHLN::ErrorCode> render_calls_keep_their_own_vertices() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            auto& rc = engine->GetRenderContext();

            // The engine's own loop initialises the frame schedule; the frame
            // that carries the two calls is driven by hand below, because the
            // host UI callback can only hand RenderSystem one payload.
            ZHLN::Test::Headless::TickFrames(*engine, 2);

            const SolidPayload green = BuildSolidBox(*engine, {0.0f, 1.0f, 0.0f, 1.0f}, 240.0f);
            const SolidPayload blue  = BuildSolidBox(*engine, {0.0f, 0.0f, 1.0f, 1.0f}, 240.0f);
            if (!ZHLN::Test::ExpectTrue(!green.View().Empty() && !blue.View().Empty())) {
                return std::unexpected(UITestError::UINotRendered);
            }

            // A frame that *began*: no error, and not FrameSkipped either -- the
            // test is about to hand-drive a frame, so "there was nothing to draw
            // into" is its own failure here, as it was when a skip was an
            // out-of-date code.
            const auto began = rc.BeginFrame();
            if (!ZHLN::Test::ExpectTrue(began.has_value() && !began->has_value())) {
                return std::unexpected(UITestError::FrameDriveFailed);
            }
            const ZHLN::RenderAttachment attachment = rc.GetWindowAttachment(engine->GetWindow());
            const uint32_t               frameIndex = rc.GetFrameIndex();
            rc.RenderUI(
                ZHLN::UIView {.viewport = {.x = 0, .y = 0, .width = 320, .height = 480}, .target = attachment, .frameIndex = frameIndex}, green.View()
            );
            rc.RenderUI(
                ZHLN::UIView {.viewport = {.x = 320, .y = 0, .width = 320, .height = 480}, .target = attachment, .frameIndex = frameIndex}, blue.View()
            );
            if (!ZHLN::Test::ExpectTrue(rc.EndFrame().has_value())) {
                return std::unexpected(UITestError::FrameDriveFailed);
            }

            const RgbImage frame = ZHLN::Test::Headless::Capture(*engine, "headless_ui_two_calls_one_window.ppm");
            if (!ZHLN::Test::ExpectTrue(frame.Valid())) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }

            const auto left  = MeasureSubRegion(frame, {.x0 = 0.0, .y0 = 0.0, .x1 = 0.5, .y1 = 1.0});
            const auto right = MeasureSubRegion(frame, {.x0 = 0.5, .y0 = 0.0, .x1 = 1.0, .y1 = 1.0});

            ZHLN::Println(
                "    [INFO] One frame, two calls: left green {} blue {}, right blue {} green {}", left.dominantGrn, left.dominantBlu, right.dominantBlu,
                right.dominantGrn
            );

            // Each band shows its own payload and no trace of the other's: the
            // second call must not have taken the first's vertices, and the
            // first must not have leaked into the second's range either.
            const bool leftIsOwnPayload  = ShareAtLeast(left.dominantGrn, left.pixels) && left.dominantBlu == 0;
            const bool rightIsOwnPayload = ShareAtLeast(right.dominantBlu, right.pixels) && right.dominantGrn == 0;
            if (!ZHLN::Test::ExpectTrue(leftIsOwnPayload && rightIsOwnPayload)) {
                return std::unexpected(UITestError::SharedVertexRange);
            }

            return {};
        }

        // The editor's own shape: the primary window is drawn first and, while
        // the preview is open, a second destination is drawn from the same
        // frame. That second call is what used to take the window's vertices,
        // which is why the assertion is on the window: it is the surface the
        // user is looking at, and it must show its own chrome.
        //
        // A headless engine has no second swapchain, so the second destination
        // is a render texture -- the invariant being pinned is the call's, not
        // the destination's.
        std::expected<void, ZHLN::ErrorCode> second_destination_does_not_replace_the_first() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            auto& rc = engine->GetRenderContext();

            ZHLN::Test::Headless::TickFrames(*engine, 2);

            const SolidPayload windowPayload = BuildSolidBox(*engine, {0.0f, 1.0f, 0.0f, 1.0f}, 240.0f);
            const SolidPayload otherPayload  = BuildSolidBox(*engine, {0.0f, 0.0f, 1.0f, 1.0f}, 120.0f);
            if (!ZHLN::Test::ExpectTrue(!windowPayload.View().Empty() && !otherPayload.View().Empty())) {
                return std::unexpected(UITestError::UINotRendered);
            }

            const auto textureRes = rc.CreateRenderTexture(160, 160, false);
            if (!ZHLN::Test::ExpectTrue(textureRes.has_value())) {
                return std::unexpected(UITestError::RenderTextureFailed);
            }
            const ZHLN::TextureHandle texture = *textureRes;

            const ZHLN::Extent2D size = engine->GetWindow().GetSize();

            // As above: the frame has to have begun, not merely to not have failed.
            const auto began = rc.BeginFrame();
            if (!ZHLN::Test::ExpectTrue(began.has_value() && !began->has_value())) {
                return std::unexpected(UITestError::FrameDriveFailed);
            }
            const ZHLN::RenderAttachment attachment = rc.GetWindowAttachment(engine->GetWindow());
            const uint32_t               frameIndex = rc.GetFrameIndex();
            rc.RenderUI(
                ZHLN::UIView {.viewport = {.x = 0, .y = 0, .width = size.width, .height = size.height}, .target = attachment, .frameIndex = frameIndex},
                windowPayload.View()
            );
            rc.RenderUI(
                ZHLN::UIView {
                    .viewport   = {.x = 0, .y = 0, .width = 160, .height = 160},
                    .target     = ZHLN::RenderAttachment {.texture = texture, .mipLevel = 0, .arrayLayer = 0},
                    .frameIndex = frameIndex
                },
                otherPayload.View()
            );
            if (!ZHLN::Test::ExpectTrue(rc.EndFrame().has_value())) {
                return std::unexpected(UITestError::FrameDriveFailed);
            }

            const RgbImage frame = ZHLN::Test::Headless::Capture(*engine, "headless_ui_window_and_second_destination.ppm");
            rc.DestroyRenderTexture(texture);
            if (!ZHLN::Test::ExpectTrue(frame.Valid())) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }

            const auto window = MeasureSubRegion(frame, {});

            ZHLN::Println("    [INFO] Window after a second destination: green {} blue {}", window.dominantGrn, window.dominantBlu);

            const bool windowKeptItsOwnUI = ShareAtLeast(window.dominantGrn, window.pixels) && window.dominantBlu == 0;
            if (!ZHLN::Test::ExpectTrue(windowKeptItsOwnUI)) {
                return std::unexpected(UITestError::SecondDestinationReplacedUI);
            }

            return {};
        }
    };
};

auto RunUISuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UITestSuite>();
}
