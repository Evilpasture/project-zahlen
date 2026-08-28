// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <cmath>
#include <expected>

enum class PhysicsTestError : uint32_t {
    RaycastMissedExpectedBody[[= ZHLN::Reflect::Description<"Raycast did not hit expected collider.">{}]] = 1,
    OverlapQueryFailed[[= ZHLN::Reflect::Description<"Broadphase overlap failed to detect sphere/AABB collision.">{}]],
};

struct PhysicsTestSuite {
    PhysicsTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);

        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance == nullptr) {
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }

    ~PhysicsTestSuite() {
        ZHLN::TaskSystem::Shutdown();
        JPH::UnregisterTypes();
        if (JPH::Factory::sInstance != nullptr) {
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

    struct Tests {
        std::expected<void, ZHLN::Error> rigid_body_raycast_and_overlap() {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 128, .maxBodyPairs = 256, .maxContactConstraints = 256, .tempAllocatorSize = 4 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // Create static ground box at (0, 0, 0) with half-extents (10, 1, 10)
            auto         boxShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 10.0f, 1.0f, 10.0f);
            ZHLN::Entity ground = pc.CreateRigidBody(boxShape, JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);
            ZHLN::Test::ExpectTrue(ground != ZHLN::Entity::Null());

            pc.OptimizeBroadphase();

            // 1. Raycast downwards from (0, 10, 0) -> (0, -1, 0)
            auto hit = pc.Raycast(JPH::RVec3(0, 10, 0), JPH::Vec3(0, -1, 0), 20.0f);
            ZHLN::Test::ExpectTrue(hit.hasHit);
            ZHLN::Test::ExpectEq(hit.handle, ground);

            // Ground box top surface is at Y = 1.0
            ZHLN::Test::ExpectTrue(std::abs(hit.position.GetY() - 1.0f) < 0.05f);
            ZHLN::Test::ExpectTrue(hit.normal.GetY() > 0.9f);

            // 2. Sphere overlap at (0, 0, 0)
            JPH::Array<ZHLN::Entity> overlapResults;
            pc.OverlapSphere(JPH::RVec3(0, 0, 0), 5.0f, overlapResults);
            ZHLN::Test::ExpectTrue(overlapResults.size() >= 1);

            return {};
        }

        std::expected<void, ZHLN::Error> dynamic_body_simulation_step() {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 128, .maxBodyPairs = 256, .maxContactConstraints = 256, .tempAllocatorSize = 4 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // Create a dynamic falling sphere (radius 0.5) centered at (0, 10, 0)
            // Top surface initially at Y = 10.5
            auto         sphereShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, 0.5f);
            ZHLN::Entity sphere =
                pc.CreateRigidBody(sphereShape, JPH::RVec3(0, 10, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::MOVING);
            ZHLN::Test::ExpectTrue(sphere != ZHLN::Entity::Null());

            pc.OptimizeBroadphase();

            // Step 30 physics frames (0.5s total): falls d = 0.5 * 9.81 * 0.25 = ~1.23m
            // New center Y ~= 8.77m, new top surface Y ~= 9.27m
            for (int i = 0; i < 30; ++i) {
                pc.Step(1.0f / 60.0f);
            }

            // Raycast down from (0, 15, 0) to find new fallen position
            auto hit = pc.Raycast(JPH::RVec3(0, 15, 0), JPH::Vec3(0, -1, 0), 30.0f);
            ZHLN::Test::ExpectTrue(hit.hasHit);
            ZHLN::Test::ExpectTrue(hit.position.GetY() < 10.0f); // Top surface fell below 10.0m

            // Verify position buffer holds fallen center of mass (< 9.0m)
            auto posView = pc.GetPositionBuffer();
            ZHLN::Test::ExpectTrue(posView.buf != nullptr);
            auto* posData = static_cast<const JPH::Real*>(posView.buf);
            ZHLN::Test::ExpectTrue(posData[1] < 9.0);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<PhysicsTestSuite>();
}
