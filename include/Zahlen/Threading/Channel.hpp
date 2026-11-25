// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <atomic>
#include <cstddef>
#include <optional>
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
        Waiter wakeTarget {};
        bool   hasWaiter = false;

        ZHLN::Lock(_mutex, [&] {
            if (!_waiters.empty()) {
                wakeTarget = _waiters.front();
                _waiters.pop();

                *wakeTarget.outMsg = std::move(msg);
                hasWaiter          = true;
            } else {
                _queue.push(std::move(msg));
            }
        });

        if (hasWaiter) {
            // Two-phase wake: store Waking (1) before TaskSystem::WakeUp so the
            // waiting fiber cannot yield again, then store Done (2) after WakeUp
            // returns so the fiber cannot exit Wait(), destroy its stack frame,
            // or recycle back into TaskSystem's freeQueue before queued is set.
            wakeTarget.wakeState->store(1, std::memory_order::seq_cst);
            TaskSystem::WakeUp(wakeTarget.fiber);
            wakeTarget.wakeState->store(2, std::memory_order::release);
        }
    }

    void Push(const T& msg) {
        T copy = msg;
        Push(std::move(copy));
    }

    // Blocking receive: synchronously waits (or fiber-yields) for an item.
    T Wait() {
        Fiber* self = Fiber::GetCurrent();

        if ((self == nullptr) || self->isMain) {
            return PopBlocking();
        }

        T                    result;
        std::atomic<uint8_t> wakeState {0};
        bool                 got = false;

        ZHLN::Lock(_mutex, [&] {
            if (!_queue.empty()) {
                result = std::move(_queue.front());
                _queue.pop();
                got = true;
                return;
            }

            _waiters.push(Waiter {.fiber = self, .outMsg = &result, .wakeState = &wakeState});
        });

        if (got) {
            return result;
        }

        while (wakeState.load(std::memory_order::seq_cst) == 0) {
            Fiber::Yield();
        }
        while (wakeState.load(std::memory_order::acquire) < 2) {
            CPURelax();
        }

        return result;
    }

    // Non-blocking receive: `Poll` names the waiting policy, the optional
    // names data presence.
    [[nodiscard]] auto Poll() -> std::optional<T> {
        return ZHLN::Lock(_mutex, [&] -> std::optional<T> {
            if (_queue.empty()) {
                return std::nullopt;
            }
            T msg = std::move(_queue.front());
            _queue.pop();
            return msg;
        });
    }

    size_t Size() const {
        return ZHLN::Lock(_mutex, [&] { return _queue.size(); });
    }

  private:
    struct Waiter {
        Fiber*                fiber     = nullptr;
        T*                    outMsg    = nullptr;
        std::atomic<uint8_t>* wakeState = nullptr;
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

}
