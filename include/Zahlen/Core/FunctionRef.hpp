// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <functional> // std::function_ref and its feature-test macro, when available

#if !defined(__cpp_lib_function_ref) || __cpp_lib_function_ref < 202306L
#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>
#endif

namespace ZHLN {

// Borrowed callable; never owns its target. Keep the target alive through any
// synchronous dispatch-and-wait scope that passes this view to worker fibers.
#if defined(__cpp_lib_function_ref) && __cpp_lib_function_ref >= 202306L

template <typename Signature>
using FunctionRef = std::function_ref<Signature>;

#else

// Fallback for standard libraries without C++26 std::function_ref (notably
// the reflection toolchain). Unlike the standard type, this subset accepts
// only const-callable lvalue
// objects; it rejects temporaries at construction.
template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...) const> {
  public:
    template <typename F>
        requires(std::is_lvalue_reference_v<F> && std::is_object_v<std::remove_reference_t<F>> &&
                 !std::same_as<std::remove_cvref_t<F>, FunctionRef> &&
                 std::is_invocable_r_v<R, const std::remove_reference_t<F>&, Args...>)
    constexpr FunctionRef(F&& callable) noexcept:
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

#endif

}
