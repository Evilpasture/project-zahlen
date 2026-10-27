// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Clock.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

namespace ZHLN::Test {

namespace {
[[nodiscard]] auto ApproxEq(double a, double b, double eps = 1e-5) noexcept -> bool {
    return std::abs(a - b) <= eps;
}
} // namespace

void test_clock_observers_are_side_effect_free() {
    Clock clock({.maxDeltaTime = 0.1f, .timeScale = 1.0f});

    Expect(clock.FrameCount()) == 0u;
    Expect(clock.DeltaTime()) == 0.0f;
    Expect(clock.UnscaledDeltaTime()) == 0.0f;
    Expect(clock.RawDeltaTime()) == 0.0f;
    Expect(clock.ElapsedTime()) == 0.0;
    Expect(clock.UnscaledElapsedTime()) == 0.0;

    const float dt = clock.Advance(0.016);
    Expect(ApproxEq(dt, 0.016));
    Expect(clock.FrameCount()) == 1u;

    // Repeated observer queries must never mutate state or consume time.
    for (int i = 0; i < 8; ++i) {
        Expect(clock.DeltaTime()) == dt;
        Expect(clock.UnscaledDeltaTime()) == dt;
        Expect(clock.RawDeltaTime()) == dt;
        Expect(ApproxEq(clock.ElapsedTime(), 0.016));
        Expect(ApproxEq(clock.UnscaledElapsedTime(), 0.016));
        Expect(clock.FrameCount()) == 1u;
    }
}

void test_clock_clamping_and_time_scale() {
    Clock clock({.maxDeltaTime = 0.05f, .timeScale = 0.25f});

    // Spike of 0.50s clamps to 0.05s unscaled, then scales by 0.25 -> 0.0125s.
    const float dt = clock.Advance(0.50);
    Expect(ApproxEq(clock.RawDeltaTime(), 0.50));
    Expect(ApproxEq(clock.UnscaledDeltaTime(), 0.05));
    Expect(ApproxEq(dt, 0.0125));
    Expect(ApproxEq(clock.DeltaTime(), 0.0125));
    Expect(ApproxEq(clock.ElapsedTime(), 0.0125));
    Expect(ApproxEq(clock.UnscaledElapsedTime(), 0.05));

    // Negative raw input is clamped cleanly to 0.0.
    clock.Advance(-1.0);
    Expect(clock.RawDeltaTime()) == 0.0f;
    Expect(clock.DeltaTime()) == 0.0f;

    // Disabling maxDeltaTime (<= 0) passes large steps through unscaled.
    clock.SetMaxDeltaTime(0.0f);
    clock.SetTimeScale(2.0f);
    clock.Advance(0.25);
    Expect(ApproxEq(clock.UnscaledDeltaTime(), 0.25));
    Expect(ApproxEq(clock.DeltaTime(), 0.50));

    // Negative timeScale clamps to 0.
    clock.SetTimeScale(-3.0f);
    Expect(clock.TimeScale()) == 0.0f;
}

void test_clock_pause_freezes_simulation_and_resyncs_wall_time() {
    Clock clock({.maxDeltaTime = 0.5f, .timeScale = 1.0f});

    clock.Advance(0.02);
    Expect(ApproxEq(clock.ElapsedTime(), 0.02));
    Expect(ApproxEq(clock.FixedAccumulator(), 0.02));

    clock.Pause();
    Expect(clock.IsPaused());
    Expect(clock.DeltaTime()) == 0.0f;

    // Advancing while paused increments FrameCount and UnscaledElapsedTime,
    // while keeping DeltaTime, ElapsedTime, and FixedAccumulator frozen.
    const float pausedDt = clock.Advance(0.04);
    Expect(pausedDt) == 0.0f;
    Expect(clock.DeltaTime()) == 0.0f;
    Expect(ApproxEq(clock.UnscaledDeltaTime(), 0.04));
    Expect(ApproxEq(clock.ElapsedTime(), 0.02));
    Expect(ApproxEq(clock.UnscaledElapsedTime(), 0.06));
    Expect(ApproxEq(clock.FixedAccumulator(), 0.02));
    Expect(clock.FrameCount()) == 2u;

    // Sleep briefly while paused, then Resume() + Tick(): the wall interval
    // spent paused must be discarded by Resume()'s ResyncWallClock().
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    clock.Resume();
    Expect(!clock.IsPaused());
    const float postResumeDt = clock.Tick();
    Expect(postResumeDt < 0.020f);
    Expect(clock.WallTimeSinceStart() >= 0.020);
}

void test_clock_fixed_step_accumulator_and_spiral_clamp() {
    Clock clock({.maxDeltaTime = 1.0f, .timeScale = 1.0f});
    constexpr float kFixedDt = 0.01f;

    // 2.5 steps -> 2 steps + alpha = 0.5.
    clock.Advance(0.025);
    const FixedStepBatch first = clock.ConsumeFixedSteps(kFixedDt, 8);
    Expect(first.steps) == 2u;
    Expect(ApproxEq(first.alpha, 0.5, 1e-4));

    // Another 0.6 steps -> total 1.1 steps -> 1 step + alpha = 0.1.
    clock.Advance(0.006);
    const FixedStepBatch second = clock.ConsumeFixedSteps(kFixedDt, 8);
    Expect(second.steps) == 1u;
    Expect(ApproxEq(second.alpha, 0.1, 1e-4));

    // Huge spike exceeding maxSteps=4: executes 4 steps and drops surplus
    // whole steps modulo fixedDt so alpha stays in [0, 1).
    clock.Reset();
    clock.Advance(0.0975); // 9.75 fixed steps
    const FixedStepBatch clamped = clock.ConsumeFixedSteps(kFixedDt, 4);
    Expect(clamped.steps) == 4u;
    Expect(ApproxEq(clamped.alpha, 0.75, 1e-3));
    Expect(clamped.alpha >= 0.0f && clamped.alpha < 1.0f);

    // Degenerate fixedDt returns zero steps safely.
    const FixedStepBatch invalid = clock.ConsumeFixedSteps(0.0f, 4);
    Expect(invalid.steps) == 0u;
    Expect(invalid.alpha) == 0.0f;
}

} // namespace ZHLN::Test
