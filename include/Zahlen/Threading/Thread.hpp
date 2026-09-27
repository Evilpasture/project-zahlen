// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Platform.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace ZHLN {

inline constexpr size_t kMinimumFiberStackSize = 512u * 1024u;

using FiberFunc = void (*)(void*);

struct alignas(128) Fiber {
    void*     stackPointer;
    void*     mapAddr;
    size_t    mapSize;
    FiberFunc func;
    void*     arg;
    Fiber*    caller;

    StackBounds bounds {};

    bool              isFinished;
    bool              isMain;
    std::atomic<bool> isRunning;
    std::atomic<bool> taskDone {false};

    static auto GetCurrent() noexcept -> Fiber*;
    static void Yield() noexcept;
    static void Resume(Fiber* target) noexcept;

    static auto Create(size_t stackSize, FiberFunc func, void* arg) noexcept -> Fiber*;
    static void Destroy(Fiber* fiber) noexcept;

    static void InitMainThread() noexcept;
};

auto GetCurrentFiber() noexcept -> Fiber*;
void YieldFiber() noexcept;

auto GetCurrentFiberID() -> uint64_t;

}
