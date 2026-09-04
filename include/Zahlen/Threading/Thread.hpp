// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Platform.hpp>
#include <atomic>
#include <cstddef>
namespace ZHLN {

// Jolt collision jobs and modern libc++ frames can exceed 128 KiB on ARM64.
// Keep enough usable stack for physics callbacks plus the fiber trampoline.
inline constexpr size_t kMinimumFiberStackSize = 512u * 1024u;

using FiberFunc = void (*)(void*);

/**
 * @brief High-performance Stackful Coroutine (Fiber).
 * Wraps the mag_asm backend in a C++ interface.
 */
struct alignas(128) Fiber {
    void*     stackPointer; // Offset 0: Used by ZHLN_Switch
    void*     mapAddr;      // Base of the stack mapping (AllocateGuardedRegion)
    size_t    mapSize;      // Total size including guard pages
    FiberFunc func;         // Entry point
    void*     arg;          // User data
    Fiber*    caller;       // Parent fiber to return to on Yield

    // The stack bounds the OS has recorded for this fiber. Swapped in and out
    // of the platform's per-thread state on every switch; stays zeroed and
    // unused where the OS tracks nothing (see GetCurrentStackBounds()).
    StackBounds bounds {};

    bool              isFinished;
    bool              isMain;
    std::atomic<bool> isRunning;
    // Set by the fiber right before its final Yield of a task, read by the
    // resumer after Resume() returns: "this fiber finished its task, recycle
    // it". Fibers that yield while blocked (mutex/condvar/counter waits)
    // leave it clear, so resumers never recycle a fiber that is still owed a
    // wakeup. Only the resumer may recycle -- a fiber that publishes itself
    // as free BEFORE suspending can be claimed by a second thread while it
    // is still running, which double-owns the fiber and corrupts the pool.
    std::atomic<bool> taskDone {false};

    // Static API for Mutex/Scheduler access
    static auto GetCurrent() noexcept -> Fiber*;
    static void Yield() noexcept;
    static void Resume(Fiber* target) noexcept;

    // Allocation
    static auto Create(size_t stackSize, FiberFunc func, void* arg) noexcept -> Fiber*;
    static void Destroy(Fiber* fiber) noexcept;

    /**
     * @brief Converts the current OS thread into the "Main" Fiber.
     * Must be called once per thread before using fibers or Mutexes.
     */
    static void InitMainThread() noexcept;
};

// Global Linker Satellites for Mutex.cpp
auto GetCurrentFiber() noexcept -> Fiber*;
void YieldFiber() noexcept;

} // namespace ZHLN
