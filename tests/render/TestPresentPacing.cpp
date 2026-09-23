// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestPresentPacing.cpp
//
// The presentation pacer's headless contract. The headless fixture builds
// with vsync=false and no surface, so the pacer must resolve Decoupled and
// report no paced timing, no margin and no paced dt -- "unknown" everywhere
// rather than a plausible-looking zero. Anything else means a paced policy
// leaked into a session with no display to pace against, and the engine would
// simulate off a fabricated cadence.
#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render/PresentTiming.hpp>
#include <Zahlen/Render/Render.hpp>
#include <cstdint>
#include <expected>

enum class PresentPacingTestError : uint8_t {
    EngineInitFailed        ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for present-pacing test."> {}) = 1,
    UnexpectedPolicy        ZHLN_ANNOTATION(ZHLN::Description<"Headless session did not resolve the Decoupled pacing policy."> {}),
    UnexpectedPacedTiming   ZHLN_ANNOTATION(ZHLN::Description<"Headless session reported paced display timing."> {}),
    UnexpectedMargin        ZHLN_ANNOTATION(ZHLN::Description<"Headless session reported a present slack margin."> {}),
    UnexpectedPacedDelta    ZHLN_ANNOTATION(ZHLN::Description<"Headless session answered a paced delta time."> {}),
    InfoPolicyMismatch      ZHLN_ANNOTATION(ZHLN::Description<"RenderInfo.pacingPolicy disagrees with the presenter's policy."> {}),
    GovernorSteppedDown     ZHLN_ANNOTATION(ZHLN::Description<"Fidelity governor changed quality without a margin signal."> {}),
    TickFailed              ZHLN_ANNOTATION(ZHLN::Description<"Engine::Tick did not return OK while exercising the pacing contract."> {}),
    LateLatchFailed         ZHLN_ANNOTATION(ZHLN::Description<"Engine::PollLateInput did not survive a headless frame."> {}),
};

struct PresentPacingSuite {
    PresentPacingSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~PresentPacingSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    static auto CreateTestEngine() -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(
            ZHLN::Test::Headless::EngineOptions {.appName = "Headless Present Pacing Test", .width = 640, .height = 480}
        );
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> headless_reports_decoupled_without_paced_timing() {
            auto engine = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(PresentPacingTestError::EngineInitFailed);
            }
            auto& rc = engine->GetRenderContext();

            const ZHLN::PresentTimingMetrics timing = rc.GetPresentTiming();
            if (!ZHLN::Test::ExpectEq(timing.policy, ZHLN::PacingPolicy::Decoupled)) {
                return std::unexpected(PresentPacingTestError::UnexpectedPolicy);
            }
            if (!ZHLN::Test::ExpectTrue(!timing.hasPacedTiming && timing.refreshIntervalNs == 0 && !timing.variableRefresh)) {
                return std::unexpected(PresentPacingTestError::UnexpectedPacedTiming);
            }
            if (!ZHLN::Test::ExpectTrue(!timing.hasMargin && timing.lastPresentMarginNs == 0)) {
                return std::unexpected(PresentPacingTestError::UnexpectedMargin);
            }
            if (!ZHLN::Test::ExpectTrue(!rc.GetPacedDeltaTime().has_value())) {
                return std::unexpected(PresentPacingTestError::UnexpectedPacedDelta);
            }
            if (!ZHLN::Test::ExpectEq(rc.GetInfo().pacingPolicy, ZHLN::PacingPolicy::Decoupled)) {
                return std::unexpected(PresentPacingTestError::InfoPolicyMismatch);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> pacing_contract_stable_across_frames() {
            auto engine = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(PresentPacingTestError::EngineInitFailed);
            }
            auto& rc = engine->GetRenderContext();

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 5; ++frame) {
                engine->ProcessEvents();
                if (!ZHLN::Test::ExpectEq(engine->Tick(dt, ZHLN::GameplayDriver::Cpp), ZHLN::GameplayStatus::OK)) {
                    return std::unexpected(PresentPacingTestError::TickFailed);
                }
            }

            // Frames flowed (acquires, submits, headless presents); the pacer
            // must still report the same unpaced contract, not bootstrap into
            // a timing it cannot have.
            const ZHLN::PresentTimingMetrics timing = rc.GetPresentTiming();
            if (!ZHLN::Test::ExpectEq(timing.policy, ZHLN::PacingPolicy::Decoupled)) {
                return std::unexpected(PresentPacingTestError::UnexpectedPolicy);
            }
            if (!ZHLN::Test::ExpectTrue(!timing.hasPacedTiming && !timing.hasMargin)) {
                return std::unexpected(PresentPacingTestError::UnexpectedPacedTiming);
            }
            if (!ZHLN::Test::ExpectTrue(!rc.GetPacedDeltaTime().has_value())) {
                return std::unexpected(PresentPacingTestError::UnexpectedPacedDelta);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> fidelity_governor_inert_without_margin() {
            auto engine = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(PresentPacingTestError::EngineInitFailed);
            }
            auto& rc = engine->GetRenderContext();

            const ZHLN::QualityLevel before = rc.GetSettings().qualityPreset;
            // Past the governor's 90-frame patience: with no margin signal it
            // must never step the preset, whatever the default tier is.
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 95; ++frame) {
                engine->ProcessEvents();
                if (!ZHLN::Test::ExpectEq(engine->Tick(dt, ZHLN::GameplayDriver::Cpp), ZHLN::GameplayStatus::OK)) {
                    return std::unexpected(PresentPacingTestError::TickFailed);
                }
            }
            if (!ZHLN::Test::ExpectEq(rc.GetSettings().qualityPreset, before)) {
                return std::unexpected(PresentPacingTestError::GovernorSteppedDown);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> late_input_latch_safe_headless() {
            auto engine = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(PresentPacingTestError::EngineInitFailed);
            }

            // The headless host's pump is specified as a no-op; the late latch
            // must survive it mid-frame without disturbing the tick.
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 3; ++frame) {
                engine->ProcessEvents();
                engine->PollLateInput();
                if (!ZHLN::Test::ExpectEq(engine->Tick(dt, ZHLN::GameplayDriver::Cpp), ZHLN::GameplayStatus::OK)) {
                    return std::unexpected(PresentPacingTestError::LateLatchFailed);
                }
                engine->PollLateInput();
            }
            return {};
        }
    };
};

auto RunPresentPacingSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<PresentPacingSuite>();
}
