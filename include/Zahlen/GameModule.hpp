// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/GameModule.hpp
//
// What a module *is*, as a value the application installs.
//
// A module used to be an entry point that grabbed the engine:
//
//     void Install(Engine& engine) {
//         engine.AddFrameSchedulerExtension(&AddFrameSteps);
//         engine.AddSystemGraphsExtension(&AddGraphSystems);
//     }
//
// Two things were wrong with that, and they are different things. The first is
// ordering: the engine kept the callbacks and replayed them on every schedule
// rebuild, so a module's steps landed wherever its insertion found an anchor --
// see the fallback FramePlan.hpp describes. The second is authority: nothing in
// the engine or in the application could say what was installed, in what order,
// or why. An Install that only mutates its argument is a program that changes
// the engine without a value describing the change.
//
// Module is that value. It names the module, carries its frame steps as
// declarations, and points at the functions that register its components, add
// its graph systems, clean up after its scene, and contribute its script verbs.
// Engine::InstallModule applies one, reports a placement error instead of
// absorbing it, and keeps the value, so the application can ask what is running.
//
// The frame steps are declarations and not a callback that mutates a scheduler,
// which is what keeps the module from knowing *who* resolves them. A module
// describes its placement intent; the plan decides whether the intent can be
// honoured.

#include <Zahlen/FramePlan.hpp>
#include <Zahlen/SystemContext.hpp>
#include <span>
#include <string_view>

namespace ZHLN {

namespace ECS {
class Registry;
}

class Engine;

// Hooks a module contributes to engine-owned lifecycles. They live here rather
// than on Engine so that a module is describable without including Engine, and
// Engine declares its own spellings of them as aliases.
using TeardownHook     = void (*)(Engine& engine);
using SceneCleanupPass = void (*)(Engine& engine, bool all);
using GraphSystemsFn   = void (*)(SimGraph& updateGraph, RenderGraph& renderGraph);

// Component registration is a function rather than a list of names because a
// family id is a compile-time hash of the type: the registry is addressed by
// type, so the module that owns a component is the only thing that can register
// it.
using ComponentRegistrar = void (*)(ECS::Registry& registry);

struct Module {
    std::string_view name;

    // Null when the module owns no components of its own.
    ComponentRegistrar registerComponents = nullptr;

    // The module's frame steps, as declarations. Empty by default: a module that
    // only supplies graph systems or a script verb contributes nothing to the
    // timeline, and saying so by leaving the span empty is clearer than a
    // callback that would be a no-op.
    std::span<const ModuleStep> frameSteps {};

    // Runs after core's own nodes when the simulation and render graphs are
    // (re)built, so a module can order its systems against core's by declaring
    // the component hazards its systems carry.
    GraphSystemsFn addGraphSystems = nullptr;

    // Released while the registry is still whole, before the scene's entities
    // and owned resources go.
    SceneCleanupPass sceneCleanup = nullptr;

    // Runs when the engine is torn down, for the module's process-global state.
    TeardownHook teardown = nullptr;
};

} // namespace ZHLN
