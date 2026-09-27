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
}

using BonePosePostProcessor = void (*)(
    ECS::Registry& registry, Entity rootEntity, const ModelPrefab& prefab, std::span<const JPH::Mat44> localTransforms, std::vector<JPH::Mat44>& worldTransforms
);

struct SystemContext {
    ECS::Registry& registry;

    RenderContext*     render       = nullptr;
    PhysicsContext*    physics      = nullptr;
    AudioContext*      audio        = nullptr;
    Camera*            camera       = nullptr;
    CullingSystem*     culling      = nullptr;
    ArticulationSystem* articulation = nullptr;

    BonePosePostProcessor bonePosePostProcessor = nullptr;

    JPH::Array<Entity>* visibleEntities       = nullptr;
    JPH::Array<Entity>* visibleShadowEntities = nullptr;

    uint64_t frame = 0;
    float    alpha = 0.0f;
    float    dt    = 0.0f;
};

}
