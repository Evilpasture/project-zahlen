// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Animator.hpp"
#include <Animation/Components.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <cmath>
namespace ZHLN::Animator {
namespace {
bool Usable(COM::IEntities& entities, Entity entity) {
    return entities.IsAlive(entity) && !entities.Get<Components::PendingDestroy>(entity) && entities.Get<Animation::AnimatorComponent>(entity);
}
} // namespace
bool Attach(COM::IEntities& entities, Entity entity) {
    if (!Usable(entities, entity))
        return false;
    if (entities.Get<Animation::AnimatorControlComponent>(entity))
        return true;
    // Copy controls, never the extension's generational runtime handle.
    const auto*                               state = entities.Get<Animation::AnimatorComponent>(entity);
    const Animation::AnimatorControlComponent control {
        .trackIndex = state->currentTrackIdx, .playbackSpeed = state->currentPlaybackSpeed, .blendDuration = state->blendDuration, .loop = state->currentLoop
    };
    return entities.Attach(entity, control) != nullptr;
}
bool Read(COM::IEntities& entities, Entity entity, PlaybackSnapshot& out) {
    if (!Usable(entities, entity))
        return false;
    const auto* state = entities.Get<Animation::AnimatorComponent>(entity);
    out               = {
        .trackIndex = state->currentTrackIdx,
        .time       = state->currentTrackTime,
        .speed      = state->currentPlaybackSpeed,
        .loop       = state->currentLoop,
        .finished   = state->isFinished
    };
    return true;
}
bool Play(COM::IEntities& entities, Entity entity, int32_t track, float blendDuration, bool loop) {
    if (track < -1 || !std::isfinite(blendDuration) || blendDuration < 0 || !Attach(entities, entity))
        return false;
    auto* control          = entities.Get<Animation::AnimatorControlComponent>(entity);
    control->trackIndex    = track;
    control->blendDuration = blendDuration;
    control->loop          = loop;
    control->seekTime      = 0;
    control->seekRequested = true;
    return true;
}
bool Seek(COM::IEntities& entities, Entity entity, float seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || !Attach(entities, entity))
        return false;
    auto* control          = entities.Get<Animation::AnimatorControlComponent>(entity);
    control->seekTime      = seconds;
    control->seekRequested = true;
    return true;
}
bool Speed(COM::IEntities& entities, Entity entity, float multiplier) {
    if (!std::isfinite(multiplier) || !Attach(entities, entity))
        return false;
    entities.Get<Animation::AnimatorControlComponent>(entity)->playbackSpeed = multiplier;
    return true;
}
bool SetIK(COM::IEntities& entities, Entity entity, const IK::TwoBoneIKComponent& ik) {
    if (!Usable(entities, entity) || ik.chainCount > ik.chains.size())
        return false;
    for (const auto& chain: ik.ActiveChains()) {
        if (chain.upperNodeIndex < 0 || chain.lowerNodeIndex < 0 || chain.endNodeIndex < 0 || !std::isfinite(chain.weight) || chain.weight < 0 ||
            chain.weight > 1 || !std::isfinite(chain.targetPosition.LengthSquared()) || !std::isfinite(chain.targetRotation.LengthSquared()) ||
            !std::isfinite(chain.poleVector.LengthSquared()) || !std::isfinite(chain.targetOffset.LengthSquared()))
            return false;
        if (chain.targetEntity != Entity::Null() && !entities.IsAlive(chain.targetEntity))
            return false;
    }
    return entities.Attach(entity, ik) != nullptr;
}
void Detach(COM::IEntities& entities, Entity entity) {
    entities.Detach<Animation::AnimatorControlComponent>(entity);
    entities.Detach<IK::TwoBoneIKComponent>(entity);
}
} // namespace ZHLN::Animator
