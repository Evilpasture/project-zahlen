// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Types.hpp>
#include <cstdint>
#include <vector>

namespace ZHLN {

class Engine;

/**
 * @brief The frame's phases, in execution order.
 *
 * Naming the phases makes the frame reviewable in one place instead of reading
 * Engine::Tick top to bottom, and lets a phase gain or lose steps without the
 * surrounding sequence being touched.
 */
enum class FramePhase : uint8_t {
    Input,        ///< Raw device state -> InputStateComponent.
    UI,           ///< UI interaction plus the host editor callback.
    HotReload,    ///< Gameplay script and shader reload checks.
    PlayerIntent, ///< Input -> movement, using last frame's resolved camera.
    Physics,      ///< Fixed-step simulation and transform write-back.
    Gameplay,     ///< Native and/or scripted gameplay modules.
    Simulation,   ///< The hazard-analysed update graph, then command playback.
    Camera,       ///< Target cameras, camera matrices, LOD selection.
    Visibility,   ///< The render graph: culling, decals, lighting.
    Present,      ///< Frame submission and device-lost handling.
    Fallback,     ///< Missing-module detection and the default preset.
    History,      ///< Motion vectors and transform history.
};

// Phase names come from ZHLN::Reflect::EnumToString(FramePhase) -- the codebase
// convention (see src/render/RenderCore.cpp:95, include/Zahlen/Error.hpp:39,
// include/Zahlen/ScriptBinder.hpp:77). A hand-rolled switch here would be the
// only one in the tree and would silently drift when a phase is added.

/**
 * @brief Values steps need to exchange across phase boundaries.
 *
 * A `SystemGraph` cannot carry these: its `SystemFunc` signature is fixed at
 * `(Engine&, float)`, which is exactly why the gameplay status and the
 * device-lost result had to be threaded out of Engine::Tick by hand.
 */
struct FrameContext {
    GameplayDriver driver     = GameplayDriver::Cpp;
    GameplayStatus status     = GameplayStatus::OK;
    bool           deviceLost = false;
};

using FrameStepFn = void (*)(Engine&, float, FrameContext&);

/// One ordered unit of work. A compiled `SystemGraph` is simply a step whose
/// body executes that graph, so hazard analysis only ever orders systems that
/// genuinely may run concurrently -- never the phases around them.
struct FrameStep {
    FramePhase  phase = FramePhase::Input;
    const char* name  = "UnnamedStep";
    FrameStepFn run   = nullptr;
};

/**
 * @brief Runs the frame as an explicit, ordered list of named steps.
 *
 * Steps execute strictly in registration order; there is no reordering and no
 * implicit parallelism at this level. Concurrency happens only inside a
 * SystemGraph step, bounded by that graph's hazard analysis.
 */
class FrameScheduler {
  public:
    void Add(FramePhase phase, const char* name, FrameStepFn run) {
        _steps.push_back(FrameStep {.phase = phase, .name = name, .run = run});
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

} // namespace ZHLN
