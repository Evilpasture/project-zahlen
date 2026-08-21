// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerrainSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string_view>
#include <vector>

namespace ZHLN {

namespace {
constexpr size_t MAX_TERRAIN_SLOTS = 1024;

struct TerrainSlot {
    TerrainData            data;
    ZHLN::Atomic<uint32_t> generation {1};
    ZHLN::Atomic<bool>     occupied {false};
};

std::array<TerrainSlot, MAX_TERRAIN_SLOTS> s_TerrainSlots;
ZHLN::Mutex                                s_LifecycleMutex {};
std::vector<TerrainData>                   s_DeferredCleanup;
} // namespace

TerrainHandle TerrainSystem::RegisterTerrainData(TerrainData data) noexcept {
    return Lock(s_LifecycleMutex, [&]() -> TerrainHandle {
        for (size_t i = 0; i < MAX_TERRAIN_SLOTS; ++i) {
            auto& slot = s_TerrainSlots[i];
            if (!slot.occupied.load(std::memory_order::relaxed)) {
                slot.data    = std::move(data);
                uint32_t gen = slot.generation.load(std::memory_order::relaxed);
                slot.occupied.store(true, std::memory_order::release);

                uint64_t handleRaw = (static_cast<uint64_t>(gen) << 32) | static_cast<uint64_t>(i + 1);
                return static_cast<TerrainHandle>(handleRaw);
            }
        }
        ZHLN::Log("[TerrainSystem] ERROR: Exceeded maximum terrain slot capacity ({})!", MAX_TERRAIN_SLOTS);
        return TerrainHandle::Invalid;
    });
}

const TerrainData* TerrainSystem::GetTerrainData(TerrainHandle handle) noexcept {
    if (handle == TerrainHandle::Invalid) {
        return nullptr;
    }
    auto     raw     = static_cast<uint64_t>(handle);
    uint32_t slotIdx = static_cast<uint32_t>(raw & 0xFFFFFFFF) - 1;
    auto     gen     = static_cast<uint32_t>(raw >> 32);

    if (slotIdx >= MAX_TERRAIN_SLOTS) {
        return nullptr;
    }

    const auto& slot = s_TerrainSlots[slotIdx];
    if (!slot.occupied.load(std::memory_order::acquire)) {
        return nullptr;
    }
    if (slot.generation.load(std::memory_order::relaxed) != gen) {
        return nullptr;
    }
    return &slot.data;
}

void TerrainSystem::UnregisterTerrainData(TerrainHandle handle) noexcept {
    if (handle == TerrainHandle::Invalid) {
        return;
    }
    auto     raw     = static_cast<uint64_t>(handle);
    uint32_t slotIdx = static_cast<uint32_t>(raw & 0xFFFFFFFF) - 1;
    auto     gen     = static_cast<uint32_t>(raw >> 32);

    if (slotIdx >= MAX_TERRAIN_SLOTS) {
        return;
    }

    Lock(s_LifecycleMutex, [&] {
        auto& slot = s_TerrainSlots[slotIdx];
        if (slot.occupied.load(std::memory_order_relaxed) && slot.generation.load(std::memory_order_relaxed) == gen) {
            slot.occupied.store(false, std::memory_order::release);
            slot.generation.fetch_add(1, std::memory_order::relaxed);

            // Defer buffer deallocation to ensure active frame sampling remains 100% safe
            s_DeferredCleanup.push_back(std::move(slot.data));
            slot.data = {};
        }
    });
}

void TerrainSystem::Update(Engine& engine, float /*dt*/) {
    // Reclaim retired terrain buffers from previous frames
    Lock(s_LifecycleMutex, [&] { s_DeferredCleanup.clear(); });

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

        // 1. Zero-allocation asset ID assignment using stack buffer formatting
        if (meshComp->meshAsset == InvalidAssetID) {
            std::array<char, 64> buf {};
            meshComp->meshAsset = HashAssetID(ZHLN::FormatTo(buf, "terrain_mesh_{}", e.index));
        }
        if (meshComp->materialAsset == InvalidMaterialID) {
            std::array<char, 64> buf {};
            meshComp->materialAsset = HashAssetID(ZHLN::FormatTo(buf, "terrain_mat_{}", e.index));
        }

        const TerrainData* tData = GetTerrainData(terrain.terrainHandle);

        // 2. Lazy bake or re-bake GPU mesh if invalidated
        if (!rc.GetGPUMesh(meshComp->meshAsset).has_value()) {
            if (tData != nullptr && !tData->heights.empty()) {
                Mesh tMesh = CreativeWorksFactory::CreateTerrainMeshFromData(
                    rc, tData->sampleCount, tData->worldSize, tData->heights.data(), tData->colors.empty() ? nullptr : tData->colors.data()
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

        // Lock-free O(1) slot resolution
        const TerrainData* tData = GetTerrainData(terrain.terrainHandle);
        if (tData == nullptr) {
            continue;
        }

        const auto* trans    = reg.Get<Components::TransformComponent>(e);
        JPH::Vec3   pos      = (trans != nullptr) ? trans->position : JPH::Vec3::sZero();
        float       halfSize = tData->worldSize * 0.5f;

        float localX = worldX - pos.GetX();
        float localZ = worldZ - pos.GetZ();

        if (localX >= -halfSize && localX <= halfSize && localZ >= -halfSize && localZ <= halfSize) {
            if (tData->heights.empty() || tData->sampleCount < 2) {
                return pos.GetY();
            }

            // Map [-halfSize, halfSize] -> [0, sampleCount - 1]
            float normX = (localX + halfSize) / tData->worldSize * static_cast<float>(tData->sampleCount - 1);
            float normZ = (localZ + halfSize) / tData->worldSize * static_cast<float>(tData->sampleCount - 1);

            int x0 = std::clamp(static_cast<int>(std::floor(normX)), 0, static_cast<int>(tData->sampleCount - 1));
            int z0 = std::clamp(static_cast<int>(std::floor(normZ)), 0, static_cast<int>(tData->sampleCount - 1));
            int x1 = std::min(x0 + 1, static_cast<int>(tData->sampleCount - 1));
            int z1 = std::min(z0 + 1, static_cast<int>(tData->sampleCount - 1));

            float tx = normX - static_cast<float>(x0);
            float tz = normZ - static_cast<float>(z0);

            float h00 = tData->heights[x0 + z0 * tData->sampleCount];
            float h10 = tData->heights[x1 + z0 * tData->sampleCount];
            float h01 = tData->heights[x0 + z1 * tData->sampleCount];
            float h11 = tData->heights[x1 + z1 * tData->sampleCount];

            float h0 = h00 + tx * (h10 - h00);
            float h1 = h01 + tx * (h11 - h01);

            return pos.GetY() + (h0 + tz * (h1 - h0));
        }
    }

    return 0.0f;
}

} // namespace ZHLN
