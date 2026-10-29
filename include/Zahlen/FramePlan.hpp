// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/FramePlan.hpp
//
// One frame's order of execution, as data instead of as a callback that reaches
// into the engine.
//
// This header replaces FrameScheduler.hpp. The class it replaces could already
// hold an ordered list of steps; what it could not do is say where a step goes.
// A module was handed the scheduler and mutated it --
//
//     if (!scheduler.InsertBefore("HostUICallback", FramePhase::Input, "CharacterInput", &Step)) {
//         scheduler.Add(FramePhase::Input, "CharacterInput", &Step);   // <- the fallback
//     }
//
// -- and the fallback is the problem. InsertBefore reports a missing anchor by
// returning false, and the fallback silently appends the step at the *end of the
// frame*, whatever phase it declared. A renamed core step, or an anchor that was
// never registered, therefore did not fail: it moved gameplay input to the last
// thing before present. The failure mode of an engine-owned timeline is not an
// error, it is a schedule that is wrong and looks fine.
//
// The inversion here is that a module *declares* its steps (ModuleStep: a phase,
// a name, a function and a placement intent) and something else resolves them.
// The resolver is FramePlan::Compose, which reports FramePlanError::AnchorMissing
// rather than guessing, and which applies a module's steps all-or-nothing so a
// failed install leaves the timeline exactly as it was.
//
// Who resolves is the composition root's business, which is the other half of
// the inversion. The engine still owns the plan for the frame it is running --
// something has to execute it -- but the plan it starts from is core's own
// steps, and every step beyond those arrives because the application installed
// the module that declared it. There is no registry of extension callbacks
// replayed on a schedule rebuild, and no way for a module to add a step that the
// application did not ask for.

#include <Zahlen/Common.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN {

class Engine;

enum class GameplayDriver : uint8_t { Cpp };

enum class GameplayStatus : int8_t { OK = 0, RequestQuit = 1, RequestReload = 2, Error = -1 };

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
    FramePhase       phase = FramePhase::Input;
    std::string_view name  = "UnnamedStep";
    FrameStepFn      run   = nullptr;
};

// Placement failures are values, not aborts. A module that names an anchor the
// host did not register is a composition error the application can report and
// still boot from; it is never a reason to place the step somewhere else.
enum class FramePlanError : uint8_t {
    NullStep,
    EmptyName,
    DuplicateName,
    AnchorMissing,
};

[[nodiscard]] constexpr auto ToString(FramePlanError error) noexcept -> std::string_view {
    switch (error) {
        case FramePlanError::NullStep: return "step has no function";
        case FramePlanError::EmptyName: return "step has no name";
        case FramePlanError::DuplicateName: return "step name is already in the plan";
        case FramePlanError::AnchorMissing: return "anchor step is not in the plan";
    }
    return "unknown frame plan error";
}

enum class StepPlacement : uint8_t { Append, Before, After };

// A step as a module declares it: what to run and where it wants to sit relative
// to a step the host owns. `anchor` is empty for StepPlacement::Append, and names
// a step already in the plan otherwise.
//
// A module anchors to the host's step names rather than to indices or to a
// sibling module, so an application free to install a subset of modules -- or to
// install them in a different order -- still gets the ordering each module asked
// for. That is the point of a name: it survives the composition changing around
// it.
struct ModuleStep {
    FramePhase       phase     = FramePhase::Input;
    std::string_view name;
    FrameStepFn      run       = nullptr;
    StepPlacement    placement = StepPlacement::Append;
    std::string_view anchor;
};

// The ordered timeline.
//
// The plan does not own the engine and the engine does not own the plan's
// contents: whoever composes it -- the composition root for the application's
// steps, SystemWiring for core's -- decides what is in it, and Engine::Tick only
// executes what it was given.
// ZHLN_API because the definitions are out of line and the callers are not all
// in the engine image: a module's Install reaches InstallModule, which composes
// into this, and FramePlan.cpp is compiled once into zahlen_engine. Off Windows
// the macro is default visibility and changes nothing; on Windows it is the
// dllexport that keeps the definition from being private to the DLL.
class ZHLN_API FramePlan {
  public:
    // Single-step forms of Compose. Reported, never repaired: a step that cannot
    // be placed is not placed at all, and the plan is left as it was.
    [[nodiscard]] auto Append(FramePhase phase, std::string_view name, FrameStepFn run) -> std::expected<void, FramePlanError>;
    [[nodiscard]] auto InsertBefore(std::string_view anchor, FramePhase phase, std::string_view name, FrameStepFn run) -> std::expected<void, FramePlanError>;
    [[nodiscard]] auto InsertAfter(std::string_view anchor, FramePhase phase, std::string_view name, FrameStepFn run) -> std::expected<void, FramePlanError>;

    // Place every declared step, in declaration order, resolving each one
    // against the plan as it stands plus the steps already placed in this call.
    // A module may therefore anchor to an earlier step of its own.
    //
    // All-or-nothing: the steps are placed into a copy, and the copy replaces
    // the plan only if every placement resolved. A half-installed module is
    // worse than an uninstalled one, because the half that did land is the half
    // nothing reports.
    [[nodiscard]] auto Compose(std::span<const ModuleStep> steps) -> std::expected<void, FramePlanError>;

    void Execute(Engine& engine, float dt, FrameContext& ctx) const;

    [[nodiscard]] auto Count() const noexcept -> size_t;
    [[nodiscard]] auto IsEmpty() const noexcept -> bool;
    [[nodiscard]] auto Steps() const noexcept -> std::span<const FrameStep>;
    [[nodiscard]] auto Find(std::string_view name) const noexcept -> const FrameStep*;
    [[nodiscard]] auto IndexOf(std::string_view name) const noexcept -> std::optional<size_t>;

    void Clear() noexcept;

  private:
    // The one placer. `steps` is the working copy Compose commits: an anchor
    // named by a later step in the same batch is found because the earlier one
    // was already placed in it.
    [[nodiscard]] static auto Place(std::vector<FrameStep>& steps, const ModuleStep& step) -> std::expected<void, FramePlanError>;

    std::vector<FrameStep> _steps;
};

} // namespace ZHLN
