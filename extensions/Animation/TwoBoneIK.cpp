// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "IK.hpp"
#include "AnimationContext.hpp"
#include "AnimationSystem.hpp"
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <array>

namespace ZHLN::IK {

namespace {

void RegisterComponents(ECS::Registry& registry) {
    registry.RegisterComponent<TwoBoneIKComponent>();
}

void AddSystems(ECS::SimGraph& updateGraph, ECS::RenderGraph&) {
    if (!updateGraph.AddSystemBefore<&TwoBoneIKSystem::Update>("AnimationStepSystem")) {
        updateGraph.AddSystem<&TwoBoneIKSystem::Update>();
    }
}

} // namespace

void TwoBoneIKSystem::Update(
    ECS::Query<const Components::AnimatorComponent, const TwoBoneIKComponent, const Components::TransformComponent> query,
    ECS::ResMut<Animation::AnimationContext>                                                                        anim
) {
    for (const Entity entity: query.Entities<TwoBoneIKComponent>()) {
        const auto animComp = query.Get<Components::AnimatorComponent>(entity);
        const auto ikComp   = query.Get<TwoBoneIKComponent>(entity);
        if (!animComp || !ikComp || !animComp->animationHandle.IsValid()) {
            continue;
        }

        JPH::Mat44 invEntityLocal = JPH::Mat44::sIdentity();
        JPH::Quat  invEntityRot   = JPH::Quat::sIdentity();
        if (const auto entityTr = query.Get<Components::TransformComponent>(entity)) {
            invEntityLocal = entityTr->GetLocalMatrix().Inversed();
            invEntityRot   = entityTr->rotation.Normalized().Conjugated();
        }

        std::array<Animation::IKChain, kMaxTwoBoneIKChains> translated {};
        size_t                                              count = 0;

        for (const TwoBoneIKChain& src: ikComp->ActiveChains()) {
            if (count >= translated.size()) {
                break;
            }
            JPH::Vec3 targetPosModel = src.targetPosition;
            JPH::Quat targetRotModel = src.targetRotation;

            if (src.targetEntity != Entity::Null()) {
                if (const auto targetTr = query.Get<Components::TransformComponent>(src.targetEntity)) {
                    const JPH::Mat44 targetInModel = invEntityLocal * targetTr->GetLocalMatrix();
                    targetPosModel                 = targetInModel * src.targetOffset;
                    targetRotModel                 = (invEntityRot * targetTr->rotation).Normalized();
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

        anim->SetIKChains(animComp->animationHandle, std::span<const Animation::IKChain> {translated.data(), count});
    }
}

auto Describe() noexcept -> Module {
    return Module {
        .name               = "TwoBoneIK",
        .registerComponents = &RegisterComponents,
        .addGraphSystems    = &AddSystems,
    };
}

void Install(Engine& engine) {
    Animation::Install(engine);
    if (auto installed = engine.InstallModule(Describe()); !installed) {
        ZHLN::LogError("[TwoBoneIK] module install failed: {}", ToString(installed.error()));
    }
}

} // namespace ZHLN::IK
