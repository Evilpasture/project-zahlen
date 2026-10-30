// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/ECS/SystemGraph.hpp>
#include <atomic>
#include <cstddef>
#include <span>

namespace ZHLN::Test {

// Run submitted graph tasks inline and observe only the public scheduling API.
// A task (not the scheduler) must retire its graph completion token.
class MockTaskScheduler final : public ECS::GraphTaskScheduler {
  public:
    void Dispatch(std::span<const TaskSystem::Task> tasks) override {
        _dispatches.fetch_add(1, std::memory_order_relaxed);
        _submitted.fetch_add(tasks.size(), std::memory_order_relaxed);
        for (const auto& task: tasks) {
            task.func(task.arg);
        }
    }

    void Wait(TaskSystem::Counter& completion) override {
        _waits.fetch_add(1, std::memory_order_relaxed);
        if (completion.value.load(std::memory_order_acquire) != 0) {
            _incompleteWaits.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] auto Dispatches() const noexcept -> size_t { return _dispatches.load(std::memory_order_relaxed); }
    [[nodiscard]] auto Submitted() const noexcept -> size_t { return _submitted.load(std::memory_order_relaxed); }
    [[nodiscard]] auto Waits() const noexcept -> size_t { return _waits.load(std::memory_order_relaxed); }
    [[nodiscard]] auto IncompleteWaits() const noexcept -> size_t { return _incompleteWaits.load(std::memory_order_relaxed); }

  private:
    std::atomic<size_t> _dispatches {0};
    std::atomic<size_t> _submitted {0};
    std::atomic<size_t> _waits {0};
    std::atomic<size_t> _incompleteWaits {0};
};

} // namespace ZHLN::Test
