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

#if !ZHLN_REFLECTION_AVAILABLE
namespace TemplatedDetail {
// The project normally requires P2996/P3096. This path is for the explicit
// ZHLN_ALLOW_REFLECTION_STUBS mode (clangd and isolated non-reflection tests).
template <typename T>
struct FunctionParameters;

template <typename R, typename... Params>
struct FunctionParameters<R (*)(Params...)> {
    template <typename F>
    static void ForEach(F&& f) { (f.template operator()<Params>(), ...); }

    template <auto Fn, template <typename> class Resolver, typename Context>
    static void Invoke(Context& ctx) {
        auto callable = Fn;
        std::invoke(callable, Resolver<Params>::Resolve(ctx)...);
    }
};

template <typename R, typename... Params>
struct FunctionParameters<R (*)(Params...) noexcept>: FunctionParameters<R (*)(Params...)> {};

template <typename C, typename R, typename... Params>
struct FunctionParameters<R (C::*)(Params...) const>: FunctionParameters<R (*)(Params...)> {};

template <typename C, typename R, typename... Params>
struct FunctionParameters<R (C::*)(Params...)>: FunctionParameters<R (*)(Params...)> {};

template <typename C, typename R, typename... Params>
struct FunctionParameters<R (C::*)(Params...) const noexcept>: FunctionParameters<R (*)(Params...)> {};

template <typename C, typename R, typename... Params>
struct FunctionParameters<R (C::*)(Params...) noexcept>: FunctionParameters<R (*)(Params...)> {};

template <typename T, typename = void>
struct CallableParameters: FunctionParameters<T> {};

template <typename T>
struct CallableParameters<T, std::void_t<decltype(&T::operator())>>: FunctionParameters<decltype(&T::operator())> {};
} // namespace TemplatedDetail
#endif

// Reusable callable introspection: names, signature parameters, and invocation
// with caller-provided argument resolution. Nothing here depends on ECS.
// The NTTP denotes a free/static function or an invocable function object.
template <auto Fn>
struct CallableInspector {
    using Callable = std::remove_cvref_t<decltype(Fn)>;
    static_assert(!std::is_member_function_pointer_v<Callable>,
                  "Unbound member functions need a receiver; use a free/static function or a callable object");

#if ZHLN_REFLECTION_AVAILABLE
  private:
    static consteval auto FunctionEntity() -> std::meta::info {
        if constexpr (std::is_pointer_v<Callable> && std::is_function_v<std::remove_pointer_t<Callable>>) {
            static_assert(Fn != nullptr, "Cannot inspect a null function");
            return std::meta::reflect_function(*Fn);
        } else if constexpr (std::is_class_v<Callable> && requires { &Callable::operator(); }) {
            return ^^Callable::operator();
        } else {
            static_assert(std::is_function_v<Callable>, "Callable must be a free/static function or function object");
            return std::meta::info {};
        }
    }

    static constexpr auto fnEntity = FunctionEntity();

    static consteval auto ParameterCount() -> std::size_t { return std::meta::parameters_of(fnEntity).size(); }

    template <std::size_t I>
    static consteval auto ParameterTypeInfo() -> std::meta::info {
        return std::meta::type_of(std::meta::parameters_of(fnEntity)[I]);
    }

    template <std::size_t I>
    using ParameterType = typename[:ParameterTypeInfo<I>():];

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
        // Only the type aliases above touch reflections; f runs with C++ types.
        ForEachWithIndices(std::forward<F>(f), std::make_index_sequence<ParameterCount()>{});
    }

    static consteval auto Name() -> std::string_view {
        constexpr auto parent = std::meta::parent_of(fnEntity);
        if constexpr (std::meta::is_type(parent) && std::meta::has_identifier(parent)) {
            // An owning type makes a concise name for its member callable.
            return std::meta::identifier_of(parent);
        } else if constexpr (std::is_class_v<Callable>) {
            // A lambda has no identifier; append its location in NameCString.
            return std::meta::display_string_of(parent);
        } else if constexpr (std::meta::has_identifier(fnEntity)) {
            return std::meta::identifier_of(fnEntity);
        } else {
            return "AnonymousCallable";
        }
    }

    static auto NameCString() -> const char* {
        static const std::string name = [] {
            std::string result {Name()}; // identifier_of returns a view, not necessarily a C string
            if constexpr (HasUnnamedOwner()) {
                // Reflection values stay in consteval helpers; the runtime
                // string builder receives only an ordinary source_location.
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
        // A reflection (std::meta::info) is consteval-only in Clang/P2996.
        // Resolve every parameter type above, during template instantiation;
        // the runtime thunk must only mention ordinary C++ types and values.
        InvokeWithIndices<Resolver>(ctx, std::make_index_sequence<ParameterCount()>{});
    }

#else
  private:
    using Params = TemplatedDetail::CallableParameters<Callable>;
    static inline char uniqueTag; // fallback identity for same-signature lambdas

  public:
    template <typename F>
    static void ForEachParameter(F&& f) { Params::ForEach(std::forward<F>(f)); }

    static consteval auto Name() -> std::string_view {
        // Only for reflection stubs. Native reflection above obtains the
        // actual entity and its canonical identifier instead of parsing text.
        std::string_view pretty = __PRETTY_FUNCTION__;
        auto start = pretty.find("Fn = ");
        if (start == std::string_view::npos) return "AnonymousCallable";
        pretty.remove_prefix(start + sizeof("Fn = ") - 1);
        auto end = pretty.find_first_of(";]");
        auto name = pretty.substr(0, end);
        if (name.find("<lambda") != std::string_view::npos) return name; // NameCString adds identity
        if (name.starts_with('&')) name.remove_prefix(1);
        if (name.ends_with("()")) name.remove_suffix(2);
        if (name.ends_with("{}")) name.remove_suffix(2);
        auto last = name.rfind("::");
        if (last != std::string_view::npos) {
            auto suffix = name.substr(last + 2);
            if (suffix == "Update" || suffix == "GraphUpdate") {
                name = name.substr(0, last);
                last = name.rfind("::");
                return name.substr(last == std::string_view::npos ? 0 : last + 2);
            }
            return suffix;
        }
        return name;
    }

    static auto NameCString() -> const char* {
        static const std::string name = [] {
            std::string result {Name()};
            if constexpr (std::is_class_v<Callable>) {
                if (Name().find("<lambda") != std::string_view::npos) {
                    // GCC's pretty-function text omits the closure location.
                    // The tag is unique for each NTTP in stub builds.
                    result += "@" + std::to_string(reinterpret_cast<std::uintptr_t>(&uniqueTag));
                }
            }
            return result;
        }();
        return name.c_str();
    }

    template <template <typename> class Resolver, typename Context>
    static void Invoke(Context& ctx) { Params::template Invoke<Fn, Resolver>(ctx); }
#endif
};

} // namespace ZHLN::Reflect
