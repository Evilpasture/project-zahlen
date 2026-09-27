// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/SignalSafe.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN {

using CrashObserver = void (*)(void* context, const SignalEvent& event) noexcept;

inline constexpr size_t kMaxCrashObservers = 8;

struct CrashObserverEntry {
    std::string_view name {};
    CrashObserver    observer {nullptr};
    void*            context {nullptr};
};

struct CrashState {

    std::atomic<int>      pendingPhase {0};
    std::atomic<uint32_t> pendingKind {0};
    std::atomic<void*>    faultAddr {nullptr};
    std::atomic<uint64_t> pendingThread {0};

    std::atomic<bool> handlersRegistered {false};


    std::atomic<uint32_t> observerCount {0};
    CrashObserverEntry    observers[kMaxCrashObservers] {};
};

}
