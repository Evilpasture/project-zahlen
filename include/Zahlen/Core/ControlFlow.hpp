// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/ControlFlow.hpp
#pragma once

#include <Zahlen/Threading/Mutex.hpp>
#include <utility>

namespace ZHLN {

/**
 * @brief Higher-order functional mutex lock.
 * Automatically locks the mutex, executes the lambda, and unlocks on exit.
 *
 * Usage:
 *   ZHLN::Lock(myMutex, [&] {
 *       return registry.Create();
 *   });
 */
template <typename MutexT, typename Func>
decltype(auto) Lock(MutexT& mutex, Func&& func) {
    MutexGuard guard(mutex);
    return std::forward<Func>(func)();
}

namespace Internal {
struct FFISafetyGuard {
    void* _reg;
    explicit FFISafetyGuard(void* reg): _reg(reg) {
    }
    ~FFISafetyGuard() = default;
};
} // namespace Internal

} // namespace ZHLN
