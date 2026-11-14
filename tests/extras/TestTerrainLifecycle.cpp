// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Terrain/TerrainSystem.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <cmath>
#include <expected>
#include <type_traits>
#include <utility>

enum class TerrainLifecycleTestError : uint8_t {
    SlotExhausted ZHLN_ANNOTATION(ZHLN::Description<"Could not allocate a terrain heightmap slot.">{}) = 1,
};

namespace {

using TerrainRead = ZHLN::Terrain::TerrainHeightQuery;
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<TerrainRead>().Get<ZHLN::Terrain::TerrainComponent>(ZHLN::Entity {}))>>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<TerrainRead>().Get<ZHLN::Components::TransformComponent>(ZHLN::Entity {}))>>);

struct TerrainLifecycleTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> typed_sampling_preserves_interpolation_bounds_and_first_match() {
            using namespace ZHLN;
            using namespace ZHLN::Terrain;
            ECS::Registry registry;
            TerrainData data;
            data.sampleCount = 2;
            data.worldSize = 10.0f;
            data.heights.resize(4);
            data.heights[0] = 1.0f;
            data.heights[1] = 3.0f;
            data.heights[2] = 5.0f;
            data.heights[3] = 7.0f;
            const auto first = TerrainSystem::RegisterTerrainData(std::move(data));
            TerrainData fallback;
            fallback.sampleCount = 1;
            fallback.worldSize = 4.0f;
            const auto second = TerrainSystem::RegisterTerrainData(std::move(fallback));
            if (first == TerrainID::Invalid || second == TerrainID::Invalid) {
                TerrainSystem::UnregisterTerrainData(first);
                TerrainSystem::UnregisterTerrainData(second);
                return std::unexpected(TerrainLifecycleTestError::SlotExhausted);
            }
            registry.Create(TerrainComponent {.terrainID = TerrainID::Invalid}); // skipped before first match
            registry.Create(TerrainComponent {.terrainID = first}, Components::TransformComponent {.position = {10.0f, 4.0f, 20.0f}});
            registry.Create(TerrainComponent {.terrainID = second}, Components::TransformComponent {.position = {10.0f, 50.0f, 20.0f}});
            TerrainData originData;
            originData.sampleCount = 2;
            originData.worldSize = 2.0f;
            originData.heights.resize(4);
            for (auto& height: originData.heights) height = 9.0f;
            const auto origin = TerrainSystem::RegisterTerrainData(std::move(originData));
            if (origin == TerrainID::Invalid) {
                TerrainSystem::ReleaseTerrainDataWithQuery(TerrainReleaseQuery {registry});
                return std::unexpected(TerrainLifecycleTestError::SlotExhausted);
            }
            registry.Create(TerrainComponent {.terrainID = origin}); // missing transform uses origin
            TerrainData flatData;
            flatData.sampleCount = 1;
            flatData.worldSize = 2.0f;
            const auto flat = TerrainSystem::RegisterTerrainData(std::move(flatData));
            if (flat == TerrainID::Invalid) {
                TerrainSystem::ReleaseTerrainDataWithQuery(TerrainReleaseQuery {registry});
                return std::unexpected(TerrainLifecycleTestError::SlotExhausted);
            }
            registry.Create(TerrainComponent {.terrainID = flat}, Components::TransformComponent {.position = {30.0f, 17.0f, 0.0f}});
            const TerrainHeightQuery query {registry};
            const auto close = [](float a, float b) { return std::abs(a - b) < 0.0001f; };
            Test::ExpectTrue(close(TerrainSystem::SampleHeightAtWithQuery(query, 10.0f, 20.0f), 8.0f));
            Test::ExpectTrue(close(TerrainSystem::SampleHeightAtWithQuery(query, 5.0f, 15.0f), 5.0f));
            Test::ExpectTrue(close(TerrainSystem::SampleHeightAtWithQuery(query, 15.0f, 25.0f), 11.0f));
            Test::ExpectTrue(close(TerrainSystem::SampleHeightAtWithQuery(query, 100.0f, 100.0f), 0.0f));
            Test::ExpectTrue(close(TerrainSystem::SampleHeightAtWithQuery(query, 0.0f, 0.0f), 9.0f));
            Test::ExpectTrue(close(TerrainSystem::SampleHeightAtWithQuery(query, 30.0f, 0.0f), 17.0f));
            TerrainSystem::ReleaseTerrainDataWithQuery(TerrainReleaseQuery {registry});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> typed_cleanup_retires_only_pending_then_all() {
            using namespace ZHLN;
            using namespace ZHLN::Terrain;
            ECS::Registry registry;
            const auto liveID = TerrainSystem::RegisterTerrainData({});
            const auto pendingID = TerrainSystem::RegisterTerrainData({});
            if (liveID == TerrainID::Invalid || pendingID == TerrainID::Invalid) {
                TerrainSystem::UnregisterTerrainData(liveID);
                TerrainSystem::UnregisterTerrainData(pendingID);
                return std::unexpected(TerrainLifecycleTestError::SlotExhausted);
            }
            const Entity live = registry.Create(TerrainComponent {.terrainID = liveID});
            const Entity pending = registry.Create(TerrainComponent {.terrainID = pendingID}, Components::PendingDestroy {});
            TerrainSystem::CleanupWithQuery(TerrainCleanupQuery {registry}, false);
            Test::ExpectTrue(TerrainSystem::GetTerrainData(liveID) != nullptr);
            Test::ExpectTrue(TerrainSystem::GetTerrainData(pendingID) == nullptr);
            Test::ExpectEq(registry.Get<TerrainComponent>(live)->terrainID, liveID);
            Test::ExpectEq(registry.Get<TerrainComponent>(pending)->terrainID, TerrainID::Invalid);
            Test::ExpectTrue(registry.IsAlive(pending)); // cleanup does not destroy the component or entity
            TerrainSystem::CleanupWithQuery(TerrainCleanupQuery {registry}, false);
            TerrainSystem::CleanupWithQuery(TerrainCleanupQuery {registry}, true);
            Test::ExpectTrue(TerrainSystem::GetTerrainData(liveID) == nullptr);
            Test::ExpectEq(registry.Get<TerrainComponent>(live)->terrainID, TerrainID::Invalid);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> standalone_scene_releases_heightmap_ids_before_clear() {
            ZHLN::ECS::Registry registry;
            ZHLN::Terrain::TerrainData data;
            data.sampleCount = 2;
            data.heights.resize(4);
            data.heights[0] = 2.5f;

            const auto handle = ZHLN::Terrain::TerrainSystem::RegisterTerrainData(std::move(data));
            if (!ZHLN::Test::ExpectNe(handle, ZHLN::Terrain::TerrainID::Invalid)) {
                return std::unexpected(TerrainLifecycleTestError::SlotExhausted);
            }
            const auto entity = registry.Create(ZHLN::Terrain::TerrainComponent {.terrainID = handle});
            ZHLN::Test::ExpectEq(ZHLN::Terrain::TerrainSystem::GetTerrainData(handle)->heights[0], 2.5f);

            ZHLN::Terrain::TerrainSystem::ReleaseTerrainDataWithQuery(ZHLN::Terrain::TerrainReleaseQuery {registry});
            ZHLN::Test::ExpectTrue(ZHLN::Terrain::TerrainSystem::GetTerrainData(handle) == nullptr);
            ZHLN::Test::ExpectEq(registry.Get<ZHLN::Terrain::TerrainComponent>(entity)->terrainID, ZHLN::Terrain::TerrainID::Invalid);
            ZHLN::Terrain::TerrainSystem::ReleaseTerrainDataWithQuery(ZHLN::Terrain::TerrainReleaseQuery {registry}); // repeated release is harmless
            registry.Clear();
            ZHLN::Test::ExpectFalse(registry.IsAlive(entity));

            const auto reused = ZHLN::Terrain::TerrainSystem::RegisterTerrainData({});
            if (ZHLN::Test::ExpectNe(reused, ZHLN::Terrain::TerrainID::Invalid)) {
                ZHLN::Test::ExpectNe(reused, handle); // the slot's generation advanced
                ZHLN::Terrain::TerrainSystem::UnregisterTerrainData(reused);
            }
            return {};
        }
    };
};

} // namespace
