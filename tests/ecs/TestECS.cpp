// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/TrackingMemoryResource.hpp"
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// --- Mock Components for Testing ---
struct PositionComponent {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool  operator==(const PositionComponent&) const = default;
};

struct VelocityComponent {
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    bool  operator==(const VelocityComponent&) const = default;
};

struct TagComponent {
    std::string tag = "Default";
};

struct FlagComponent {
    bool active = true;
};

struct PayloadComponent {
    uint32_t resource = 0;
};

struct CountedComponent {
    uint32_t* destructions = nullptr;

    ~CountedComponent() {
        if (destructions != nullptr) {
            ++*destructions;
        }
    }
};

enum class ECSTestError : uint8_t {
    EntityGenerationMismatch ZHLN_ANNOTATION(ZHLN::Description<"Recycled entity handle failed generation check.">{}) = 1,
    ComponentAccessFailed ZHLN_ANNOTATION(ZHLN::Description<"Component addition, retrieval, or removal failed.">{}),
    CommandBufferFailed ZHLN_ANNOTATION(ZHLN::Description<"Deferred EntityCommandBuffer operations failed.">{}),
};

namespace ZHLN::Test {
namespace {

struct ZHLN_ANNOTATION(SuiteFixture {}) EcsComponentRegistration {
    EcsComponentRegistration() {
        ZHLN::ECS::Registry reg;
        reg.RegisterComponent<PositionComponent>("PositionComponent");
        reg.RegisterComponent<VelocityComponent>("VelocityComponent");
        reg.RegisterComponent<TagComponent>("TagComponent");
        reg.RegisterComponent<FlagComponent>("FlagComponent");
    }
};

} // namespace

namespace ZHLN_ANNOTATION(UseFixtures<EcsComponentRegistration> {}) {

        // --- 1. Entity Lifecycle & Generation Recycling ---
        std::expected<void, ZHLN::ErrorCode> test_entity_creation_destruction_recycling(ZHLN::ECS::Registry& reg) {
            ZHLN::Entity e1 = reg.Create();
            ZHLN::Test::Expect(reg.IsAlive(e1));
            ZHLN::Test::Expect(e1.generation) == 1;

            uint32_t originalIndex = e1.index;
            reg.Destroy(e1);
            ZHLN::Test::Expect(!reg.IsAlive(e1));

            // Create new entity; should recycle e1's index with an incremented generation
            ZHLN::Entity e2 = reg.Create();
            ZHLN::Test::Expect(reg.IsAlive(e2));
            ZHLN::Test::Expect(e2.index) == originalIndex;
            ZHLN::Test::Expect(e2.generation) == 2;

            // The old handle e1 must remain dead!
            ZHLN::Test::Expect(!reg.IsAlive(e1));

            return {};
        }

        // --- 2. Component Add, Get, Patch, and Remove ---
        std::expected<void, ZHLN::ErrorCode> test_component_crud_and_patching(ZHLN::ECS::Registry& reg) {
            ZHLN::Entity e = reg.Create();

            constexpr float StartX = 10.0f;
            constexpr float StartY = 20.0f;
            constexpr float StartZ = 30.0f;
            constexpr float VelX   = 1.0f;
            constexpr float VelY   = 0.0f;
            constexpr float VelZ   = -1.0f;
            constexpr float EndX   = 11.0f;
            constexpr float EndZ   = 29.0f;

            reg.Add<PositionComponent>(e, PositionComponent {.x = StartX, .y = StartY, .z = StartZ});
            reg.Add<VelocityComponent>(e, VelocityComponent {.vx = VelX, .vy = VelY, .vz = VelZ});

            // Retrieve and verify
            auto pos = reg.Get<PositionComponent>(e);
            if (!(ZHLN::Test::Expect(pos.has_value()) == true)) {
                return std::unexpected(ECSTestError::ComponentAccessFailed);
            }

            ZHLN::Test::Expect(pos->x) == StartX;

            // Test Patch combinator
            bool patched = reg.Patch<PositionComponent, VelocityComponent>(e, [](auto& p, auto& v) {
                p.x += v.vx;
                p.y += v.vy;
                p.z += v.vz;
            });
            ZHLN::Test::Expect(patched);
            ZHLN::Test::Expect(pos->x) == EndX;
            ZHLN::Test::Expect(pos->z) == EndZ;

            // Remove component
            reg.Remove<VelocityComponent>(e);
            ZHLN::Test::Expect(!reg.Get<VelocityComponent>(e).has_value());
            ZHLN::Test::Expect(reg.Get<PositionComponent>(e).has_value());

            return {};
        }

        // --- 3. Registry Bulk Operations (Fold Expressions) ---
        std::expected<void, ZHLN::ErrorCode> test_registry_bulk_operations(ZHLN::ECS::Registry& reg) {
            constexpr float BulkPosX = 5.0f;
            constexpr float BulkPosY = 5.0f;
            constexpr float BulkVelY = 10.0f;

            // Bulk Create with instances
            ZHLN::Entity e1 = reg.Create(PositionComponent {.x = BulkPosX, .y = BulkPosY}, VelocityComponent {.vy = BulkVelY});

            ZHLN::Test::Expect(reg.Get<PositionComponent>(e1).has_value());
            ZHLN::Test::Expect(reg.Get<PositionComponent>(e1)->x) == BulkPosX;
            ZHLN::Test::Expect(reg.Get<VelocityComponent>(e1).has_value());

            // Bulk Create with Type parameters (default constructed)
            ZHLN::Entity e2 = reg.Create<PositionComponent, TagComponent, FlagComponent>();

            ZHLN::Test::Expect(reg.Get<PositionComponent>(e2).has_value());
            ZHLN::Test::Expect(reg.Get<PositionComponent>(e2)->x) == 0.0f; // Default

            ZHLN::Test::Expect(reg.Get<TagComponent>(e2).has_value());
            ZHLN::Test::Expect(reg.Get<TagComponent>(e2)->tag) == "Default";

            ZHLN::Test::Expect(reg.Get<FlagComponent>(e2).has_value());

            return {};
        }

        // Plain ECS stays immediate: it owns only component storage, not the
        // resources a scene's component handles may refer to.
        std::expected<void, ZHLN::ErrorCode> test_data_only_registry_lifecycle_is_immediate(ZHLN::ECS::Registry& reg) {
            const auto a = reg.Create(PayloadComponent {.resource = 1});
            const auto b = reg.Create(PayloadComponent {.resource = 2});
            reg.Add(a, PayloadComponent {.resource = 3});
            ZHLN::Test::Expect(reg.Get<PayloadComponent>(a)->resource) == 3;
            reg.Remove<PayloadComponent>(a);
            ZHLN::Test::Expect(!reg.Get<PayloadComponent>(a).has_value());
            ZHLN::Test::Expect(reg.Get<PayloadComponent>(b)->resource) == 2;
            reg.Destroy(b);
            ZHLN::Test::Expect(!reg.IsAlive(b));

            const auto recycled = reg.Create(PayloadComponent {.resource = 4});
            reg.Remove<PayloadComponent>(b); // stale generation cannot affect the recycled entity
            ZHLN::Test::Expect(reg.Get<PayloadComponent>(recycled)->resource) == 4;
            reg.Clear();
            ZHLN::Test::Expect(!reg.IsAlive(recycled));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> test_dynamic_add_cannot_overwrite_typed_owners(ZHLN::ECS::Registry& reg) {
            reg.RegisterComponent<PositionComponent>("PositionComponent");
            const auto entity = reg.Create(PositionComponent {.x = 9.0f});
            const auto typedFamily = ZHLN::ECS::ComponentFamily::GetTypeID<PositionComponent>();
            ZHLN::Test::Expect(reg.AddDynamic(entity, typedFamily)) == nullptr;
            ZHLN::Test::Expect(reg.AddDynamic(entity, 0xFFFFFFFFu)) == nullptr;
            ZHLN::Test::Expect(reg.Get<PositionComponent>(entity)->x) == 9.0f;
            ZHLN::Test::Expect(reg.RegisterComponentDynamic("PositionComponent", sizeof(PositionComponent), alignof(PositionComponent))) == 0xFFFFFFFFu;

            const auto dynamicFamily = reg.RegisterComponentDynamic("ECSDynamicDataOnlyTest", sizeof(uint32_t), alignof(uint32_t));
            void* data = reg.AddDynamic(entity, dynamicFamily);
            ZHLN::Test::Expect(data) != nullptr;
            ZHLN::Test::Expect(reg.GetRawByFamily(entity, dynamicFamily)) == data;
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> test_command_buffer_destroy_policy_keeps_components_for_cleanup(ZHLN::ECS::Registry& reg) {
            struct Marked {};
            ZHLN::ECS::EntityCommandBuffer ecb(reg, [](ZHLN::ECS::Registry& registry, ZHLN::Entity entity) {
                if (registry.IsAlive(entity) && !registry.Get<Marked>(entity)) {
                    registry.Add(entity, Marked {});
                }
            });
            const auto owner = reg.Create(PayloadComponent {.resource = 42});
            ecb.DestroyEntity(owner);
            ecb.DestroyEntity(owner);
            ecb.Playback();
            ZHLN::Test::Expect(reg.IsAlive(owner));
            ZHLN::Test::Expect(reg.Get<Marked>(owner).has_value());
            ZHLN::Test::Expect(reg.Get<PayloadComponent>(owner)->resource) == 42;

            // A batch system can inspect every marked component before it
            // reclaims the entities. Snapshot because Destroy compacts sets.
            const auto marked = reg.GetEntitiesWith<Marked>();
            std::vector<ZHLN::Entity> pending(marked.begin(), marked.end());
            for (auto entity: pending) {
                reg.Destroy(entity);
            }
            ZHLN::Test::Expect(!reg.IsAlive(owner));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> test_registry_teardown_runs_component_destructors() {
            ZHLN::Test::TrackingMemoryResource storage;
            uint32_t                           destructions   = 0;
            uint32_t                           beforeTeardown = 0;
            {
                ZHLN::ECS::Registry reg {&storage};
                reg.RegisterComponent<PositionComponent>("PositionComponent");
                // Grow both the entity tables and component storage; register
                // metadata and a second set through the public component API.
                for (uint32_t i = 0; i < 1100; ++i) {
                    reg.Create(PayloadComponent {.resource = i});
                }
                reg.Create(CountedComponent {.destructions = &destructions}, PositionComponent {});
                ZHLN::Test::ExpectGt(storage.Allocations(), 0u);
                ZHLN::Test::ExpectGt(storage.LiveBlocks(), 0u);

                const size_t warmedAllocations = storage.Allocations();
                reg.Clear();
                reg.Create(CountedComponent {.destructions = &destructions});
                reg.Create(PayloadComponent {.resource = 7});
                // Reusing the warmed registry must not grow its storage.
                ZHLN::Test::Expect(storage.Allocations()) == warmedAllocations;
                beforeTeardown = destructions; // Account for the Add temporary.
            }
            ZHLN::Test::Expect(destructions) == beforeTeardown + 1;
            ZHLN::Test::ExpectGt(storage.Allocations(), 0u);
            ZHLN::Test::Expect(storage.LiveBlocks()) == 0u;
            ZHLN::Test::Expect(storage.LiveBytes()) == 0u;
            ZHLN::Test::Expect(storage.Deallocations()) == storage.Allocations();
            return {};
        }

        // --- 4. Deferred EntityCommandBuffer Playback (Fold Expressions) ---
        std::expected<void, ZHLN::ErrorCode> test_entity_command_buffer_playback(ZHLN::ECS::Registry& reg) {
            ZHLN::ECS::EntityCommandBuffer ecb(reg);

            constexpr float DefPosX = 100.0f;
            constexpr float DefPosY = 200.0f;
            constexpr float DefPosZ = 300.0f;

            // Pre-existing entity to test deferred deletion
            ZHLN::Entity targetToDestroy = reg.Create<FlagComponent>();

            // Record deferred operations using fold expressions!
            ZHLN::Entity tempEntity1 = ecb.CreateEntity(PositionComponent {.x = DefPosX, .y = DefPosY, .z = DefPosZ}, TagComponent {.tag = "DeferredTag"});

            ZHLN::Entity tempEntity2 = ecb.CreateEntity<VelocityComponent>();
            ecb.AddComponent<FlagComponent, PositionComponent>(tempEntity2); // Bulk default add

            ecb.DestroyEntity(targetToDestroy);

            // Before playback: registry has no new entities, and the target is still alive
            ZHLN::Test::ExpectFalse(reg.IsAlive(tempEntity1));
            ZHLN::Test::ExpectTrue(reg.IsAlive(targetToDestroy));

            // Execute playback
            ecb.Playback();

            // After playback: Target should be destroyed
            ZHLN::Test::Expect(!reg.IsAlive(targetToDestroy));

            // Verify deferred creations by querying the registry
            // (ECB temporary entity IDs map to actual IDs internally during Playback)
            auto taggedEntities = reg.GetEntitiesWith<TagComponent>();
            if (!(ZHLN::Test::Expect(taggedEntities.size()) == 1)) {
                return std::unexpected(ECSTestError::ComponentAccessFailed);
            }

            ZHLN::Entity realEntity1 = taggedEntities[0];
            ZHLN::Test::Expect(reg.IsAlive(realEntity1));

            auto pos = reg.Get<PositionComponent>(realEntity1);
            ZHLN::Test::Expect(pos.has_value());
            ZHLN::Test::Expect(pos->x) == DefPosX;

            // Verify the second entity (Velocity + Flag + Position)
            auto flaggedEntities = reg.GetEntitiesWith<FlagComponent>();
            if (!(ZHLN::Test::Expect(flaggedEntities.size()) == 1)) {
                return std::unexpected(ECSTestError::ComponentAccessFailed);
            }

            ZHLN::Entity realEntity2 = flaggedEntities[0];
            ZHLN::Test::Expect(reg.Get<VelocityComponent>(realEntity2).has_value());
            ZHLN::Test::Expect(reg.Get<PositionComponent>(realEntity2).has_value());

            return {};
        }

} // namespace
} // namespace ZHLN::Test
