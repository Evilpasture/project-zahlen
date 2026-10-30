// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/CharacterController/CharacterController.cpp
#include "CharacterController.hpp"

#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/GameModule.hpp"
#include "Zahlen/Log.hpp"
#include "Zahlen/FramePlan.hpp"
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <algorithm>
#include <optional>

namespace ZHLN::Character {

namespace {

// --- Frame-phase contributions -----------------------------------------------

void CharacterInputStep(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    static PlayerInputSystem inputSystem;
    inputSystem.Update(engine);
}

// Translate gameplay input using the previous resolved camera. Camera
// transforms are finalized after physics and the update graph so rig-driven
// first-person views cannot lag one simulation frame behind their body.
//
// Late input latch: the OS event source is re-pumped and the raw state is
// re-translated immediately before intent is derived, so physics steers off
// device state sampled microseconds ago instead of the frame-top pump (the UI
// and hot-reload steps ran in between). Re-translation is a pure overwrite of
// the InputComponents, and the pump only refreshes levels and accumulates
// motion/wheel deltas, so running both twice per frame neither double-applies
// nor loses anything.
void PlayerIntentStep(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    static PlayerInputSystem inputSystem;
    engine.PollLateInput();
    inputSystem.Update(engine);
    // Translation is relative to the view; a world with no camera has none, and the
    // raw intents stay latched for the frame that does.
    if (const auto camera = engine.GetRegistry().GetSingleton<Components::CameraComponent>()) {
        inputSystem.PlayerInputTranslate(engine, camera->camera);
    }
}

// The steps this module contributes, declared rather than inserted. The input
// step ran first in the core schedule, and anchoring before "HostUICallback" is
// that position restored now that core no longer registers it; player intent
// sits after hot-reload dispatch and before Physics, where the core
// PlayerIntent phase step used to.
//
// Inserting these from a callback is what this replaces. Both used to fall back
// to a plain append when their anchor was missing, which put character input at
// the end of the frame -- after rendering -- and said nothing. FramePlan::Compose
// reports the missing anchor instead, and Engine::InstallModule hands that error
// back here.
constexpr ModuleStep kFrameSteps[] = {
    {
        .phase     = FramePhase::Input,
        .name      = "CharacterInput",
        .run       = &CharacterInputStep,
        .placement = StepPlacement::Before,
        .anchor    = "HostUICallback",
    },
    {
        .phase     = FramePhase::PlayerIntent,
        .name      = "PlayerInputTranslate",
        .run       = &PlayerIntentStep,
        .placement = StepPlacement::After,
        .anchor    = "StateAndShaderReload",
    },
};

void RegisterComponents(ECS::Registry& reg) {
    reg.RegisterComponent<MovementComponent>();
    reg.RegisterComponent<InputComponent>();
}

// --- Physics substep hooks ----------------------------------------------------

void CharacterPreStep(Engine& engine, float dt) {
    MovementSystem(engine, dt);
    CommitCharacterSteering(engine);
    // Prop interaction runs with the steering it depends on: the impulse is
    // computed from the character velocity committed into the body just above.
    PushProps(engine.GetPhysicsContext(), engine.GetRegistry());
}

// --- Update-graph contributions ------------------------------------------------

// Character yaw is integrated at the physics tick rate inside
// MovementComponent, not in transform history, so its render-time rotation
// must SLERP between prevOrientation and orientation with the substep alpha
// -- the same math VisualInterpolationSystem used to inline for entities
// carrying a MovementComponent. Runs after VisualInterpolationSystem (which
// wrote the position and the fallback rotation) and before TransformSystem
// resolves world transforms: AddSystemBefore("TransformSystem") gives it
// both orderings, because Compile() only builds edges from earlier nodes to
// later ones.
void CharacterOrientationInterpolation(
    ECS::Query<const MovementComponent, const Components::PhysicsComponent, Components::TransformComponent&> query,
    FrameAlpha alpha
) {
    const float clampedAlpha = std::clamp(alpha.value, 0.0f, 1.0f);
    query.ForEach([&](Entity, const auto& move, const auto& phys, auto& transform) {
        if (!phys.isStatic) {
            transform.rotation = move.prevOrientation.SLERP(move.orientation, clampedAlpha);
        }
    });
}

void AddGraphSystems(SimGraph& updateGraph, RenderGraph& /*renderGraph*/) {
    // Imperative writers of MovementComponent run before this graph executes:
    // PlayerInputTranslate (PlayerIntent phase) and the physics substep hooks
    // (MovementSystem + grounded write-back). The anchor gives hazard
    // analysis a writer to hang this graph's readers off. Declared before the
    // nodes below so Compile() can order them after it.
    updateGraph.DeclareExternalWrites(
        "ExternalCharacterWrites", {
                                       ECS::Write<MovementComponent>(),
                                   }
    );

    if (!updateGraph.AddSystemBefore<&CharacterOrientationInterpolation>("TransformSystem")) {
        // Anchor missing (a host trimmed the core graph): append instead.
        updateGraph.AddSystem<&CharacterOrientationInterpolation>();
    }
}

} // namespace

auto Describe() -> Module {
    return Module{
        .name               = "CharacterController",
        .registerComponents = &RegisterComponents,
        .frameSteps         = kFrameSteps,
        .addGraphSystems    = &AddGraphSystems,
    };
}

void Install(Engine& engine) {
    // The physics substep hooks are engine-owned lifecycle, not a contribution:
    // they run inside PhysicsSystem's fixed-timestep loop, which no module can
    // express as a frame step. See Engine::PhysicsSubstepHooks.
    engine.SetPhysicsSubstepHooks({.preStep = &CharacterPreStep, .postStep = &WriteCharacterGrounded});

    // A host that wants the placement error itself calls Describe() and
    // Engine::InstallModule directly; Install is the two-line convenience and
    // reports through the log.
    if (auto installed = engine.InstallModule(Describe()); !installed) {
        LogError("[Character] module install failed: {}", ToString(installed.error()));
    }
}

} // namespace ZHLN::Character
