// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <ecs/EntityCommandBuffer.hpp>
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

enum class ECSTestError : uint32_t {
    Success = 0,
    EntityGenerationMismatch[[= ZHLN::Reflect::Description("Recycled entity handle failed generation check.")]],
    ComponentAccessFailed[[= ZHLN::Reflect::Description("Component addition, retrieval, or removal failed.")]],
    CommandBufferPlaybackFailed[[= ZHLN::Reflect::Description("Deferred EntityCommandBuffer playback failed.")]],
};

struct ECSTestSuite {
    ECSTestSuite() {
        // Register test component types with the family dispatcher
        ZHLN::ECS::Registry reg;
        reg.RegisterComponent<PositionComponent>("PositionComponent");
        reg.RegisterComponent<VelocityComponent>("VelocityComponent");
        reg.RegisterComponent<TagComponent>("TagComponent");
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

            reg.Add<PositionComponent>(e, PositionComponent {.x = 10.0f, .y = 20.0f, .z = 30.0f});
            reg.Add<VelocityComponent>(e, VelocityComponent {.vx = 1.0f, .vy = 0.0f, .vz = -1.0f});

            // Retrieve and verify
            auto* pos      = reg.Get<PositionComponent>(e);
            auto  checkPos = ZHLN::Test::AssertTrue(pos != nullptr);
            if (!checkPos) {
                return checkPos;
            }
            ZHLN::Test::ExpectEq(pos->x, 10.0f);

            // Test Patch combinator
            bool patched = ZHLN::ECS::Patch<PositionComponent, VelocityComponent>(reg, e, [](auto& p, auto& v) {
                p.x += v.vx;
                p.y += v.vy;
                p.z += v.vz;
            });
            ZHLN::Test::ExpectTrue(patched);
            ZHLN::Test::ExpectEq(pos->x, 11.0f);
            ZHLN::Test::ExpectEq(pos->z, 29.0f);

            // Remove component
            reg.Remove<VelocityComponent>(e);
            ZHLN::Test::ExpectTrue(reg.Get<VelocityComponent>(e) == nullptr);
            ZHLN::Test::ExpectTrue(reg.Get<PositionComponent>(e) != nullptr);

            return {};
        }

        // --- 3. Deferred EntityCommandBuffer Playback ---
        std::expected<void, ZHLN::Error> entity_command_buffer_playback() {
            ZHLN::ECS::Registry            reg;
            ZHLN::ECS::EntityCommandBuffer ecb(reg);

            // Record deferred operations
            ZHLN::Entity tempEntity = ecb.CreateEntity();
            ecb.AddComponent(tempEntity, PositionComponent {.x = 100.0f, .y = 200.0f, .z = 300.0f});
            ecb.AddComponent(tempEntity, TagComponent {.tag = "DeferredTag"});

            // Before playback, registry has no entities
            ZHLN::Test::ExpectFalse(reg.IsAlive(tempEntity));

            // Execute playback
            ecb.Playback();

            // Find entity by queries
            auto taggedEntities = reg.GetEntitiesWith<TagComponent>();
            auto checkCount     = ZHLN::Test::AssertTrue(taggedEntities.size() == 1);
            if (!checkCount) {
                return checkCount;
            }

            ZHLN::Entity realEntity = taggedEntities[0];
            ZHLN::Test::ExpectTrue(reg.IsAlive(realEntity));

            auto* pos      = reg.Get<PositionComponent>(realEntity);
            auto  checkPos = ZHLN::Test::AssertTrue(pos != nullptr);
            if (!checkPos) {
                return checkPos;
            }
            ZHLN::Test::ExpectEq(pos->x, 100.0f);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ECSTestSuite>();
}
