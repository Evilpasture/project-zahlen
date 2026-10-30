// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/CommonFixtures.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Physics/Physics.hpp>

// A separate source-file shard deliberately omits TaskSystemScope. Jolt still
// needs at least one reported executor when TaskSystem::Dispatch runs inline.
namespace ZHLN::Test {

void test_physics_step_without_task_scheduler_uses_serial_fallback() {
    Fixture::JoltScope jolt;
    if (!ExpectTrue(TaskSystem::GetWorkerCount() == 0u)) {
        return; // This test only exercises the schedulerless path.
    }

    PhysicsConfig  cfg {.maxBodies = 16, .maxBodyPairs = 32, .maxContactConstraints = 32, .tempAllocatorSize = 2 * 1024 * 1024};
    PhysicsContext pc(cfg);
    const auto     shape = pc.GetOrCreateShape(Physics::ShapeType::Sphere, 0.5f);
    if (!ExpectTrue(shape != nullptr)) {
        return;
    }
    const auto body = pc.CreateRigidBody(shape, JPH::RVec3(0.0, 10.0, 0.0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::ID::MOVING);
    if (!ExpectTrue(body != Physics::BodyHandle::Null())) {
        return;
    }
    pc.OptimizeBroadphase();
    for (int i = 0; i < 3; ++i) {
        pc.Step(1.0f / 60.0f);
    }
    const auto position = pc.BodyPosition(body);
    if (ExpectTrue(position.has_value())) {
        ExpectLt(position->GetY(), 10.0);
    }
}

} // namespace ZHLN::Test
