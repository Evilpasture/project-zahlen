// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


// src/threading/Channel.hpp
#pragma once

#include <queue>
#include <threading/Mutex.hpp>
#include <threading/Thread.hpp>
#include <threading/TaskSystem.hpp>
#include <detail/Atomic.hpp>
#include <detail/ControlFlow.hpp>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace ZHLN {

template <typename T>
class Channel {
public:
    Channel() = default;
    ~Channel() = default;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /**
     * @brief Sends a message into the channel. If a fiber is suspended waiting,
     * this writes directly to its stack and schedules it for immediate resumption.
     */
    void Push(T&& msg) {
        ZHLN_LOCK(_mutex) {
            if (!_waiters.empty()) {
                // Direct-pass optimization: bypass the queue entirely
                Waiter waiter = _waiters.front();
                _waiters.pop();

                *waiter.outMsg = std::move(msg);
                waiter.signaled->store(true, std::memory_order_release);
                
                // Return the fiber to the task system queue
                TaskSystem::WakeUp(waiter.fiber);
            } else {
                _queue.push(std::move(msg));
            }
        }
    }

    void Push(const T& msg) {
        T copy = msg;
        Push(std::move(copy));
    }

    /**
     * @brief Receives a message. If the channel is empty, suspends the calling
     * fiber (yielding to the scheduler) until a writer pushes a message.
     */
    T Pop() {
        Fiber* self = Fiber::GetCurrent();
        
        // Fallback for raw OS threads / main thread (where we cannot yield)
        if ((self == nullptr) || self->isMain) {
            return PopBlocking();
        }

        T result;
        ZHLN::Atomic<bool> signaled{false};

        ZHLN_LOCK(_mutex) {
            if (!_queue.empty()) {
                result = std::move(_queue.front());
                _queue.pop();
                return result;
            }

            // Queue is empty. Register the fiber as suspended.
            // Safely passing pointers to 'result' and 'signaled' because
            // the stack frame is preserved while this fiber is yielded.
            _waiters.push(Waiter{
                .fiber = self,
                .outMsg = &result,
                .signaled = &signaled
            });
        }

        // Suspend the fiber back to the worker thread scheduler
        while (!signaled.load(std::memory_order_acquire)) {
            Fiber::Yield();
        }

        return result;
    }

    bool TryPop(T& outMsg) {
        ZHLN_LOCK(_mutex) {
            if (_queue.empty()) {
                return false;
            }
            outMsg = std::move(_queue.front());
            _queue.pop();
            return true;
        }
    }

    size_t Size() const {
        ZHLN_LOCK(_mutex) {
            return _queue.size();
        }
    }

private:
    struct Waiter {
        Fiber* fiber;
        T* outMsg;
        ZHLN::Atomic<bool>* signaled;
    };

    T PopBlocking() {
        for (;;) {
            ZHLN_LOCK(_mutex) {
                if (!_queue.empty()) {
                    T result = std::move(_queue.front());
                    _queue.pop();
                    return result;
                }
            }
            // CPU relaxation for non-fiber thread spinning
            #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
            #elif defined(__aarch64__)
                __asm__ __volatile__("yield" ::: "memory");
            #else
                std::this_thread::yield();
            #endif
        }
    }

    mutable ZHLN::Mutex _mutex{};
    std::queue<T> _queue;
    std::queue<Waiter> _waiters;
};

} // namespace ZHLN