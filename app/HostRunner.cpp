// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Platform.hpp"
#include <Signal/CrashHandler.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace ZHLN {

namespace {
// Signal handlers retain the address of their state across individual runs.
// This is process-wide, not a member whose address could outlive its host.
auto SharedCrashState() -> CrashState& {
    static CrashState state;
    return state;
}
} // namespace

struct ProcessScope::Impl {
    TaskSystem::Scope tasks;
};

ProcessScope::ProcessScope() {
    SetupSignalHandler(SharedCrashState());
    Platform::Init();
    _impl = std::make_unique<Impl>(); // starts workers only after platform setup
}

ProcessScope::~ProcessScope() = default;

void ProcessScope::PollCrashes(const Engine* engine) const {
    if (engine != nullptr) {
        CheckForCrashes(SharedCrashState(), engine);
    } else {
        CheckForCrashes(SharedCrashState());
    }
}

FramePacer::FramePacer(uint32_t fpsLimit) noexcept:
    _targetFrameTime(fpsLimit > 0 ? 1.0 / static_cast<double>(fpsLimit) : 0.0) {
}

auto FramePacer::DisplayPaced(std::optional<float> pacedDt) const noexcept -> bool {
    constexpr double kFpsCapSlack = 1.05;
    return pacedDt.has_value() && (_targetFrameTime == 0.0 || _targetFrameTime <= static_cast<double>(*pacedDt) * kFpsCapSlack);
}

auto FramePacer::SelectDelta(float wallDt, std::optional<float> pacedDt) const noexcept -> float {
    return DisplayPaced(pacedDt) ? *pacedDt : wallDt;
}

void FramePacer::EndFrame(Clock::TimePoint frameStart, std::optional<float> pacedDt) const noexcept {
    if (_targetFrameTime <= 0.0 || DisplayPaced(pacedDt)) {
        return;
    }
    const auto   now     = Clock::SteadyClock::now();
    const double elapsed = std::chrono::duration<double>(now - frameStart).count();
    if (elapsed >= _targetFrameTime) {
        return;
    }
    const double sleepTime = _targetFrameTime - elapsed;
    if (sleepTime > 0.002) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
    }
    while (std::chrono::duration<double>(Clock::SteadyClock::now() - frameStart).count() < _targetFrameTime) {
        CPURelax();
    }
}

} // namespace ZHLN
