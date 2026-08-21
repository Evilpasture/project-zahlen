// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <queue>
#include <type_traits>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace ZHLN {

template <typename T>
class Channel {
  public:
    Channel()  = default;
    ~Channel() = default;

    Channel(const Channel&)            = delete;
    Channel& operator=(const Channel&) = delete;

    void Push(T&& msg) {
        ZHLN::Lock(_mutex, [&] {
            if (!_waiters.empty()) {
                Waiter waiter = _waiters.front();
                _waiters.pop();

                *waiter.outMsg = std::move(msg);
                waiter.signaled->store(true, std::memory_order::release);

                TaskSystem::WakeUp(waiter.fiber);
            } else {
                _queue.push(std::move(msg));
            }
        });
    }

    void Push(const T& msg) {
        T copy = msg;
        Push(std::move(copy));
    }

    T Pop() {
        Fiber* self = Fiber::GetCurrent();

        if ((self == nullptr) || self->isMain) {
            return PopBlocking();
        }

        T                  result;
        ZHLN::Atomic<bool> signaled {false};

        ZHLN::Lock(_mutex, [&] {
            if (!_queue.empty()) {
                result = std::move(_queue.front());
                _queue.pop();
                return;
            }

            _waiters.push(Waiter {.fiber = self, .outMsg = &result, .signaled = &signaled});
        });

        if (!_queue.empty() && signaled.load(std::memory_order::acquire)) {
            return result;
        }

        while (!signaled.load(std::memory_order::acquire)) {
            Fiber::Yield();
        }

        return result;
    }

    bool TryPop(T& outMsg) {
        return ZHLN::Lock(_mutex, [&] {
            if (_queue.empty()) {
                return false;
            }
            outMsg = std::move(_queue.front());
            _queue.pop();
            return true;
        });
    }

    size_t Size() const {
        return ZHLN::Lock(_mutex, [&] { return _queue.size(); });
    }

  private:
    struct Waiter {
        Fiber*              fiber;
        T*                  outMsg;
        ZHLN::Atomic<bool>* signaled;
    };

    T PopBlocking() {
        for (;;) {
            T    result;
            bool popped = false;
            ZHLN::Lock(_mutex, [&] {
                if (!_queue.empty()) {
                    result = std::move(_queue.front());
                    _queue.pop();
                    popped = true;
                }
            });
            if (popped) {
                return result;
            }
            CPURelax();
        }
    }

    mutable ZHLN::Mutex _mutex {};
    std::queue<T>       _queue;
    std::queue<Waiter>  _waiters;
};

} // namespace ZHLN
