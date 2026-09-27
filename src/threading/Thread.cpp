// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

extern "C" void ZHLN_Switch(void** old_sp, void* new_sp);
extern "C" void ZHLN_TrampolineAsm(void);

namespace ZHLN {

namespace {

struct InitialStackFrame {
    size_t size;
    size_t returnAddressOffset;
};

[[nodiscard]] constexpr auto GetInitialStackFrame() noexcept -> InitialStackFrame {
    static_assert(isX64 || isARM64, "Thread.S implements ZHLN_Switch for x86_64 and AArch64 only.");

    if constexpr (isWindows && isX64) {
        return {.size = 240, .returnAddressOffset = 232};
    } else if constexpr (isX64) {
        return {.size = 48 + 8, .returnAddressOffset = 48};
    } else {
        return {.size = 160, .returnAddressOffset = 88};
    }
}

thread_local Fiber  t_mainFiber;
thread_local Fiber* t_currentFiber = nullptr;

extern "C" void ZHLN_Trampoline() {
    Fiber* self = t_currentFiber;
    if (self->func != nullptr) {
        self->func(self->arg);
    }
    self->isFinished = true;

    while (true) {
        Fiber::Yield();
    }
}

void SwapStackBounds(Fiber* target) noexcept {
    const StackBounds outgoing = GetCurrentStackBounds();
    if (outgoing.base == nullptr) {
        return;
    }

    t_currentFiber->bounds = outgoing;
    SetCurrentStackBounds(target->bounds);
}

}

auto GetCurrentFiberID() -> uint64_t {
    if (t_currentFiber == nullptr) {
        return 0;
    }
    if (t_currentFiber->isMain) {
        return 1;
    }

    return std::bit_cast<uint64_t>(t_currentFiber);
}

auto Fiber::GetCurrent() noexcept -> Fiber* {
    return t_currentFiber;
}

void Fiber::InitMainThread() noexcept {
    if (t_currentFiber != nullptr) {
        return;
    }

    t_mainFiber.isFinished = false;
    t_mainFiber.isMain     = true;
    t_mainFiber.caller     = nullptr;

    t_mainFiber.bounds = GetCurrentStackBounds();

    t_currentFiber = &t_mainFiber;
}

auto Fiber::Create(size_t stackSize, FiberFunc func, void* arg) noexcept -> Fiber* {
    const size_t        requested = std::max(stackSize, kMinimumFiberStackSize);
    const GuardedRegion stack     = AllocateGuardedRegion(requested);
    if (!stack.valid()) {
        return nullptr;
    }
    const uintptr_t stackTop = std::bit_cast<uintptr_t>(stack.end);

    static_assert(std::has_single_bit(alignof(Fiber)));
    const uintptr_t structAddr = (stackTop - sizeof(Fiber)) & ~(static_cast<uintptr_t>(alignof(Fiber)) - 1u);
    auto* const     fiber      = std::construct_at(std::bit_cast<Fiber*>(structAddr));

    fiber->mapAddr    = stack.base;
    fiber->mapSize    = stack.size;
    fiber->bounds     = {.base = stack.end, .limit = stack.begin};
    fiber->func       = func;
    fiber->arg        = arg;
    fiber->caller     = nullptr;
    fiber->isFinished = false;
    fiber->isMain     = false;
    fiber->isRunning.store(false, std::memory_order::relaxed);

    constexpr InitialStackFrame frame = GetInitialStackFrame();
    const uintptr_t             sp    = structAddr - frame.size;

    *std::bit_cast<uintptr_t*>(sp + frame.returnAddressOffset) = reinterpret_cast<uintptr_t>(ZHLN_TrampolineAsm);

    fiber->stackPointer = std::bit_cast<void*>(sp);
    return fiber;
}

void Fiber::Resume(Fiber* target) noexcept {
    while (target->isRunning.exchange(true, std::memory_order::acq_rel)) {
        CPURelax();
    }

    Fiber* self    = t_currentFiber;
    target->caller = self;

    SwapStackBounds(target);
    t_currentFiber = target;

    ZHLN_Switch(&self->stackPointer, target->stackPointer);

    target->isRunning.store(false, std::memory_order::release);
}

void Fiber::Yield() noexcept {
    Fiber* self   = t_currentFiber;
    Fiber* target = self->caller;
    if (target == nullptr) {
        return;
    }

    SwapStackBounds(target);
    t_currentFiber = target;
    ZHLN_Switch(&self->stackPointer, target->stackPointer);
}

void Fiber::Destroy(Fiber* fiber) noexcept {
    if ((fiber == nullptr) || fiber->isMain) {
        return;
    }
    const GuardedRegion stack = {.base = fiber->mapAddr, .size = fiber->mapSize};
    std::destroy_at(fiber);
    FreeGuardedRegion(stack);
}

auto GetCurrentFiber() noexcept -> Fiber* {
    return Fiber::GetCurrent();
}

void YieldFiber() noexcept {
    Fiber::Yield();
}

}
