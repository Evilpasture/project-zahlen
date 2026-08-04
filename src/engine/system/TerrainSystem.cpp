// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerrainSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <string>

namespace ZHLN {

void TerrainSystem::Update(Engine& engine, float /*dt*/) {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    auto entities = reg.GetEntitiesWith<Components::TerrainComponent>();
    auto terrains = reg.GetRawArray<Components::TerrainComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity e        = entities[i];
        auto&  terrain  = terrains[i];
        auto*  meshComp = reg.Get<Components::MeshComponent>(e);

        if (meshComp == nullptr) {
            continue;
        }

        // 1. Assign deterministic asset IDs if unassigned
        if (meshComp->meshAsset == InvalidAssetID) {
            meshComp->meshAsset = HashAssetID("terrain_mesh_" + std::to_string(e.index));
        }
        if (meshComp->materialAsset == InvalidMaterialID) {
            meshComp->materialAsset = HashAssetID("terrain_mat_" + std::to_string(e.index));
        }

        // 2. Lazy bake or re-bake GPU mesh if invalidated
        if (!rc.GetGPUMesh(meshComp->meshAsset).has_value()) {
            if (!terrain.heights.empty()) {
                Mesh tMesh = CreativeWorksFactory::CreateTerrainFromData(
                    rc, terrain.sampleCount, terrain.worldSize, terrain.heights.data(), terrain.colors.empty() ? nullptr : terrain.colors.data()
                );
                rc.RegisterGPUMesh(meshComp->meshAsset, tMesh);
            }
        }

        // 3. Lazy bake or re-bake GPU material if invalidated
        if (!rc.GetGPUMaterial(meshComp->materialAsset).has_value()) {
            auto mat            = CreativeWorksFactory::CreateBasicMaterial(rc).value_or(Material {});
            mat.roughnessFactor = terrain.roughness;
            mat.metallicFactor  = terrain.metallic;
            rc.RegisterGPUMaterial(meshComp->materialAsset, mat);
        }
    }
}

float TerrainSystem::SampleHeightAt(const Engine& engine, float worldX, float worldZ) noexcept {
    const auto& reg      = engine.GetRegistry();
    const auto  entities = reg.GetEntitiesWith<Components::TerrainComponent>();
    const auto  terrains = reg.GetRawArray<Components::TerrainComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity      e       = entities[i];
        const auto& terrain = terrains[i];
        const auto* trans   = reg.Get<Components::TransformComponent>(e);

        JPH::Vec3 pos      = (trans != nullptr) ? trans->position : JPH::Vec3::sZero();
        float     halfSize = terrain.worldSize * 0.5f;

        float localX = worldX - pos.GetX();
        float localZ = worldZ - pos.GetZ();

        if (localX >= -halfSize && localX <= halfSize && localZ >= -halfSize && localZ <= halfSize) {
            if (terrain.heights.empty() || terrain.sampleCount < 2) {
                return pos.GetY();
            }

            // Map [-halfSize, halfSize] -> [0, sampleCount - 1]
            float normX = (localX + halfSize) / terrain.worldSize * static_cast<float>(terrain.sampleCount - 1);
            float normZ = (localZ + halfSize) / terrain.worldSize * static_cast<float>(terrain.sampleCount - 1);

            int x0 = std::clamp(static_cast<int>(std::floor(normX)), 0, static_cast<int>(terrain.sampleCount - 1));
            int z0 = std::clamp(static_cast<int>(std::floor(normZ)), 0, static_cast<int>(terrain.sampleCount - 1));
            int x1 = std::min(x0 + 1, static_cast<int>(terrain.sampleCount - 1));
            int z1 = std::min(z0 + 1, static_cast<int>(terrain.sampleCount - 1));

            float tx = normX - static_cast<float>(x0);
            float tz = normZ - static_cast<float>(z0);

            float h00 = terrain.heights[x0 + z0 * terrain.sampleCount];
            float h10 = terrain.heights[x1 + z0 * terrain.sampleCount];
            float h01 = terrain.heights[x0 + z1 * terrain.sampleCount];
            float h11 = terrain.heights[x1 + z1 * terrain.sampleCount];

            float h0 = h00 + tx * (h10 - h00);
            float h1 = h01 + tx * (h11 - h01);

            return pos.GetY() + (h0 + tz * (h1 - h0));
        }
    }

    return 0.0f;
}

} // namespace ZHLN
