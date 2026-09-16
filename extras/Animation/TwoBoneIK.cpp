// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Animation/TwoBoneIK.cpp
#include "IK.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>

namespace ZHLN::IK {

void ApplyTwoBoneIK(
    ECS::Registry& registry, Entity rootEntity, const ModelPrefab& prefab, std::span<const JPH::Mat44> localTransforms, std::vector<JPH::Mat44>& worldTransforms
) {
    auto& reg = registry;

    if (auto* ikComp = reg.Get<TwoBoneIKComponent>(rootEntity)) {
        for (auto& chain: ikComp->chains) {
            if (chain.weight <= 0.001f || chain.upperNodeIndex < 0 || chain.lowerNodeIndex < 0 || chain.endNodeIndex < 0) {
                continue;
            }
            if (chain.upperNodeIndex >= static_cast<int32_t>(prefab.nodes.size()) ||
                chain.lowerNodeIndex >= static_cast<int32_t>(prefab.nodes.size()) || chain.endNodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
                continue;
            }

            JPH::Vec3 solvedTargetPos = chain.targetPosition;
            JPH::Quat solvedTargetRot = chain.targetRotation;

            if (chain.targetEntity != Entity::Null() && reg.IsAlive(chain.targetEntity)) {
                if (auto* tTrans = reg.Get<Components::TransformComponent>(chain.targetEntity)) {
                    JPH::Mat44 tMat = tTrans->GetLocalMatrix();
                    solvedTargetPos = tMat * chain.targetOffset;
                    solvedTargetRot = tTrans->rotation;
                }
            }

            JPH::Mat44 upperWorld = worldTransforms[chain.upperNodeIndex];
            JPH::Mat44 lowerWorld = worldTransforms[chain.lowerNodeIndex];
            JPH::Mat44 endWorld   = worldTransforms[chain.endNodeIndex];

            JPH::Vec3 pUpper = upperWorld.GetTranslation();
            JPH::Vec3 pLower = lowerWorld.GetTranslation();
            JPH::Vec3 pEnd   = endWorld.GetTranslation();

            float l1 = (pLower - pUpper).Length();
            float l2 = (pEnd - pLower).Length();

            TwoBoneIKSolverInput ikInput = {
                .upperPosition = pUpper, .targetPosition = solvedTargetPos, .poleVector = chain.poleVector, .upperLength = l1, .lowerLength = l2
            };

            TwoBoneIKSolverOutput ikOutput = SolveTwoBoneIK(ikInput);

            if (ikOutput.valid) {
                JPH::Vec3 localUpperDir = (localTransforms[chain.lowerNodeIndex].GetTranslation()).Normalized();
                JPH::Vec3 localLowerDir = (localTransforms[chain.endNodeIndex].GetTranslation()).Normalized();

                JPH::Mat44 newUpperWorld = AlignNodeToDirection(upperWorld, localUpperDir, ikOutput.upperDirection);
                JPH::Mat44 newLowerWorld = JPH::Mat44::sRotationTranslation(lowerWorld.GetQuaternion(), ikOutput.midPosition);
                newLowerWorld            = AlignNodeToDirection(newLowerWorld, localLowerDir, ikOutput.lowerDirection);

                JPH::Mat44 newEndWorld = endWorld;
                newEndWorld.SetTranslation(ikOutput.endPosition);
                if (chain.orientEndEffector) {
                    newEndWorld = JPH::Mat44::sRotationTranslation(solvedTargetRot, ikOutput.endPosition);
                }

                float w        = std::clamp(chain.weight, 0.0f, 1.0f);
                auto  BlendMat = [](const JPH::Mat44& a, const JPH::Mat44& b, float t) {
                    JPH::Vec3 tA = a.GetTranslation();
                    JPH::Vec3 tB = b.GetTranslation();
                    JPH::Quat rA = a.GetQuaternion().Normalized();
                    JPH::Quat rB = b.GetQuaternion().Normalized();
                    return JPH::Mat44::sRotationTranslation(rA.SLERP(rB, t), tA + t * (tB - tA));
                };

                worldTransforms[chain.upperNodeIndex] = BlendMat(upperWorld, newUpperWorld, w);
                worldTransforms[chain.lowerNodeIndex] = BlendMat(lowerWorld, newLowerWorld, w);
                worldTransforms[chain.endNodeIndex]   = BlendMat(endWorld, newEndWorld, w);
            }
        }
    }
}

void Install(Engine& engine) {
    engine.GetRegistry().RegisterComponent<TwoBoneIKComponent>();
    engine.SetBonePosePostProcessor(&ApplyTwoBoneIK);
}

} // namespace ZHLN::IK
