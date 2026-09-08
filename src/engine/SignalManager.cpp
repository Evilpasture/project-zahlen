// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/SignalManager.hpp>
#include <atomic>
#include <cstddef>
#include <csignal>
#include <cstring>
#include <mutex>

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace ZHLN {
namespace {

constexpr uint32_t kMaxHandlers = 32;
constexpr uint32_t kStateBytes  = SignalManager::kMaxHandlerState;

struct HandlerSlot {
    std::atomic<uint32_t>          id {0};
    Signal                         signal {};
    SignalManager::Handler         fn {nullptr};
    SignalManager::StatefulHandler stateful {nullptr};
    alignas(16) unsigned char      state[kStateBytes] {};
};

HandlerSlot               s_slots[kMaxHandlers] {};
std::atomic<uint32_t>     s_nextId {1};
std::atomic<bool>         s_installed {false};
std::mutex                s_mu;

auto CurrentNativeThreadId() noexcept -> uint64_t {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<uint64_t>(syscall(SYS_gettid));
#elif defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid;
#else
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#endif
}

#ifndef _WIN32

constexpr int kPosixSignals[] = {SIGINT, SIGTERM, SIGQUIT, SIGUSR1, SIGUSR2, SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGABRT};

auto FromNative(int sig) noexcept -> Signal {
    switch (sig) {
        case SIGINT:
            return Signal::Interrupt;
        case SIGTERM:
            return Signal::Terminate;
        case SIGQUIT:
            return Signal::Quit;
        case SIGUSR1:
            return Signal::User1;
        case SIGUSR2:
            return Signal::User2;
        case SIGSEGV:
            return Signal::AccessViolation;
        case SIGILL:
            return Signal::IllegalInstruction;
        case SIGFPE:
            return Signal::MathError;
        case SIGBUS:
            return Signal::BusError;
        case SIGABRT:
            return Signal::Abort;
        default:
            return Signal::Interrupt;
    }
}

auto IsKnownNative(int sig) noexcept -> bool {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
        case SIGQUIT:
        case SIGUSR1:
        case SIGUSR2:
        case SIGSEGV:
        case SIGILL:
        case SIGFPE:
        case SIGBUS:
        case SIGABRT:
            return true;
        default:
            return false;
    }
}

struct sigaction s_previous[sizeof(kPosixSignals) / sizeof(kPosixSignals[0])] {};
bool             s_havePrevious[sizeof(kPosixSignals) / sizeof(kPosixSignals[0])] {};

void PosixHandler(int sig, siginfo_t* info, void* /*context*/) {
    if (!IsKnownNative(sig)) {
        return;
    }
    const SignalEvent ev {
        .signal       = FromNative(sig),
        .faultAddress = (info != nullptr) ? info->si_addr : nullptr,
        .threadId     = CurrentNativeThreadId(),
    };
    SignalManager::Dispatch(ev);
}

#else

PVOID s_veh = nullptr;
bool  s_consoleHooked = false;

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    Signal sig = Signal::Interrupt;
    switch (type) {
        case CTRL_C_EVENT:
            sig = Signal::Interrupt;
            break;
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            sig = Signal::Terminate;
            break;
        default:
            return FALSE;
    }
    const SignalEvent ev {
        .signal       = sig,
        .faultAddress = nullptr,
        .threadId     = CurrentNativeThreadId(),
    };
    SignalManager::Dispatch(ev);
    return TRUE;
}

LONG WINAPI VectoredCrashHandler(PEXCEPTION_POINTERS info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    Signal sig {};
    switch (info->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
            sig = Signal::AccessViolation;
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            sig = Signal::IllegalInstruction;
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            sig = Signal::MathError;
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
    const SignalEvent ev {
        .signal       = sig,
        .faultAddress = info->ExceptionRecord->ExceptionAddress,
        .threadId     = CurrentNativeThreadId(),
    };
    SignalManager::Dispatch(ev);
    return EXCEPTION_CONTINUE_SEARCH;
}

void WinAbortHandler(int /*sig*/) {
    const SignalEvent ev {
        .signal       = Signal::Abort,
        .faultAddress = nullptr,
        .threadId     = CurrentNativeThreadId(),
    };
    SignalManager::Dispatch(ev);
}

#endif

auto AllocateId() noexcept -> uint32_t {
    uint32_t id = s_nextId.fetch_add(1, std::memory_order::relaxed);
    if (id == SignalManager::InvalidId) {
        id = s_nextId.fetch_add(1, std::memory_order::relaxed);
    }
    return id;
}

} // namespace

void SignalManager::Install() noexcept {
    const std::lock_guard lock(s_mu);
    if (s_installed.load(std::memory_order::relaxed)) {
        return;
    }

#ifdef _WIN32
    s_veh = AddVectoredExceptionHandler(1, VectoredCrashHandler);
    s_consoleHooked = SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE) != 0;
    std::signal(SIGABRT, WinAbortHandler);
#else
    struct sigaction sa {};
    sa.sa_sigaction = PosixHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;

    for (size_t i = 0; i < sizeof(kPosixSignals) / sizeof(kPosixSignals[0]); ++i) {
        s_havePrevious[i] = sigaction(kPosixSignals[i], &sa, &s_previous[i]) == 0;
    }
#endif
    s_installed.store(true, std::memory_order::release);
}

void SignalManager::Uninstall() noexcept {
    const std::lock_guard lock(s_mu);
    if (!s_installed.load(std::memory_order::relaxed)) {
        return;
    }

#ifdef _WIN32
    if (s_veh != nullptr) {
        RemoveVectoredExceptionHandler(s_veh);
        s_veh = nullptr;
    }
    if (s_consoleHooked) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        s_consoleHooked = false;
    }
    std::signal(SIGABRT, SIG_DFL);
#else
    for (size_t i = 0; i < sizeof(kPosixSignals) / sizeof(kPosixSignals[0]); ++i) {
        if (s_havePrevious[i]) {
            sigaction(kPosixSignals[i], &s_previous[i], nullptr);
            s_havePrevious[i] = false;
        }
    }
#endif
    s_installed.store(false, std::memory_order::release);
}

auto SignalManager::IsInstalled() noexcept -> bool {
    return s_installed.load(std::memory_order::acquire);
}

auto SignalManager::RegisterHandlerInternal(Signal sig, Handler handler) -> uint32_t {
    if (handler == nullptr) {
        return InvalidId;
    }
    const std::lock_guard lock(s_mu);
    for (uint32_t i = 0; i < kMaxHandlers; ++i) {
        uint32_t expected = 0;
        if (s_slots[i].id.compare_exchange_strong(expected, 0xFFFFFFFFu, std::memory_order::acq_rel)) {
            s_slots[i].signal   = sig;
            s_slots[i].fn       = handler;
            s_slots[i].stateful = nullptr;
            const uint32_t id   = AllocateId();
            s_slots[i].id.store(id, std::memory_order::release);
            return id;
        }
    }
    return InvalidId;
}

auto SignalManager::RegisterStatefulInternal(Signal sig, StatefulHandler handler, const void* state, uint32_t size) -> uint32_t {
    if (handler == nullptr || state == nullptr || size == 0 || size > kStateBytes) {
        return InvalidId;
    }
    const std::lock_guard lock(s_mu);
    for (uint32_t i = 0; i < kMaxHandlers; ++i) {
        uint32_t expected = 0;
        if (s_slots[i].id.compare_exchange_strong(expected, 0xFFFFFFFFu, std::memory_order::acq_rel)) {
            s_slots[i].signal   = sig;
            s_slots[i].fn       = nullptr;
            s_slots[i].stateful = handler;
            std::memcpy(s_slots[i].state, state, size);
            const uint32_t id = AllocateId();
            s_slots[i].id.store(id, std::memory_order::release);
            return id;
        }
    }
    return InvalidId;
}

void SignalManager::Unregister(uint32_t id) noexcept {
    if (id == InvalidId) {
        return;
    }
    const std::lock_guard lock(s_mu);
    for (uint32_t i = 0; i < kMaxHandlers; ++i) {
        if (s_slots[i].id.load(std::memory_order::acquire) == id) {
            s_slots[i].fn       = nullptr;
            s_slots[i].stateful = nullptr;
            s_slots[i].id.store(0, std::memory_order::release);
            return;
        }
    }
}

void SignalManager::Dispatch(const SignalEvent& ev) noexcept {
    for (uint32_t i = 0; i < kMaxHandlers; ++i) {
        const uint32_t id = s_slots[i].id.load(std::memory_order::acquire);
        if (id == InvalidId || id == 0xFFFFFFFFu) {
            continue;
        }
        if (s_slots[i].signal != ev.signal) {
            continue;
        }
        if (s_slots[i].id.load(std::memory_order::acquire) != id) {
            continue;
        }
        if (s_slots[i].fn != nullptr) {
            s_slots[i].fn(ev);
        } else if (s_slots[i].stateful != nullptr) {
            s_slots[i].stateful(ev, s_slots[i].state);
        }
    }
}

} // namespace ZHLN
