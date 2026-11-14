// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <cstdint>

namespace ZHLN {
class ArticulationSystem;
class GraphicsDevice;
class PhysicsContext;
struct ModelPrefab;
namespace ECS { class Registry; }
namespace PrefabFactory {
// Engine's device-recovery barrier and internal spawners.
void RebuildVulkanResources(GraphicsDevice gfx, ECS::Registry& registry);
auto CreateBoxInternal(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, ZHLN::FVec3 halfExtents, const SpawnParams& params) -> Entity;
auto CreateSphereInternal(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, const SpawnParams& params) -> Entity;
auto CreateCylinderInternal(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, float height, const SpawnParams& params) -> Entity;
auto CreateConeInternal(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, float height, const SpawnParams& params) -> Entity;
auto CreatePlaneInternal(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float extent, ZHLN::FVec4 color, const SpawnParams& params) -> Entity;
auto InstantiatePrefabInternal(
    GraphicsDevice gfx, ECS::Registry& reg, PhysicsContext& pc, ArticulationSystem& art, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount
) -> uint32_t;
}
}
