// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Scheduler.hpp
#pragma once

#include <array>
#include <threading/TaskSystem.hpp>

namespace ZHLN {

struct TaskSystemScheduler {
    template <typename... Tasks>
    void Dispatch(Tasks&&... tasks) const {
        constexpr size_t numTasks = sizeof...(Tasks);
        if constexpr (numTasks == 0) {
            return;
        }

        std::array<TaskSystem::Task, numTasks> fiberTasks {};
        size_t                                 idx = 0;

        ((fiberTasks[idx] =
              TaskSystem::Task {
                  .func =
                      [](void* arg) {
                          using DecayedTask = std::decay_t<decltype(tasks)>;
                          auto* taskPtr     = static_cast<DecayedTask*>(arg);
                          (*taskPtr)();
                      },
                  .arg = const_cast<void*>(static_cast<const void*>(std::addressof(tasks)))
              },
          ++idx),
         ...);

        TaskSystem::Counter sync;
        TaskSystem::Dispatch({fiberTasks.data(), numTasks}, &sync);

        // Yield the current fiber cooperatively. The stack frame containing
        // fiberTasks and tasks remains frozen and valid in memory.
        TaskSystem::Wait(&sync);
    }
};

} // namespace ZHLN
