// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Animation/AnimationContext.hpp>
#include <Animation/Components.hpp>
#include <Animation/IK.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <array>
#include <span>

namespace ZHLN::IK {

void ApplyTwoBoneIK(const COM::IEntities& entities, Animation::AnimationContext& anim) {
    for (const Entity entity : entities.EntitiesWith<TwoBoneIKComponent>()) {
        const auto* animComp = entities.Get<Animation::AnimatorComponent>(entity);
        const auto* ikComp   = entities.Get<TwoBoneIKComponent>(entity);
        if (animComp == nullptr || ikComp == nullptr || !animComp->animationID.IsValid()) continue;

        auto invEntityLocal = Mat4x4::Identity();
        auto invEntityRot   = Quat::Identity();
        if (const auto* entityTr = entities.Get<Components::TransformComponent>(entity)) {
            invEntityLocal = Inverse(ComposeTransform(entityTr->position, entityTr->rotation, entityTr->scale));
            invEntityRot   = entityTr->rotation.Normalized().Conjugated();
        }

        std::array<Animation::IKChain, kMaxTwoBoneIKChains> translated {};
        size_t                                              count = 0;

        for (const TwoBoneIKChain& src : ikComp->ActiveChains()) {
            if (count >= translated.size()) break;
            FVec3 targetPosModel = src.targetPosition;
            auto  targetRotModel = src.targetRotation;

            if (src.targetEntity != Entity::Null()) {
                if (const auto* targetTr = entities.Get<Components::TransformComponent>(src.targetEntity)) {
                    const auto targetInModel = invEntityLocal * ComposeTransform(targetTr->position, targetTr->rotation, targetTr->scale);
                    targetPosModel           = (targetInModel * FVec4 {src.targetOffset.x, src.targetOffset.y, src.targetOffset.z, 1.0f}).XYZ();
                    targetRotModel           = (invEntityRot * targetTr->rotation).Normalized();
                }
            }

            translated[count++] = Animation::IKChain {
                .upperNodeIndex    = src.upperNodeIndex,
                .lowerNodeIndex    = src.lowerNodeIndex,
                .endNodeIndex      = src.endNodeIndex,
                .targetPosition    = targetPosModel,
                .targetRotation    = targetRotModel,
                .poleVector        = src.poleVector,
                .weight            = src.weight,
                .orientEndEffector = src.orientEndEffector,
            };
        }

        anim.SetIKChains(animComp->animationID, std::span<const Animation::IKChain> {translated.data(), count});
    }
}

} // namespace ZHLN::IK
