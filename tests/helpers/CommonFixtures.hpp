// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/CommonFixtures.hpp
//
// Process-wide RAII lifecycle for the two subsystems a test suite has to bring
// up before any engine or ECS work: the fiber/task system, and Jolt's
// allocator + type registry.
//
// CPU and GPU suites share a TaskSystem::Scope with test-specific worker/fiber
// defaults. As a fixture member the pairing cannot be forgotten on an early
// return, and a suite that needs Jolt composes the second scope instead of
// re-spelling the factory dance.
//
// These are process-wide singletons, so the scopes are non-copyable and
// non-movable, and a binary must not nest two of the same kind.

#pragma once

#include "TestsFramework.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Engine/World.hpp>
#include <cstdint>
#include <memory>
#include <utility>

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

namespace ZHLN::Test::Fixture {

// Brings up the main-thread fiber and the worker pool; tears the pool down.
//
// Defaults match every existing call site: 2 workers, 32 fibers, minimum
// stack. Raise them only for a suite that genuinely saturates the pool.
class ZHLN_ANNOTATION(ZHLN::Test::SuiteFixture {}) TaskSystemScope {
  public:
    explicit TaskSystemScope(uint32_t workerThreads = 2, uint32_t maxFibers = 32)
        : _tasks(workerThreads, maxFibers, ZHLN::kMinimumFiberStackSize) {}

    ~TaskSystemScope() = default;

    TaskSystemScope(const TaskSystemScope&)            = delete;
    TaskSystemScope& operator=(const TaskSystemScope&) = delete;
    TaskSystemScope(TaskSystemScope&&)                 = delete;
    TaskSystemScope& operator=(TaskSystemScope&&)      = delete;

  private:
    ZHLN::TaskSystem::Scope _tasks;
};

// Registers Jolt's default allocator, factory and type registry; undoes them.
//
// The factory is created only if one is not already live, matching the guard
// the physics suites used, so a suite can share a process with another that
// has already registered types.
class ZHLN_ANNOTATION(ZHLN::Test::SuiteFixture {}) JoltScope {
  public:
    JoltScope() {
        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance == nullptr) {
            _ownsFactory              = true;
            JPH::Factory::sInstance   = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }

    ~JoltScope() {
        if (_ownsFactory) {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

    JoltScope(const JoltScope&)            = delete;
    JoltScope& operator=(const JoltScope&) = delete;
    JoltScope(JoltScope&&)                 = delete;
    JoltScope& operator=(JoltScope&&)      = delete;

  private:
    bool _ownsFactory = false;
};

// Task system + Jolt, torn down in the order the physics suites used:
// workers first, then the type registry (FixtureScopeStore destroys in
// reverse-insertion LIFO order: JoltScope inserted first, TaskSystemScope
// inserted second -> TaskSystemScope destroyed first, JoltScope second).
class ZHLN_ANNOTATION(ZHLN::Test::SuiteFixture {}) TaskSystemAndJoltScope {
  public:
    TaskSystemAndJoltScope(JoltScope&, TaskSystemScope&) noexcept {}
    ~TaskSystemAndJoltScope() = default;

    TaskSystemAndJoltScope(const TaskSystemAndJoltScope&)            = delete;
    TaskSystemAndJoltScope& operator=(const TaskSystemAndJoltScope&) = delete;
    TaskSystemAndJoltScope(TaskSystemAndJoltScope&&)                 = delete;
    TaskSystemAndJoltScope& operator=(TaskSystemAndJoltScope&&)      = delete;
};

// Fresh headless ECS + Jolt World per test case, backed by the session-wide
// fiber pool and Jolt type registry.
class ZHLN_ANNOTATION(ZHLN::Test::FunctionFixture {}) WorldFixture {
  public:
    explicit WorldFixture(TaskSystemAndJoltScope&) {
        const ZHLN::PhysicsConfig cfg {
            .maxBodies             = 256,
            .maxBodyPairs          = 512,
            .maxContactConstraints = 512,
            .tempAllocatorSize     = 4 * 1024 * 1024,
        };
        auto res = ZHLN::World::Create(cfg);
        if (res.has_value()) {
            _world = std::move(res.value());
        }
    }

    ~WorldFixture() = default;

    WorldFixture(const WorldFixture&)            = delete;
    WorldFixture& operator=(const WorldFixture&) = delete;
    WorldFixture(WorldFixture&&)                 = delete;
    WorldFixture& operator=(WorldFixture&&)      = delete;

    [[nodiscard]] auto Get() const noexcept -> ZHLN::World* { return _world.get(); }
    auto               operator->() const noexcept -> ZHLN::World* { return _world.get(); }
    auto               operator*() const noexcept -> ZHLN::World& { return *_world; }
    explicit           operator bool() const noexcept { return _world != nullptr; }

    auto Registry() -> ZHLN::ECS::Registry& { return _world->GetRegistry(); }
    auto Physics() -> ZHLN::PhysicsContext& { return _world->GetPhysics(); }

  private:
    std::unique_ptr<ZHLN::World> _world;
};

} // namespace ZHLN::Test::Fixture
