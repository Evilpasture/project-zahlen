// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/CommonFixtures.hpp
//
// Process-wide RAII lifecycle for the two subsystems a test suite has to bring
// up before any engine or ECS work: the fiber/task system, and Jolt's
// allocator + type registry.
//
// Nine CPU suites and every GPU suite opened with the same
// Fiber::InitMainThread() + TaskSystem::Init(2, 32, kMinimumFiberStackSize)
// and closed with TaskSystem::Shutdown(). As a fixture member the pairing
// cannot be forgotten on an early return, and a suite that needs Jolt composes
// the second scope instead of re-spelling the factory dance.
//
// These are process-wide singletons, so the scopes are non-copyable and
// non-movable, and a binary must not nest two of the same kind.

#pragma once

#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <cstdint>

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

namespace ZHLN::Test::Fixture {

/// Brings up the main-thread fiber and the worker pool; tears the pool down.
///
/// Defaults match every existing call site: 2 workers, 32 fibers, minimum
/// stack. Raise them only for a suite that genuinely saturates the pool.
class TaskSystemScope {
  public:
    explicit TaskSystemScope(uint32_t workerThreads = 2, uint32_t maxFibers = 32) {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(workerThreads, maxFibers, ZHLN::kMinimumFiberStackSize);
    }

    ~TaskSystemScope() {
        ZHLN::TaskSystem::Shutdown();
    }

    TaskSystemScope(const TaskSystemScope&)            = delete;
    TaskSystemScope& operator=(const TaskSystemScope&) = delete;
    TaskSystemScope(TaskSystemScope&&)                 = delete;
    TaskSystemScope& operator=(TaskSystemScope&&)      = delete;
};

/// Registers Jolt's default allocator, factory and type registry; undoes them.
///
/// The factory is created only if one is not already live, matching the guard
/// the physics suites used, so a suite can share a process with another that
/// has already registered types.
class JoltScope {
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

/// Task system + Jolt, torn down in the order the physics suites used:
/// workers first, then the type registry.
class TaskSystemAndJoltScope {
  public:
    explicit TaskSystemAndJoltScope(uint32_t workerThreads = 2, uint32_t maxFibers = 32) : _tasks(workerThreads, maxFibers) {}

    ~TaskSystemAndJoltScope() = default;

    TaskSystemAndJoltScope(const TaskSystemAndJoltScope&)            = delete;
    TaskSystemAndJoltScope& operator=(const TaskSystemAndJoltScope&) = delete;
    TaskSystemAndJoltScope(TaskSystemAndJoltScope&&)                 = delete;
    TaskSystemAndJoltScope& operator=(TaskSystemAndJoltScope&&)      = delete;

  private:
    // Members destruct in reverse declaration order, so _jolt is declared
    // first to get the teardown the physics suites used: workers shut down
    // before the Jolt type registry goes away. Construction order (Jolt then
    // tasks) does not matter -- neither subsystem depends on the other.
    JoltScope       _jolt;
    TaskSystemScope _tasks;
};

} // namespace ZHLN::Test::Fixture
