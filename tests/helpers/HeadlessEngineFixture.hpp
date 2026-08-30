// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/HeadlessEngineFixture.hpp
//
// The headless engine lifecycle shared by the GPU suites: build a windowless
// engine, silence temporal AA so a static scene is actually static, advance
// frames, and read a frame back for inspection.
//
// Thirteen render suites carried a byte-identical CreateTestEngine apart from
// the window title and the default resolution, and five carried their own
// TickFrames. RunStableScene existed twice, in TestLightingRayTraced.cpp and
// TestRenderDistanceStability.cpp -- the copy that mattered, because it is
// what makes a device-lost recovery re-warm and retry instead of reporting a
// spurious assertion failure.
//
// Depends on the engine, so unlike ImageTesting.hpp this is not usable from a
// CPU-only test.

#pragma once

#include "TestsFramework.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ZHLN::Test::Headless {

struct EngineOptions {
    std::string_view appName               = "Headless Test";
    uint32_t         width                 = 640;
    uint32_t         height                = 480;
    uint32_t         maxBodies             = 256;
    uint32_t         maxBodyPairs          = 512;
    uint32_t         maxContactConstraints = 512;
    uint32_t         tempAllocatorSize     = 8 * 1024 * 1024;
};

/// Creates a headless engine with validation enabled and the default preset
/// suppressed, then seeds the default scene.
///
/// Returns nullptr on failure; callers assert rather than dereference. The
/// default preset is disabled process-wide, which is what keeps the engine
/// from injecting its own sun, floor and camera into a scene the test is
/// trying to measure.
[[nodiscard]] inline auto CreateEngine(const EngineOptions& opts = {}) -> std::unique_ptr<ZHLN::Engine> {
    ZHLN::DefaultPreset::SetDisabled(true);

    const ZHLN::EngineConfig cfg {
        .physics = {
            .maxBodies             = opts.maxBodies,
            .maxBodyPairs          = opts.maxBodyPairs,
            .maxContactConstraints = opts.maxContactConstraints,
            .tempAllocatorSize     = opts.tempAllocatorSize
        },
        .render = {
            // String64 is a FixedString<64>: it converts from string_view but
            // not from std::string (that would chain two user conversions).
            .appName        = opts.appName,
            .width          = opts.width,
            .height         = opts.height,
            .vsync          = false,
            .fullscreen     = false,
            .validationMode = ZHLN::ValidationMode::On,
            .headless       = true
        }
    };

    auto engineRes = ZHLN::Engine::Create(cfg);
    if (!engineRes) {
        return nullptr;
    }

    auto engine = std::move(engineRes.value());
    engine->InitializeDefaultScene();
    return engine;
}

/// Convenience overload for the common "just give me a 640x480 engine" case.
[[nodiscard]] inline auto CreateEngine(std::string_view appName, uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
    return CreateEngine(EngineOptions {.appName = appName, .width = width, .height = height});
}

/// Turns off TAA and zeroes the jitter history.
///
/// A stability test measures frame-to-frame change; TAA's own accumulation
/// would be the largest source of it, so every scene that asserts on stability
/// or on exact pixel values disables this first.
inline void DisableTAA(ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();
    for (const ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::AASettingsComponent>()) {
        reg.Patch<ZHLN::Components::AASettingsComponent>(e, [](auto& aa) {
            aa.state.mode        = ZHLN::AAMode::None;
            aa.state.jitterX     = 0.0f;
            aa.state.jitterY     = 0.0f;
            aa.state.prevJitterX = 0.0f;
            aa.state.prevJitterY = 0.0f;
            aa.state.frameIndex  = 0;
        });
    }
    engine.GetRenderContext().SetAAState(ZHLN::AAState {.mode = ZHLN::AAMode::None});
}

/// Advances the engine by `frames` fixed steps, asserting each tick succeeded.
inline void TickFrames(ZHLN::Engine& engine, uint32_t frames, float dt = 1.0f / 60.0f) {
    for (uint32_t i = 0; i < frames; ++i) {
        engine.ProcessEvents();
        const auto status = engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
        ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
    }
}

/// Renders, reads the frame back, and mirrors it to .png for inspection.
[[nodiscard]] inline auto Capture(ZHLN::Engine& engine, const std::string& ppmPath) -> Image::RgbImage {
    if (!engine.GetRenderContext().CaptureScreenshotPPM(ppmPath)) {
        return {};
    }
    const Image::RgbImage img = Image::LoadPPM(ppmPath);
    if (img.Valid()) {
        (void) Image::SavePNG(Image::PngPathOf(ppmPath), img);
    }
    return img;
}

// ============================================================================
// Device-Lost-Aware Scenario Runner
// ============================================================================

enum class StableRunResult : uint8_t { Ok, AssertionsFailed, PersistentDeviceLost };

constexpr uint32_t kMaxDeviceLostRecoveries = 2;

/// Warms up, runs the measurement once, and distinguishes three outcomes.
///
/// The engine hot-rebuilds its render context on device lost, which silently
/// invalidates anything the scene measured against the old context. Comparing
/// the context pointer before and after is what tells a real assertion failure
/// apart from a recovery that pulled the rug out; on recovery the accumulated
/// failures are rolled back and the scenario is retried from a warm start.
template <typename SceneFn>
[[nodiscard]] StableRunResult
RunStableScene(ZHLN::Engine& engine, uint32_t warmupFrames, const char* label, SceneFn&& sceneFn, uint32_t* outValidationDelta = nullptr) {
    auto&        ctx         = ZHLN::Test::GetThreadLocalContext();
    const size_t failureMark = ctx.failures.size();

    for (uint32_t attempt = 0; attempt <= kMaxDeviceLostRecoveries; ++attempt) {
        if (attempt > 0) {
            ctx.failures.resize(failureMark);
            ZHLN::Println(
                "    [WARN] {}: Vulkan device lost; engine hot-rebuilt. Re-warming and retrying (attempt {}/{}).", label, attempt, kMaxDeviceLostRecoveries
            );
        }

        const uint32_t validationBefore = ZHLN::RenderContext::ValidationErrorCount();

        ZHLN::RenderContext* const preWarmup = &engine.GetRenderContext();
        TickFrames(engine, warmupFrames);
        if (&engine.GetRenderContext() != preWarmup) {
            continue;
        }

        ZHLN::RenderContext* const preWork = &engine.GetRenderContext();
        const bool                 ok      = sceneFn(engine);
        if (ok && &engine.GetRenderContext() == preWork) {
            if (outValidationDelta != nullptr) {
                *outValidationDelta = ZHLN::RenderContext::ValidationErrorCount() - validationBefore;
            }
            return StableRunResult::Ok;
        }
        if (&engine.GetRenderContext() == preWork) {
            return StableRunResult::AssertionsFailed;
        }
    }

    return StableRunResult::PersistentDeviceLost;
}

} // namespace ZHLN::Test::Headless
