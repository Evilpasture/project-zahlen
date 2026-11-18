// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Engine/FramePlan.hpp
//
// One frame's order of execution, as data instead of as a callback that reaches
// into the engine.
//
// An earlier scheduler let a module mutate the timeline (InsertBefore, with an
// append fallback). A missing anchor therefore did not fail: it silently moved
// the step to the end of the frame. The inversion here is that a module
// *declares* its steps -- ModuleStep: a phase, a name, a function and a
// placement intent -- and FramePlan::Compose resolves them, reporting
// FramePlanError::AnchorMissing rather than guessing and applying a module's
// steps all-or-nothing.
//
// The plan a frame starts from is core's own steps; every step beyond those
// exists because the application installed the module that declared it. No
// module can add a step the application did not ask for.

#include <Zahlen/Common.h>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <engine/Frame.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN {

class Engine;

// A frame step aborted the frame. FrameContext::error carries the real
// ErrorCode: a step that knows why it failed (device loss, swapchain
// recreation, an assertion) puts that code there. This is the generic
// reason for a step with nothing richer to report.
enum class FrameError : uint8_t {
    StepFailed ZHLN_ANNOTATION(ZHLN::Description<"A frame step aborted the frame."> {}) = 1,
};

enum class FramePhase : uint8_t {
    Input,
    UI,
    HotReload,
    PlayerIntent,
    Physics,
    Simulation,
    Camera,
    Visibility,
    Present,
    Fallback,
    History,
};

struct FrameContext {
    // std::nullopt means the frame succeeded. A step that cannot continue
    // sets the actual ErrorCode; FramePlan::Execute stops at the first
    // engaged error and Engine::Tick returns it as std::unexpected, so the
    // reason survives to the host instead of being flattened to -1.
    std::optional<ErrorCode> error = std::nullopt;

    // Computed once by Engine::Tick from the engine's Clock, before any step
    // runs: how many fixed simulation steps to execute, and the interpolation
    // alpha left over after draining them. Steps read these values instead of
    // poking a mutable float& into the engine.
    uint32_t   fixedSteps = 0;
    FrameAlpha alpha {};
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
// because the definitions are out of line and the callers are not all
// in the engine image: a module's Install reaches InstallModule, which composes
// into this, and FramePlan.cpp is compiled once into zahlen_engine. Off Windows
// the macro is default visibility and changes nothing; on Windows it is the
// dllexport that keeps the definition from being private to the DLL.
class FramePlan {
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
