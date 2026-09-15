// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <cstdint>

namespace ZHLN {

class RenderContext;
class PhysicsContext;
class AudioContext;
class CullingSystem;
class ArticulationSystem;
struct Camera;

namespace ECS {
class Registry;
} // namespace ECS

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

    JPH::Array<Entity>* visibleEntities       = nullptr;
    JPH::Array<Entity>* visibleShadowEntities = nullptr;

    uint64_t frame = 0;
    float    alpha = 0.0f;
    float    dt    = 0.0f;
};

} // namespace ZHLN
