// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/FramePlan.hpp>

#include <utility>

namespace ZHLN {

auto FramePlan::Place(std::vector<FrameStep>& steps, const ModuleStep& step) -> std::expected<void, FramePlanError> {
    if (step.run == nullptr) {
        return std::unexpected(FramePlanError::NullStep);
    }
    if (step.name.empty()) {
        return std::unexpected(FramePlanError::EmptyName);
    }
    for (const FrameStep& placed: steps) {
        if (placed.name == step.name) {
            return std::unexpected(FramePlanError::DuplicateName);
        }
    }

    const FrameStep placed {.phase = step.phase, .name = step.name, .run = step.run};

    switch (step.placement) {
        case StepPlacement::Append: {
            steps.push_back(placed);
            return {};
        }
        case StepPlacement::Before:
        case StepPlacement::After: {
            for (size_t i = 0; i < steps.size(); ++i) {
                if (steps[i].name != step.anchor) {
                    continue;
                }
                const size_t at = step.placement == StepPlacement::Before ? i : i + 1;
                steps.insert(steps.begin() + static_cast<std::ptrdiff_t>(at), placed);
                return {};
            }
            // The anchor is a name the host was supposed to have. Appending here
            // would be the old scheduler's fallback, which is exactly the silent
            // misplacement this plan exists to report.
            return std::unexpected(FramePlanError::AnchorMissing);
        }
    }
    return std::unexpected(FramePlanError::NullStep);
}

auto FramePlan::Compose(std::span<const ModuleStep> steps) -> std::expected<void, FramePlanError> {
    std::vector<FrameStep> working = _steps;
    for (const ModuleStep& step: steps) {
        if (auto placed = Place(working, step); !placed) {
            return std::unexpected(placed.error());
        }
    }
    _steps = std::move(working);
    return {};
}

auto FramePlan::Append(FramePhase phase, std::string_view name, FrameStepFn run) -> std::expected<void, FramePlanError> {
    const ModuleStep step {.phase = phase, .name = name, .run = run, .placement = StepPlacement::Append, .anchor = {}};
    return Compose(std::span<const ModuleStep>(&step, 1));
}

auto FramePlan::InsertBefore(std::string_view anchor, FramePhase phase, std::string_view name, FrameStepFn run)
    -> std::expected<void, FramePlanError> {
    const ModuleStep step {.phase = phase, .name = name, .run = run, .placement = StepPlacement::Before, .anchor = anchor};
    return Compose(std::span<const ModuleStep>(&step, 1));
}

auto FramePlan::InsertAfter(std::string_view anchor, FramePhase phase, std::string_view name, FrameStepFn run)
    -> std::expected<void, FramePlanError> {
    const ModuleStep step {.phase = phase, .name = name, .run = run, .placement = StepPlacement::After, .anchor = anchor};
    return Compose(std::span<const ModuleStep>(&step, 1));
}

void FramePlan::Execute(Engine& engine, float dt, FrameContext& ctx) const {
    // A frame runs the steps that were in the plan when it started: a step that
    // composes into this plan -- a host callback installing a module, say --
    // takes effect on the next tick. The bound is re-read because a Clear()
    // during the frame would otherwise be indexed straight past the end.
    const size_t stepCount = _steps.size();
    for (size_t i = 0; i < stepCount; ++i) {
        if (i >= _steps.size()) {
            break;
        }
        if (const FrameStepFn run = _steps[i].run; run != nullptr) {
            run(engine, dt, ctx);
        }
    }
}

auto FramePlan::Count() const noexcept -> size_t {
    return _steps.size();
}

auto FramePlan::IsEmpty() const noexcept -> bool {
    return _steps.empty();
}

auto FramePlan::Steps() const noexcept -> std::span<const FrameStep> {
    return _steps;
}

auto FramePlan::Find(std::string_view name) const noexcept -> const FrameStep* {
    for (const FrameStep& step: _steps) {
        if (step.name == name) {
            return &step;
        }
    }
    return nullptr;
}

auto FramePlan::IndexOf(std::string_view name) const noexcept -> std::optional<size_t> {
    for (size_t i = 0; i < _steps.size(); ++i) {
        if (_steps[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

void FramePlan::Clear() noexcept {
    _steps.clear();
}

} // namespace ZHLN
