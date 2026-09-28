// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ZHLN {

// A non-owning view of a const-callable lvalue object. The callable must
// outlive the view; rvalues cannot bind. Keep asynchronous uses inside a dispatch-and-wait
// scope so neither the callable nor the view can expire on a worker thread.
template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...) const> {
  public:
    template <typename F>
        requires(std::is_object_v<std::remove_reference_t<F>> && !std::same_as<std::remove_cvref_t<F>, FunctionRef> &&
                 std::is_invocable_r_v<R, const F&, Args...>)
    constexpr FunctionRef(F& callable) noexcept:
        object_(std::addressof(callable)),
        invoke_([](const void* object, Args... args) -> R {
            const auto& callable = *static_cast<const std::remove_cvref_t<F>*>(object);
            if constexpr (std::is_void_v<R>) {
                std::invoke(callable, std::forward<Args>(args)...);
            } else {
                return std::invoke(callable, std::forward<Args>(args)...);
            }
        }) {}

    constexpr auto operator()(Args... args) const -> R {
        return invoke_(object_, std::forward<Args>(args)...);
    }

  private:
    const void* object_;
    R (*invoke_)(const void*, Args...);
};

}
