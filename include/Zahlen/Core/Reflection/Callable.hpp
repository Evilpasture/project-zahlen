// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/Core.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

template <auto Fn>
struct CallableInspector {
    using Callable = std::remove_cvref_t<decltype(Fn)>;
    static_assert(!std::is_member_function_pointer_v<Callable>, "Unbound member functions need a receiver; use a free/static function or a callable object");

  private:
    static consteval auto FunctionEntity() -> std::meta::info {
        if constexpr (std::is_pointer_v<Callable> && std::is_function_v<std::remove_pointer_t<Callable>>) {
#if !defined(__ASAN_ENABLED__) && !defined(__SANITIZE_ADDRESS__)
            static_assert(Fn != nullptr, "Cannot inspect a null function");
#endif
            return std::meta::reflect_function(*Fn);
        } else if constexpr (std::is_class_v<Callable> && requires { &Callable::operator(); }) {
            return ^^Callable::operator();
        } else {
            static_assert(std::is_function_v<Callable>, "Callable must be a free/static function or function object");
            return std::meta::info {};
        }
    }

    static constexpr auto fnEntity = FunctionEntity();

  public:
    static consteval auto ParameterCount() -> std::size_t {
        return std::meta::parameters_of(fnEntity).size();
    }

    template <std::size_t I>
    static consteval auto ParameterTypeInfo() -> std::meta::info {
        return std::meta::type_of(std::meta::parameters_of(fnEntity)[I]);
    }

    template <std::size_t I>
    using ParameterType = typename[:ParameterTypeInfo<I>():];

  private:
    template <typename F, std::size_t... Is>
    static void ForEachWithIndices(F&& f, std::index_sequence<Is...>) {
        (f.template operator()<ParameterType<Is>>(), ...);
    }

    template <template <typename> class Resolver, typename Context, std::size_t... Is>
    static void InvokeWithIndices(Context& ctx, std::index_sequence<Is...>) {
        auto callable = Fn;
        std::invoke(callable, Resolver<ParameterType<Is>>::Resolve(ctx)...);
    }

    static consteval auto HasUnnamedOwner() -> bool {
        if constexpr (std::is_class_v<Callable>) {
            return !std::meta::has_identifier(std::meta::parent_of(fnEntity));
        }
        return false;
    }

    static consteval auto UnnamedOwnerLocation() -> std::source_location {
        return std::meta::source_location_of(std::meta::parent_of(fnEntity));
    }

  public:
    template <typename F>
    static void ForEachParameter(F&& f) {
        ForEachWithIndices(std::forward<F>(f), std::make_index_sequence<ParameterCount()> {});
    }

    static consteval auto Name() -> std::string_view {
        constexpr auto parent = std::meta::parent_of(fnEntity);
        if constexpr (std::meta::is_type(parent) && std::meta::has_identifier(parent)) {
            return std::meta::identifier_of(parent);
        } else if constexpr (std::is_class_v<Callable>) {
            return std::meta::display_string_of(parent);
        } else if constexpr (std::meta::has_identifier(fnEntity)) {
            return std::meta::identifier_of(fnEntity);
        } else {
            return "AnonymousCallable";
        }
    }

    static auto NameCString() -> const char* {
        static const std::string name = [] {
            std::string result {Name()};
            if constexpr (HasUnnamedOwner()) {
                constexpr auto loc = UnnamedOwnerLocation();
                result += "@";
                result += loc.file_name();
                result += ":" + std::to_string(loc.line()) + ":" + std::to_string(loc.column());
            }
            return result;
        }();
        return name.c_str();
    }

    template <template <typename> class Resolver, typename Context>
    static void Invoke(Context& ctx) {
        InvokeWithIndices<Resolver>(ctx, std::make_index_sequence<ParameterCount()> {});
    }
};

#else // !ZHLN_REFLECTION_AVAILABLE (Stub for stock clangd)

template <auto Fn>
struct CallableInspector {
    using Callable = std::remove_cvref_t<decltype(Fn)>;

    static consteval auto ParameterCount() -> std::size_t {
        return 0;
    }

    template <std::size_t I>
    using ParameterType = void;

    template <typename F>
    static void ForEachParameter(F&&) noexcept {
    }

    static consteval auto Name() -> std::string_view {
        return "Callable";
    }
    static auto NameCString() -> const char* {
        return "Callable";
    }

    template <template <typename> class Resolver, typename Context>
    static void Invoke(Context&) noexcept {
    }
};

#endif

} // namespace ZHLN::Reflect
