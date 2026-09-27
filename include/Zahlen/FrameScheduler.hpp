// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/CommandLine.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ZHLN {

class Engine;

enum class FramePhase : uint8_t {
    Input,
    UI,
    HotReload,
    PlayerIntent,
    Physics,
    Gameplay,
    Simulation,
    Camera,
    Visibility,
    Present,
    Fallback,
    History,
};


struct FrameContext {
    GameplayDriver driver     = GameplayDriver::Cpp;
    GameplayStatus status     = GameplayStatus::OK;
    bool           deviceLost = false;
};

using FrameStepFn = void (*)(Engine&, float, FrameContext&);

struct FrameStep {
    FramePhase  phase = FramePhase::Input;
    const char* name  = "UnnamedStep";
    FrameStepFn run   = nullptr;
};

class FrameScheduler {
  public:
    void Add(FramePhase phase, const char* name, FrameStepFn run) {
        _steps.push_back(FrameStep {.phase = phase, .name = name, .run = run});
    }

    auto InsertAfter(const char* afterName, FramePhase phase, const char* name, FrameStepFn run) -> bool {
        for (size_t i = 0; i < _steps.size(); ++i) {
            if (std::string_view(_steps[i].name) == afterName) {
                _steps.insert(_steps.begin() + static_cast<std::ptrdiff_t>(i) + 1, FrameStep {.phase = phase, .name = name, .run = run});
                return true;
            }
        }
        return false;
    }

    auto InsertBefore(const char* beforeName, FramePhase phase, const char* name, FrameStepFn run) -> bool {
        for (size_t i = 0; i < _steps.size(); ++i) {
            if (std::string_view(_steps[i].name) == beforeName) {
                _steps.insert(_steps.begin() + static_cast<std::ptrdiff_t>(i), FrameStep {.phase = phase, .name = name, .run = run});
                return true;
            }
        }
        return false;
    }

    void Execute(Engine& engine, float dt, FrameContext& ctx) const {
        for (const FrameStep& step: _steps) {
            if (step.run != nullptr) {
                step.run(engine, dt, ctx);
            }
        }
    }

    [[nodiscard]] auto GetStepCount() const noexcept -> size_t {
        return _steps.size();
    }
    [[nodiscard]] auto IsEmpty() const noexcept -> bool {
        return _steps.empty();
    }
    [[nodiscard]] auto GetSteps() const noexcept -> const std::vector<FrameStep>& {
        return _steps;
    }
    void Clear() noexcept {
        _steps.clear();
    }

  private:
    std::vector<FrameStep> _steps;
};

}
