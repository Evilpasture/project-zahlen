// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/CharacterController/CharacterController.cpp
#include "CharacterController.hpp"

#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/FrameScheduler.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
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
    inputSystem.PlayerInputTranslate(engine, engine.GetCamera());
}

void AddFrameSteps(FrameScheduler& scheduler) {
    // The input step ran first in the core schedule; InsertBefore
    // "HostUICallback" restores that position now that the core no longer
    // registers it.
    if (!scheduler.InsertBefore("HostUICallback", FramePhase::Input, "CharacterInput", &CharacterInputStep)) {
        scheduler.Add(FramePhase::Input, "CharacterInput", &CharacterInputStep);
    }
    // Player intent sits after hot-reload dispatch and before Physics,
    // exactly where the core PlayerIntent phase step used to.
    if (!scheduler.InsertAfter("ScriptAndShaderReload", FramePhase::PlayerIntent, "PlayerInputTranslate", &PlayerIntentStep)) {
        scheduler.Add(FramePhase::PlayerIntent, "PlayerInputTranslate", &PlayerIntentStep);
    }
}

// --- Physics substep hooks ----------------------------------------------------

void CharacterPreStep(Engine& engine, float dt) {
    MovementSystem(engine, dt);
    CommitCharacterSteering(engine);
    // Prop interaction runs with the steering it depends on: the impulse is
    // computed from the character velocity committed into the body just above.
    PushProps(engine.GetPhysicsContext(), engine.GetRegistry());
}

// --- Free-cam speed query -----------------------------------------------------

std::optional<float> QueryFreeCamSpeed(ECS::Registry& reg, Entity target) {
    if (auto move = reg.Get<MovementComponent>(target)) {
        return move->speed;
    }
    return std::nullopt;
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

void AddGraphSystems(ECS::SystemGraph& updateGraph, ECS::SystemGraph& /*renderGraph*/) {
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

void Install(Engine& engine) {
    auto& reg = engine.GetRegistry();
    reg.RegisterComponent<MovementComponent>();
    reg.RegisterComponent<InputComponent>();

    engine.SetCharacterStepHooks({.preStep = &CharacterPreStep, .postStep = &WriteCharacterGrounded});
    engine.SetFreeCamSpeedQuery(&QueryFreeCamSpeed);
    engine.AddFrameSchedulerExtension(&AddFrameSteps);
    engine.AddSystemGraphsExtension(&AddGraphSystems);
}

} // namespace ZHLN::Character
