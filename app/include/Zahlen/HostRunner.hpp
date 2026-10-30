// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Engine/Clock.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace ZHLN {

class Engine;

// Host lifetime shared by the Engine application and Kernel-only applications.
// Construct before the host, destroy after it: the host must release any
// resources used by task workers before its task scope is torn down.
class ProcessScope {
  public:
    ProcessScope();
    ~ProcessScope();

    ProcessScope(const ProcessScope&) = delete;
    auto operator=(const ProcessScope&) -> ProcessScope& = delete;
    ProcessScope(ProcessScope&&) = delete;
    auto operator=(ProcessScope&&) -> ProcessScope& = delete;

    void PollCrashes(const Engine* engine = nullptr) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// Shared host-level frame pacing. Engine::Run is a lower-level Core API and
// retains its own loop; application hosts use this non-GPU pacer instead of
// duplicating sleep/spin policy. A display-paced frame already meets the cap.
class FramePacer {
  public:
    explicit FramePacer(uint32_t fpsLimit = 0) noexcept;

    [[nodiscard]] static auto BeginFrame() noexcept -> Clock::TimePoint { return Clock::SteadyClock::now(); }
    [[nodiscard]] auto TargetFrameTime() const noexcept -> double { return _targetFrameTime; }
    [[nodiscard]] auto DisplayPaced(std::optional<float> pacedDt) const noexcept -> bool;
    [[nodiscard]] auto SelectDelta(float wallDt, std::optional<float> pacedDt) const noexcept -> float;
    void EndFrame(Clock::TimePoint frameStart, std::optional<float> pacedDt) const noexcept;

  private:
    double _targetFrameTime = 0.0;
};

} // namespace ZHLN
