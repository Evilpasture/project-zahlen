// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "diagnostics/CrashObservers.hpp"
#include "diagnostics/DiagnosticsInternal.hpp"
#include "tty/TTYBackend.hpp"
#include <Zahlen/Core/CrashState.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/SignalManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace ZHLN {

namespace {

enum class DumpContext : uint8_t {
    InHandler,
    DeferredMainThread,
};

void RunCrashObservers(CrashState& state, const SignalEvent& ev) noexcept {
    const uint32_t count = state.observerCount.load(std::memory_order::acquire);
    for (uint32_t i = 0; i < count && i < kMaxCrashObservers; ++i) {
        const CrashObserverEntry& entry = state.observers[i];
        if (entry.observer == nullptr) {
            continue;
        }

        auto header = ZHLN::Format("\n{}--- {} STATE ---{}\n", Color::Cyan, entry.name, Color::Reset);
        Diagnostics::WriteErr(header.string_view());

        entry.observer(entry.context, ev);
    }
}

template <size_t Capacity, int MaxFrames>
void WriteStackTrace() {
    char         stackBuf[Capacity] {};
    const size_t len = Diagnostics::CaptureStackTrace(stackBuf, MaxFrames);
    if (len > 0) {
        Diagnostics::WriteErr(std::string_view(stackBuf, len));
    } else {
        Diagnostics::WriteErr("Not implemented\n");
    }
}

void PerformDiagnosticDump(CrashState& state, const SignalEvent& ev, DumpContext context, bool engineAlive) {
    const std::string_view sigName = Reflect::EnumToString(ev.signal);
    const void*            addr    = ev.faultAddress;

    auto sig_header  = ZHLN::Format("\n{}DIAGNOSTIC REPORT FOR SIGNAL: {}{}\n", Color::Red, sigName, Color::Reset);
    auto addr_header = ZHLN::Format("Faulting Address: {}{}{}\n", Color::Yellow, addr, Color::Reset);
    Diagnostics::WriteErr(sig_header.string_view());
    Diagnostics::WriteErr(addr_header.string_view());

    if (addr != nullptr) {
        Diagnostics::DumpFaultRegion(addr);
    }

    if (context == DumpContext::DeferredMainThread && engineAlive) {
        RunCrashObservers(state, ev);
    }

    Diagnostics::WriteErr("\nStack Trace:\n");

    if (context == DumpContext::InHandler) {
        WriteStackTrace<4096, 48>();
    } else {
        WriteStackTrace<16384, 128>();
    }
    Diagnostics::WriteErr("\n");
}

void ProcessCrash(CrashState& state, const SignalEvent& ev) {
    if (ev.signal == Signal::Abort) {
        _exit(1);
    }

    int expected = 0;
    if (!state.pendingPhase.compare_exchange_strong(expected, 1)) {
        if (expected == -1) {
            ZHLN::Println("!! SECONDARY CRASH DURING DIAGNOSTICS. ABORTING !!");
        }
        _exit(1);
    }

    state.pendingKind.store(static_cast<uint32_t>(ev.signal), std::memory_order::relaxed);
    state.faultAddr.store(ev.faultAddress, std::memory_order::relaxed);
    state.pendingThread.store(ev.threadId, std::memory_order::relaxed);

    if (ZHLN::GetCurrentFiberID() == 1) {
        ZHLN::Print("\n[ZHLN] Terminal signal on Main Thread. Attempting emergency dump...\n");

        state.pendingPhase.store(-1);
        TTYBackend::EmergencyRestore();
        PerformDiagnosticDump(state, ev, DumpContext::InHandler, false);
        _exit(1);
    }

    ZHLN::Print("\n[ZHLN] Signal intercepted in Worker. Main Thread will dump soon...\n");
    while (true) {
        HaltThread();
    }
}


struct HandleRequestStop {
    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const SignalEvent&) const noexcept {
        TTYBackend::EmergencyRestore();
        _exit(0);
    }
};

struct HandleAbort {
    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const SignalEvent&) const noexcept {
        _exit(1);
    }
};

struct HandleFatal {
    CrashState* state = nullptr;

    ZHLN_ANNOTATION(ZHLN::SignalSafe {})
    void operator()(const SignalEvent& ev) const noexcept {
        ProcessCrash(*state, ev);
    }
};

}

namespace Diagnostics {

auto RegisterCrashObserver(CrashState& state, std::string_view name, CrashObserver observer, void* context) noexcept -> bool {
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

void ClearCrashObservers(CrashState& state) noexcept {
    state.observerCount.store(0, std::memory_order::release);
}

void WriteCrashOutput(std::string_view text) noexcept {
    WriteErr(text);
}

}

void CheckForCrashes(CrashState& state, Engine* engine) {
    if (state.pendingPhase.load(std::memory_order::acquire) != 1) {
        return;
    }

    const bool engineAlive = (engine != nullptr);

    state.pendingPhase.store(-1, std::memory_order::release);
    const SignalEvent ev {
        .signal       = static_cast<Signal>(state.pendingKind.load(std::memory_order::relaxed)),
        .faultAddress = state.faultAddr.load(std::memory_order::relaxed),
        .threadId     = state.pendingThread.load(std::memory_order::relaxed),
    };
    PerformDiagnosticDump(state, ev, DumpContext::DeferredMainThread, engineAlive);
    std::abort();
}

void SetupSignalHandler(CrashState& state) {
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

}
