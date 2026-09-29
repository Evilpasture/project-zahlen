// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Terrain/TerrainSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <expected>
#include <utility>

enum class TerrainLifecycleTestError : uint8_t {
    SlotExhausted ZHLN_ANNOTATION(ZHLN::Description<"Could not allocate a terrain heightmap slot.">{}) = 1,
};

struct TerrainLifecycleTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> standalone_scene_releases_heightmap_handles_before_clear() {
            ZHLN::ECS::Registry registry;
            ZHLN::Terrain::TerrainData data;
            data.sampleCount = 2;
            data.heights.resize(4);
            data.heights[0] = 2.5f;

            const auto handle = ZHLN::Terrain::TerrainSystem::RegisterTerrainData(std::move(data));
            if (!ZHLN::Test::ExpectNe(handle, ZHLN::Terrain::TerrainHandle::Invalid)) {
                return std::unexpected(TerrainLifecycleTestError::SlotExhausted);
            }
            const auto entity = registry.Create(ZHLN::Terrain::TerrainComponent {.terrainHandle = handle});
            ZHLN::Test::ExpectEq(ZHLN::Terrain::TerrainSystem::GetTerrainData(handle)->heights[0], 2.5f);

            ZHLN::Terrain::TerrainSystem::ReleaseTerrainData(registry);
            ZHLN::Test::ExpectTrue(ZHLN::Terrain::TerrainSystem::GetTerrainData(handle) == nullptr);
            ZHLN::Test::ExpectEq(registry.Get<ZHLN::Terrain::TerrainComponent>(entity)->terrainHandle, ZHLN::Terrain::TerrainHandle::Invalid);
            ZHLN::Terrain::TerrainSystem::ReleaseTerrainData(registry); // repeated release is harmless
            registry.Clear();
            ZHLN::Test::ExpectFalse(registry.IsAlive(entity));

            const auto reused = ZHLN::Terrain::TerrainSystem::RegisterTerrainData({});
            if (ZHLN::Test::ExpectNe(reused, ZHLN::Terrain::TerrainHandle::Invalid)) {
                ZHLN::Test::ExpectNe(reused, handle); // the slot's generation advanced
                ZHLN::Terrain::TerrainSystem::UnregisterTerrainData(reused);
            }
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<TerrainLifecycleTestSuite>();
}
