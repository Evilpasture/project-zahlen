// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/SignalManager.hpp
//
// Register portable signal handlers without including POSIX or Windows
// signal headers. OS install lives in SignalManager.cpp.

#pragma once

#include <Zahlen/Core/SignalSafetyInspector.hpp>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ZHLN {

class SignalManager {
  public:
    using Handler         = void (*)(const SignalEvent&) noexcept;
    using StatefulHandler = void (*)(const SignalEvent&, void* state) noexcept;

    static constexpr uint32_t InvalidId        = 0;
    static constexpr uint32_t kMaxHandlerState = 64;

    /// Hook OS signals / VEH. Idempotent. Does not register any handlers.
    static void Install() noexcept;

    /// Restore previous OS handlers. Registered callbacks stay in the table.
    static void Uninstall() noexcept;

    [[nodiscard]] static auto IsInstalled() noexcept -> bool;

    /// NTTP handler. Requires SignalSafe on the function value, its type, or
    /// operator() (see Reflect::FunctionHasAnnotation).
    template <auto Fn>
    static auto RegisterSafeHandler(Signal sig) -> uint32_t {
        static_assert(
            Reflect::FunctionHasAnnotation<SignalSafe, Fn>(),
            "RegisterSafeHandler requires ZHLN_ANNOTATION(ZHLN::SignalSafe{}) on the handler"
        );
        static_assert(
            std::is_nothrow_invocable_v<decltype(Fn), const SignalEvent&>, "handler must be invocable as void(const SignalEvent&) noexcept"
        );
        return RegisterHandlerInternal(sig, [](const SignalEvent& ev) noexcept { Fn(ev); });
    }

    /// Runtime callable. Empty annotated functors collapse to a function
    /// pointer; non-empty trivially-copyable state is stored in the slot.
    template <AsyncSignalSafeCallable<const SignalEvent&> F>
    static auto RegisterHandler(Signal sig, F&& handler) -> uint32_t {
        using D = std::decay_t<F>;
        if constexpr (std::is_empty_v<D>) {
            static_cast<void>(handler);
            return RegisterHandlerInternal(sig, [](const SignalEvent& ev) noexcept { D {}(ev); });
        } else {
            static_assert(std::is_trivially_copyable_v<D>, "stateful signal handlers must be trivially copyable");
            static_assert(sizeof(D) <= kMaxHandlerState, "stateful signal handler exceeds slot storage");
            D copy = std::forward<F>(handler);
            return RegisterStatefulInternal(
                sig,
                [](const SignalEvent& ev, void* state) noexcept { (*static_cast<D*>(state))(ev); },
                &copy, sizeof(D)
            );
        }
    }

    static void Unregister(uint32_t id) noexcept;

    /// Invoke every handler registered for ev.signal. Safe to call from the
    /// OS handler and from tests.
    static void Dispatch(const SignalEvent& ev) noexcept;

  private:
    static auto RegisterHandlerInternal(Signal sig, Handler handler) -> uint32_t;
    static auto RegisterStatefulInternal(Signal sig, StatefulHandler handler, const void* state, uint32_t size) -> uint32_t;
};

} // namespace ZHLN
