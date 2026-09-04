// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Config.hpp>
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

// Defined in Thread.S
extern "C" void ZHLN_Switch(void** old_sp, void* new_sp);
extern "C" void ZHLN_TrampolineAsm(void);

namespace ZHLN {

namespace {

/**
 * @brief The stack frame a brand new fiber starts executing on.
 *
 * ZHLN_Switch (Thread.S) restores the callee-saved registers off the frame it
 * is handed and then returns into that frame's return-address slot, which is
 * how a freshly created fiber reaches ZHLN_Trampoline. Both numbers are
 * dictated by the assembly: `size` is the total number of bytes the switch
 * pops before returning, and `returnAddressOffset` is where the return address
 * sits, relative to the stack pointer handed to it.
 */
struct InitialStackFrame {
    size_t size;                // Total bytes ZHLN_Switch pops.
    size_t returnAddressOffset; // Offset of the return-address slot.
};

[[nodiscard]] constexpr auto GetInitialStackFrame() noexcept -> InitialStackFrame {
    static_assert(isX64 || isARM64, "Thread.S implements ZHLN_Switch for x86_64 and AArch64 only.");

    if constexpr (isWindows && isX64) {
        // Win64: 10 XMMs (160) + 9 GPRs (72) + the return address (8).
        return {.size = 240, .returnAddressOffset = 232};
    } else if constexpr (isX64) {
        // System V: the return address sits above the 6 callee-saved GPRs (48).
        return {.size = 48 + 8, .returnAddressOffset = 48};
    } else {
        // AArch64: x19-x30 + d8-d15 (160); X30 is the link register.
        return {.size = 160, .returnAddressOffset = 88};
    }
}

// Thread-local tracking of the active fiber
thread_local Fiber  t_mainFiber;
thread_local Fiber* t_currentFiber = nullptr;

/**
 * @brief The bridge between Assembly and C++.
 * This is the first code executed on a new fiber's stack.
 */
extern "C" void ZHLN_Trampoline() {
    Fiber* self = t_currentFiber;
    if (self->func != nullptr) {
        self->func(self->arg);
    }
    self->isFinished = true;

    // Fiber has returned. Yield back to the caller indefinitely.
    while (true) {
        Fiber::Yield();
    }
}

// Windows caches the active stack bounds in the TEB (Thread Environment Block);
// the kernel, stack probes and SEH all read them from there, so they have to
// follow the stack we switch to. On platforms that keep no such state
// GetCurrentStackBounds() reports an empty range and this is a no-op.
void SwapStackBounds(Fiber* target) noexcept {
    const StackBounds outgoing = GetCurrentStackBounds();
    if (outgoing.base == nullptr) {
        return; // Platform tracks nothing, nothing to swap.
    }

    t_currentFiber->bounds = outgoing;
    SetCurrentStackBounds(target->bounds);
}

} // namespace

auto GetCurrentFiberID() -> uint64_t {
    if (t_currentFiber == nullptr) {
        return 0; // Not a fiber-managed thread
    }
    if (t_currentFiber->isMain) {
        return 1; // Friendly ID for main thread
    }

    // For worker fibers, return the memory address as a unique ID
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

    // Adopt the bounds the OS already picked for this thread's real stack.
    t_mainFiber.bounds = GetCurrentStackBounds();

    t_currentFiber = &t_mainFiber;
}

auto Fiber::Create(size_t stackSize, FiberFunc func, void* arg) noexcept -> Fiber* {
    // 1. Allocate the stack: page aligned, with a guard page on both ends so
    //    that overflowing or underflowing faults instead of corrupting its
    //    neighbours. Allocation/protection is a platform concern.
    const size_t        requested = std::max(stackSize, kMinimumFiberStackSize);
    const GuardedRegion stack     = AllocateGuardedRegion(requested);
    if (!stack.valid()) {
        return nullptr;
    }
    const uintptr_t stackTop = std::bit_cast<uintptr_t>(stack.end);

    // 2. Construct aligned metadata immediately below the usable stack top.
    // Fiber is alignas(128); the previous 16-byte placement was undefined on
    // ARM64 and could fault when its atomic running flag was accessed.
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

    // 3. Seed the stack frame ZHLN_Switch expects: it pops the saved registers
    //    and returns into the trampoline, which is how a new fiber starts.
    constexpr InitialStackFrame frame = GetInitialStackFrame();
    const uintptr_t             sp    = structAddr - frame.size;

    *std::bit_cast<uintptr_t*>(sp + frame.returnAddressOffset) = reinterpret_cast<uintptr_t>(ZHLN_TrampolineAsm);

    fiber->stackPointer = std::bit_cast<void*>(sp);
    return fiber;
}

void Fiber::Resume(Fiber* target) noexcept {
    // Claim the target atomically: a load-then-store pair lets two resumers
    // both pass the guard and jump onto the same stack. The spin still
    // ensures the previous resumer has fully vacated the stack before we
    // jump into it.
    while (target->isRunning.exchange(true, std::memory_order::acq_rel)) {
        CPURelax();
    }

    Fiber* self    = t_currentFiber;
    target->caller = self;

    SwapStackBounds(target);
    t_currentFiber = target;

    // Execute Assembly Context Switch
    ZHLN_Switch(&self->stackPointer, target->stackPointer);

    // WE ARE BACK! The target has yielded to us, meaning it is safely off its stack!
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

} // namespace ZHLN
