// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Animation/NativeAnimationContext.hpp>
#include <Zahlen/Animation/NativeAnimationSystem.hpp>
#include <Zahlen/Animation/NativeIK.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <array>

namespace ZHLN::IK {

void TwoBoneIKSystem::Update(
    ECS::Query<const Components::AnimatorComponent, const TwoBoneIKComponent, const Components::TransformComponent> query,
    ECS::ResMut<Animation::AnimationContext>                                                                        anim
) {
    for (const Entity entity: query.Entities<TwoBoneIKComponent>()) {
        const auto animComp = query.Get<Components::AnimatorComponent>(entity);
        const auto ikComp   = query.Get<TwoBoneIKComponent>(entity);
        if (!animComp || !ikComp || !animComp->animationID.IsValid()) {
            continue;
        }

        auto invEntityLocal = Mat4x4::Identity();
        auto invEntityRot   = Quat::Identity();
        if (const auto entityTr = query.Get<Components::TransformComponent>(entity)) {
            invEntityLocal = Inverse(ComposeTransform(entityTr->position, entityTr->rotation, entityTr->scale));
            invEntityRot   = entityTr->rotation.Normalized().Conjugated();
        }

        std::array<Animation::IKChain, kMaxTwoBoneIKChains> translated {};
        size_t                                              count = 0;

        for (const TwoBoneIKChain& src: ikComp->ActiveChains()) {
            if (count >= translated.size()) {
                break;
            }
            FVec3 targetPosModel = src.targetPosition;
            auto  targetRotModel = src.targetRotation;

            if (src.targetEntity != Entity::Null()) {
                if (const auto targetTr = query.Get<Components::TransformComponent>(src.targetEntity)) {
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

        anim->SetIKChains(animComp->animationID, std::span<const Animation::IKChain> {translated.data(), count});
    }
}

auto Describe() noexcept -> Module {
    return Module {.name = "TwoBoneIK"};
}

void Install(Engine&) {
    // Native compatibility: the engine owns component registration and IK.
}

} // namespace ZHLN::IK
