// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <expected>
#include <string>

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

enum class ECSTestError : uint8_t {
    EntityGenerationMismatch[[= ZHLN::Reflect::Description<"Recycled entity handle failed generation check.">{}]] = 1,
    ComponentAccessFailed[[= ZHLN::Reflect::Description<"Component addition, retrieval, or removal failed.">{}]],
    CommandBufferFailed[[= ZHLN::Reflect::Description<"Deferred EntityCommandBuffer operations failed.">{}]],
};

struct ECSTestSuite {
    ECSTestSuite() {
        // Register test component types with the family dispatcher
        ZHLN::ECS::Registry reg;
        reg.RegisterComponent<PositionComponent>("PositionComponent");
        reg.RegisterComponent<VelocityComponent>("VelocityComponent");
        reg.RegisterComponent<TagComponent>("TagComponent");
        reg.RegisterComponent<FlagComponent>("FlagComponent");
    }

    struct Tests {
        // --- 1. Entity Lifecycle & Generation Recycling ---
        std::expected<void, ZHLN::Error> entity_creation_destruction_recycling() {
            ZHLN::ECS::Registry reg;

            ZHLN::Entity e1 = reg.Create();
            ZHLN::Test::ExpectTrue(reg.IsAlive(e1));
            ZHLN::Test::ExpectEq(e1.generation, 1u);

            uint32_t originalIndex = e1.index;
            reg.Destroy(e1);
            ZHLN::Test::ExpectFalse(reg.IsAlive(e1));

            // Create new entity; should recycle e1's index with an incremented generation
            ZHLN::Entity e2 = reg.Create();
            ZHLN::Test::ExpectTrue(reg.IsAlive(e2));
            ZHLN::Test::ExpectEq(e2.index, originalIndex);
            ZHLN::Test::ExpectEq(e2.generation, 2u);

            // The old handle e1 must remain dead!
            ZHLN::Test::ExpectFalse(reg.IsAlive(e1));

            return {};
        }

        // --- 2. Component Add, Get, Patch, and Remove ---
        std::expected<void, ZHLN::Error> component_crud_and_patching() {
            ZHLN::ECS::Registry reg;
            ZHLN::Entity        e = reg.Create();

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
            auto* pos      = reg.Get<PositionComponent>(e);
            auto  checkPos = ZHLN::Test::AssertTrue(pos != nullptr);
            if (!checkPos) { // FIXED: Added Braces
                return checkPos;
            }

            ZHLN::Test::ExpectEq(pos->x, StartX);

            // Test Patch combinator
            bool patched = reg.Patch<PositionComponent, VelocityComponent>(e, [](auto& p, auto& v) {
                p.x += v.vx;
                p.y += v.vy;
                p.z += v.vz;
            });
            ZHLN::Test::ExpectTrue(patched);
            ZHLN::Test::ExpectEq(pos->x, EndX);
            ZHLN::Test::ExpectEq(pos->z, EndZ);

            // Remove component
            reg.Remove<VelocityComponent>(e);
            ZHLN::Test::ExpectTrue(reg.Get<VelocityComponent>(e) == nullptr);
            ZHLN::Test::ExpectTrue(reg.Get<PositionComponent>(e) != nullptr);

            return {};
        }

        // --- 3. Registry Bulk Operations (Fold Expressions) ---
        std::expected<void, ZHLN::Error> registry_bulk_operations() {
            ZHLN::ECS::Registry reg;

            constexpr float BulkPosX = 5.0f;
            constexpr float BulkPosY = 5.0f;
            constexpr float BulkVelY = 10.0f;

            // Bulk Create with instances
            ZHLN::Entity e1 = reg.Create(PositionComponent {.x = BulkPosX, .y = BulkPosY}, VelocityComponent {.vy = BulkVelY});

            ZHLN::Test::ExpectTrue(reg.Get<PositionComponent>(e1) != nullptr);
            ZHLN::Test::ExpectEq(reg.Get<PositionComponent>(e1)->x, BulkPosX);
            ZHLN::Test::ExpectTrue(reg.Get<VelocityComponent>(e1) != nullptr);

            // Bulk Create with Type parameters (default constructed)
            ZHLN::Entity e2 = reg.Create<PositionComponent, TagComponent, FlagComponent>();

            ZHLN::Test::ExpectTrue(reg.Get<PositionComponent>(e2) != nullptr);
            ZHLN::Test::ExpectEq(reg.Get<PositionComponent>(e2)->x, 0.0f); // Default

            ZHLN::Test::ExpectTrue(reg.Get<TagComponent>(e2) != nullptr);
            ZHLN::Test::ExpectEq(reg.Get<TagComponent>(e2)->tag, std::string("Default"));

            ZHLN::Test::ExpectTrue(reg.Get<FlagComponent>(e2) != nullptr);

            return {};
        }

        // --- 4. Deferred EntityCommandBuffer Playback (Fold Expressions) ---
        std::expected<void, ZHLN::Error> entity_command_buffer_playback() {
            ZHLN::ECS::Registry            reg;
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
            ZHLN::Test::ExpectFalse(reg.IsAlive(targetToDestroy));

            // Verify deferred creations by querying the registry
            // (ECB temporary entity IDs map to actual IDs internally during Playback)
            auto taggedEntities = reg.GetEntitiesWith<TagComponent>();
            auto checkCount     = ZHLN::Test::AssertTrue(taggedEntities.size() == 1);
            if (!checkCount) { // FIXED: Added Braces
                return checkCount;
            }

            ZHLN::Entity realEntity1 = taggedEntities[0];
            ZHLN::Test::ExpectTrue(reg.IsAlive(realEntity1));

            auto* pos = reg.Get<PositionComponent>(realEntity1);
            ZHLN::Test::ExpectTrue(pos != nullptr);
            ZHLN::Test::ExpectEq(pos->x, DefPosX);

            // Verify the second entity (Velocity + Flag + Position)
            auto flaggedEntities = reg.GetEntitiesWith<FlagComponent>();
            auto checkFlagged    = ZHLN::Test::AssertEq(flaggedEntities.size(), 1u); // FIXED: Capture and check nodiscard return
            if (!checkFlagged) {
                return checkFlagged;
            }

            ZHLN::Entity realEntity2 = flaggedEntities[0];
            ZHLN::Test::ExpectTrue(reg.Get<VelocityComponent>(realEntity2) != nullptr);
            ZHLN::Test::ExpectTrue(reg.Get<PositionComponent>(realEntity2) != nullptr);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ECSTestSuite>();
}
