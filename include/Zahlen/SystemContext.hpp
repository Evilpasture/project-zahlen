// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN {

class RenderContext;
class PhysicsContext;
class AudioContext;
class CullingSystem;
class ArticulationSystem;
struct Camera;
struct ModelPrefab;

namespace ECS {
class Registry;
} // namespace ECS

/// Post-processes one skeleton's bone world matrices after the animated local
/// pose is resolved and before the matrices are uploaded (GPUJoint). This is
/// the injection point for animation modifiers that must run *inside* the
/// skinning pipeline -- analytic IK, for example, which adjusts the upper/lower
/// chain bones between pose evaluation and upload and therefore cannot be an
/// ordinary system-graph node. Core ships no implementation; optional layers
/// (extras/Animation's two-bone IK) install one through
/// Engine::SetBonePosePostProcessor. Null when no layer is installed: the pose
/// passes through untouched.
using BonePosePostProcessor = void (*)(
    ECS::Registry& registry, Entity rootEntity, const ModelPrefab& prefab, std::span<const JPH::Mat44> localTransforms, std::vector<JPH::Mat44>& worldTransforms
);

/// Everything a SystemGraph system may consume during one execution.
///
/// The registry is mandatory -- a system without a world is nothing. The
/// hardware/infrastructure services are pointers on purpose: graphs must stay
/// executable in reduced environments (ECS-only unit tests, headless logic
/// stepping) where no RenderContext or AudioContext exists. A system that
/// declares a service in its body simply dereferences the service it needs;
/// Engine::MakeSystemContext fills all of them for real frames.
struct SystemContext {
    ECS::Registry& registry;

    RenderContext*     render       = nullptr;
    PhysicsContext*    physics      = nullptr;
    AudioContext*      audio        = nullptr;
    Camera*            camera       = nullptr;
    CullingSystem*     culling      = nullptr;
    /// Must be the SAME instance DespawnEntity releases ragdolls through
    /// (World::GetArticulationSystem): its tracking ledger is the shared state.
    ArticulationSystem* articulation = nullptr;

    /// Optional animation modifier installed by the composition root (see the
    /// BonePosePostProcessor docs). Engine::MakeSystemContext forwards the
    /// engine-level hook here so graph systems never need an Engine&.
    BonePosePostProcessor bonePosePostProcessor = nullptr;

    JPH::Array<Entity>* visibleEntities       = nullptr;
    JPH::Array<Entity>* visibleShadowEntities = nullptr;

    uint64_t frame = 0;
    float    alpha = 0.0f;
    float    dt    = 0.0f;
};

} // namespace ZHLN
