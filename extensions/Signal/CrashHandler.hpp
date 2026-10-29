// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Signal/CrashHandler.hpp
//
// Caller-owned crash & terminal signal wiring built on <Signal/SignalManager.hpp>
// and Core's signal-free diagnostic dump hooks (<Zahlen/Core/Diagnostics.hpp>).
// Engine instances hold no CrashState and register no signal handlers; frontends
// own a static CrashState in main(), call SetupSignalHandler(crashState), and
// poll CheckForCrashes(crashState, &engine) in their event loop.

#include <Signal/SignalManager.hpp>
#include <Zahlen/Core/Diagnostics.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Threading/Thread.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#if defined(_WIN32)
    #include <process.h>
#else
    #include <unistd.h>
#endif

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
    std::atomic<bool>     handlersRegistered {false};

    std::atomic<uint32_t>                        observerCount {0};
    std::array<CrashObserverEntry, kMaxCrashObservers> observers {};
};

class CrashDiagnostics {
  public:
    static auto RegisterObserver(CrashState& state, std::string_view name, CrashObserver observer, void* context) noexcept -> bool {
        if (observer == nullptr) {
            return false;
        }
        const uint32_t index = state.observerCount.load(std::memory_order::relaxed);
        if (index >= kMaxCrashObservers) {
            return false;
        }
        state.observers[index] = CrashObserverEntry {.name = name, .observer = observer, .context = context};
        state.observerCount.store(index + 1, std::memory_order::release);
        return true;
    }

    static void ClearObservers(CrashState& state) noexcept {
        state.observerCount.store(0, std::memory_order::release);
    }

    static void Setup(CrashState& state) {
        if (!state.handlersRegistered.exchange(true, std::memory_order::acq_rel)) {
            SignalManager::RegisterHandler(Signal::Interrupt, HandleRequestStop {});
            SignalManager::RegisterHandler(Signal::Terminate, HandleRequestStop {});
            SignalManager::RegisterHandler(Signal::AccessViolation, HandleFatal {.state = &state});
            SignalManager::RegisterHandler(Signal::IllegalInstruction, HandleFatal {.state = &state});
            SignalManager::RegisterHandler(Signal::MathError, HandleFatal {.state = &state});
            SignalManager::RegisterHandler(Signal::BusError, HandleFatal {.state = &state});
            SignalManager::RegisterHandler(Signal::Abort, HandleAbort {});
        }
        Diagnostics::InitializeSymbolResolver();
        SignalManager::Install();
    }

    template <typename DiagnosticTarget = void>
    static void Check(CrashState& state, const DiagnosticTarget* target = nullptr) {
        if (state.pendingPhase.load(std::memory_order::acquire) != 1) {
            return;
        }

        state.pendingPhase.store(-1, std::memory_order::release);
        const SignalEvent ev {
            .signal       = static_cast<Signal>(state.pendingKind.load(std::memory_order::relaxed)),
            .faultAddress = state.faultAddr.load(std::memory_order::relaxed),
            .threadId     = state.pendingThread.load(std::memory_order::relaxed),
        };
        PerformDump<DiagnosticTarget>(state, ev, DumpContext::DeferredMainThread, target);
        std::abort();
    }

  private:
    enum class DumpContext : uint8_t {
        InHandler,
        DeferredMainThread,
    };

    [[noreturn]] static void ImmediateExit(int code) noexcept {
#if defined(_WIN32)
        ::_exit(code);
#else
        ::_exit(code);
#endif
    }

    static void RunObservers(CrashState& state, const SignalEvent& ev) noexcept {
        const uint32_t count = state.observerCount.load(std::memory_order::acquire);
        for (uint32_t i = 0; i < count && i < kMaxCrashObservers; ++i) {
            const CrashObserverEntry& entry = state.observers[i];
            if (entry.observer == nullptr) {
                continue;
            }
            if (!entry.name.empty()) {
                auto header = ZHLN::Format("\n{}--- {} STATE ---{}\n", Color::Cyan, entry.name, Color::Reset);
                Diagnostics::WriteDiagnosticError(header.string_view());
            }
            entry.observer(entry.context, ev);
        }
    }

    template <size_t Capacity, int MaxFrames>
    static void WriteStackTrace() noexcept {
        std::array<char, Capacity> stackBuf {};
        const size_t               len = Diagnostics::CaptureStackTrace(stackBuf, MaxFrames);
        if (len > 0) {
            Diagnostics::WriteDiagnosticError(std::string_view(stackBuf.data(), len));
        } else {
            Diagnostics::WriteDiagnosticError("Not implemented\n");
        }
    }

    template <typename DiagnosticTarget>
    static void PerformDump(CrashState& state, const SignalEvent& ev, DumpContext context, const DiagnosticTarget* target) noexcept {
        const std::string_view sigName = Reflect::EnumToString(ev.signal);
        const void*            addr    = ev.faultAddress;

        auto sigHeader  = ZHLN::Format("\n{}DIAGNOSTIC REPORT FOR SIGNAL: {}{}\n", Color::Red, sigName, Color::Reset);
        auto addrHeader = ZHLN::Format("Faulting Address: {}{}{}\n", Color::Yellow, addr, Color::Reset);
        Diagnostics::WriteDiagnosticError(sigHeader.string_view());
        Diagnostics::WriteDiagnosticError(addrHeader.string_view());

        if (addr != nullptr) {
            Diagnostics::DumpFaultRegion(addr);
        }

        if (context == DumpContext::DeferredMainThread) {
            if constexpr (!std::is_void_v<DiagnosticTarget>) {
                if (target != nullptr) {
                    target->TraceDiagnostics();
                }
            }
            RunObservers(state, ev);
        }

        Diagnostics::WriteDiagnosticError("\nStack Trace:\n");
        if (context == DumpContext::InHandler) {
            WriteStackTrace<4096, 48>();
        } else {
            WriteStackTrace<16384, 128>();
        }
        Diagnostics::WriteDiagnosticError("\n");
    }

    static void ProcessCrash(CrashState& state, const SignalEvent& ev) noexcept {
        if (ev.signal == Signal::Abort) {
            ImmediateExit(1);
        }

        int expected = 0;
        if (!state.pendingPhase.compare_exchange_strong(expected, 1)) {
            if (expected == -1) {
                ZHLN::Println("!! SECONDARY CRASH DURING DIAGNOSTICS. ABORTING !!");
            }
            ImmediateExit(1);
        }

        state.pendingKind.store(static_cast<uint32_t>(ev.signal), std::memory_order::relaxed);
        state.faultAddr.store(ev.faultAddress, std::memory_order::relaxed);
        state.pendingThread.store(ev.threadId, std::memory_order::relaxed);

        if (ZHLN::GetCurrentFiberID() == 1) {
            ZHLN::Print("\n[ZHLN] Terminal signal on Main Thread. Attempting emergency dump...\n");
            state.pendingPhase.store(-1);
            Diagnostics::EmergencyRestoreTerminal();
            PerformDump<void>(state, ev, DumpContext::InHandler, nullptr);
            ImmediateExit(1);
        }

        ZHLN::Print("\n[ZHLN] Signal intercepted in Worker. Main Thread will dump soon...\n");
        while (true) {
            HaltThread();
        }
    }

    struct HandleRequestStop {
        ZHLN_ANNOTATION(ZHLN::SignalSafe {})
        void operator()(const SignalEvent&) const noexcept {
            Diagnostics::EmergencyRestoreTerminal();
            ImmediateExit(0);
        }
    };

    struct HandleAbort {
        ZHLN_ANNOTATION(ZHLN::SignalSafe {})
        void operator()(const SignalEvent&) const noexcept {
            ImmediateExit(1);
        }
    };

    struct HandleFatal {
        CrashState* state = nullptr;

        ZHLN_ANNOTATION(ZHLN::SignalSafe {})
        void operator()(const SignalEvent& ev) const noexcept {
            ProcessCrash(*state, ev);
        }
    };
};

inline void SetupSignalHandler(CrashState& state) {
    CrashDiagnostics::Setup(state);
}

inline void CheckForCrashes(CrashState& state, std::nullptr_t = nullptr) {
    CrashDiagnostics::Check<void>(state, nullptr);
}

template <typename DiagnosticTarget>
inline void CheckForCrashes(CrashState& state, const DiagnosticTarget* target) {
    CrashDiagnostics::Check<DiagnosticTarget>(state, target);
}

} // namespace ZHLN
