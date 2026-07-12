// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "physics/Physics.hpp"
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/render/RenderCode.hpp>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN {
class RenderContext;
class CreativeWorksManager;
namespace ECS {
class Registry;
}
} // namespace ZHLN

namespace ZHLN::CreativeWorksFactory {
Mesh CreateTetrahedron(RenderContext& ctx);
Mesh CreatePlane(RenderContext& ctx, float extent = 10.0f, const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f});
Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f});

[[nodiscard]] std::expected<Material, Error> CreateBasicMaterial(RenderContext& ctx, bool doubleSided = false, bool alphaBlend = false);

Mesh CreateTerrain(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight, float* outHeights);

uint32_t CreateFontAtlasTexture(RenderContext& ctx);

Mesh     LoadCookedMesh(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view virtualPath);
uint32_t LoadCookedTexture(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view virtualPath);

struct SpawnParams {
    JPH::RVec3 position = JPH::RVec3::sZero();
    JPH::Quat  rotation = JPH::Quat::sIdentity();
    JPH::Vec3  scale    = JPH::Vec3::sReplicate(1.0f);

    bool     createPhysics   = false;
    bool     useBoxColliders = false;
    bool     isStaticPhysics = true;
    bool     isAnimated      = false;
    uint32_t physicsCategory = 0xFFFFFFFF;
    uint32_t physicsMask     = 0xFFFFFFFF;

    Material materialOverride = {.pipeline = PipelineHandle::Invalid};
};

ModelPrefab* LoadModelPrefab(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path);

// Returns the number of entities actually spawned and populated into outBuffer (if provided)
uint32_t InstantiatePrefab(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext&    pc,
    const ModelPrefab& prefab,
    const SpawnParams& params,
    Entity*            outBuffer = nullptr,
    uint32_t           maxCount  = 0
);

void SetupPlayerRagdoll(RenderContext& rc, PhysicsContext& pc, ECS::Registry& reg, Entity playerEntity, std::span<const Entity> visualParts);
} // namespace ZHLN::CreativeWorksFactory
