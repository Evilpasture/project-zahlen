// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ZHLN {

struct FixedStepBatch {
    uint32_t steps = 0;
    float    alpha = 0.0f;
};

struct ClockConfig {
    float maxDeltaTime = 0.1f; // <= 0.0f disables clamping
    float timeScale    = 1.0f;
    bool  paused       = false;
};

// Monotonic frame and simulation clock.
//
// Progression (`Tick`, `Advance`) is strictly separated from `const` observers
// (`DeltaTime`, `UnscaledDeltaTime`, `ElapsedTime`, ...): reading `DeltaTime()`
// never mutates the clock or swallows elapsed wall time. Wall sampling uses
// `std::chrono::steady_clock` rather than `high_resolution_clock` (which aliases
// `system_clock` on libstdc++ and jumps across NTP adjustments).
class Clock {
  public:
    using SteadyClock = std::chrono::steady_clock;
    using TimePoint   = SteadyClock::time_point;

    explicit Clock(ClockConfig config = {}) noexcept
        : _start(SteadyClock::now()),
          _last(_start),
          _maxDeltaTime(config.maxDeltaTime),
          _timeScale(std::max(0.0f, config.timeScale)),
          _paused(config.paused) {
    }

    // Samples `steady_clock::now()`, advances one frame, and returns `DeltaTime()`.
    auto Tick() noexcept -> float {
        const TimePoint now        = SteadyClock::now();
        const double    rawSeconds = std::chrono::duration<double>(now - _last).count();
        _last                      = now;
        return Advance(rawSeconds);
    }

    // Deterministic progression by an explicit wall interval (for headless
    // simulation loops, replay harnesses, and unit tests).
    auto Advance(double rawSeconds) noexcept -> float {
        const double clampedNonNegative = std::max(0.0, rawSeconds);
        const float  rawDt              = static_cast<float>(clampedNonNegative);
        const float  clampedDt          = (_maxDeltaTime > 0.0f) ? std::min(rawDt, _maxDeltaTime) : rawDt;

        _rawDeltaTime      = rawDt;
        _unscaledDeltaTime = clampedDt;
        _unscaledElapsed += static_cast<double>(clampedDt);
        ++_frameCount;

        if (_paused) {
            _deltaTime = 0.0f;
            return 0.0f;
        }

        const float scaledDt = clampedDt * _timeScale;
        _deltaTime           = scaledDt;
        _elapsed += static_cast<double>(scaledDt);
        _fixedAccumulator += scaledDt;
        return _deltaTime;
    }

    // Resets accumulated counters and re-anchors the wall reference to now.
    void Reset() noexcept {
        _start             = SteadyClock::now();
        _last              = _start;
        _rawDeltaTime      = 0.0f;
        _unscaledDeltaTime = 0.0f;
        _deltaTime         = 0.0f;
        _elapsed           = 0.0;
        _unscaledElapsed   = 0.0;
        _fixedAccumulator  = 0.0f;
        _frameCount        = 0;
    }

    // Re-anchors `_last` to `steady_clock::now()` without advancing simulation
    // state, discarding wall time spent in modal stalls or suspended states.
    void ResyncWallClock() noexcept {
        _last = SteadyClock::now();
    }

    // --- Pure observers (no side effects) ------------------------------------

    // Scaled, clamped delta time for the most recent frame (`0.0f` while paused).
    [[nodiscard]] constexpr auto DeltaTime() const noexcept -> float {
        return _deltaTime;
    }

    // Clamped wall delta time for the most recent frame (unaffected by pause or timeScale).
    [[nodiscard]] constexpr auto UnscaledDeltaTime() const noexcept -> float {
        return _unscaledDeltaTime;
    }

    // Unclamped, unscaled wall delta time for the most recent frame.
    [[nodiscard]] constexpr auto RawDeltaTime() const noexcept -> float {
        return _rawDeltaTime;
    }

    // Accumulated scaled simulation time across all unpaused frames.
    [[nodiscard]] constexpr auto ElapsedTime() const noexcept -> double {
        return _elapsed;
    }

    // Accumulated clamped wall time across all frames (including paused frames).
    [[nodiscard]] constexpr auto UnscaledElapsedTime() const noexcept -> double {
        return _unscaledElapsed;
    }

    // Live monotonic wall-clock seconds elapsed since construction or `Reset()`.
    [[nodiscard]] auto WallTimeSinceStart() const noexcept -> double {
        return std::chrono::duration<double>(SteadyClock::now() - _start).count();
    }

    [[nodiscard]] constexpr auto FrameCount() const noexcept -> uint64_t {
        return _frameCount;
    }

    // --- Playback & clamping controls ----------------------------------------

    constexpr void SetTimeScale(float scale) noexcept {
        _timeScale = (scale >= 0.0f && scale < std::numeric_limits<float>::infinity()) ? scale : 0.0f;
    }

    [[nodiscard]] constexpr auto TimeScale() const noexcept -> float {
        return _timeScale;
    }

    void SetPaused(bool paused) noexcept {
        if (_paused && !paused) {
            ResyncWallClock();
        }
        _paused = paused;
        if (_paused) {
            _deltaTime = 0.0f;
        }
    }

    void Pause() noexcept {
        SetPaused(true);
    }

    void Resume() noexcept {
        SetPaused(false);
    }

    [[nodiscard]] constexpr auto IsPaused() const noexcept -> bool {
        return _paused;
    }

    constexpr void SetMaxDeltaTime(float maxDt) noexcept {
        _maxDeltaTime = (maxDt > -std::numeric_limits<float>::infinity() && maxDt < std::numeric_limits<float>::infinity()) ? maxDt : 0.0f;
    }

    [[nodiscard]] constexpr auto MaxDeltaTime() const noexcept -> float {
        return _maxDeltaTime;
    }

    // --- Fixed-timestep accumulator ------------------------------------------

    // Drains up to `maxSteps` fixed steps of length `fixedDt` from the scaled
    // simulation accumulator and returns `{steps, alpha}` with `alpha` in `[0, 1)`.
    // Excess remainder past `maxSteps` is dropped modulo `fixedDt` to prevent
    // spiral-of-death catch-up spikes.
    [[nodiscard]] auto ConsumeFixedSteps(float fixedDt, uint32_t maxSteps = 8) noexcept -> FixedStepBatch {
        if (!(fixedDt > 0.0f) || !std::isfinite(fixedDt)) {
            return {};
        }

        uint32_t steps = 0;
        while (_fixedAccumulator >= fixedDt && steps < maxSteps) {
            _fixedAccumulator -= fixedDt;
            ++steps;
        }

        if (_fixedAccumulator >= fixedDt) {
            _fixedAccumulator = std::fmod(_fixedAccumulator, fixedDt);
        }

        const float alpha = std::clamp(_fixedAccumulator / fixedDt, 0.0f, 1.0f);
        return FixedStepBatch {.steps = steps, .alpha = alpha};
    }

    constexpr void ResetFixedAccumulator() noexcept {
        _fixedAccumulator = 0.0f;
    }

    [[nodiscard]] constexpr auto FixedAccumulator() const noexcept -> float {
        return _fixedAccumulator;
    }

  private:
    TimePoint _start {};
    TimePoint _last {};
    float     _maxDeltaTime      = 0.1f;
    float     _timeScale         = 1.0f;
    float     _rawDeltaTime      = 0.0f;
    float     _unscaledDeltaTime = 0.0f;
    float     _deltaTime         = 0.0f;
    float     _fixedAccumulator  = 0.0f;
    double    _elapsed           = 0.0;
    double    _unscaledElapsed   = 0.0;
    uint64_t  _frameCount        = 0;
    bool      _paused            = false;
};

} // namespace ZHLN
