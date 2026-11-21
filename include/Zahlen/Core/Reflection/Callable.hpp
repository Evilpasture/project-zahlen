// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Entity.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

// Function-type decomposition for plain function types. The primary
// template covers cv/ref-qualified and otherwise unsupported forms.
template <typename FnType>
struct FunctionTypeTraits {
    static constexpr bool hasSignature = false;
    static constexpr bool isNoexcept  = false;
    static constexpr bool isCVariadic = false;
};

template <typename R, typename... Args>
struct FunctionTypeTraits<R(Args...)> {
    using ReturnType                 = R;
    using ParameterTypes             = std::tuple<Args...>;
    static constexpr bool hasSignature = true;
    static constexpr bool isNoexcept  = false;
    static constexpr bool isCVariadic = false;
};

template <typename R, typename... Args>
struct FunctionTypeTraits<R(Args...) noexcept> {
    using ReturnType                 = R;
    using ParameterTypes             = std::tuple<Args...>;
    static constexpr bool hasSignature = true;
    static constexpr bool isNoexcept  = true;
    static constexpr bool isCVariadic = false;
};

template <typename R, typename... Args>
struct FunctionTypeTraits<R(Args..., ...)> {
    using ReturnType                 = R;
    using ParameterTypes             = std::tuple<Args...>;
    static constexpr bool hasSignature = true;
    static constexpr bool isNoexcept  = false;
    static constexpr bool isCVariadic = true;
};

template <typename R, typename... Args>
struct FunctionTypeTraits<R(Args..., ...) noexcept> {
    using ReturnType                 = R;
    using ParameterTypes             = std::tuple<Args...>;
    static constexpr bool hasSignature = true;
    static constexpr bool isNoexcept  = true;
    static constexpr bool isCVariadic = true;
};

// Pure C++ language facts for a reflected free or static member function.
// Exposes plain booleans, names, dealiased signature types, an opaque
// EntityToken, and an enclosing-scope walk — no ABI or mangling policy.
//
// Note: C language linkage (`extern "C"`) is not reported yet because
// std::meta::has_c_language_linkage is not merged in Bloomberg clang-p2996
// (bloomberg/clang-p2996#340).
template <auto Fn>
struct FunctionTraits {
    using PointerType = std::remove_cvref_t<decltype(Fn)>;
    static_assert(
        std::is_pointer_v<PointerType> && std::is_function_v<std::remove_pointer_t<PointerType>>,
        "FunctionTraits<Fn> requires a pointer to a free or static member function."
    );
    // Compare via NTTP type identity rather than `Fn != nullptr`: under
    // `-fsanitize=address,undefined` with `-fvisibility=hidden`, GCC refuses
    // to constant-fold external function-pointer null checks (PR 71962).
    static_assert(
        !std::is_same_v<std::integral_constant<PointerType, Fn>, std::integral_constant<PointerType, nullptr>>,
        "FunctionTraits<Fn> cannot inspect a null function pointer."
    );

  private:
    static constexpr std::meta::info kEntity = std::meta::reflect_function(*Fn);
    using Signature                          = FunctionTypeTraits<std::remove_pointer_t<PointerType>>;

    static consteval auto FunctionIdentifier() noexcept -> std::string_view {
        if constexpr (std::meta::has_identifier(kEntity)) {
            return std::meta::identifier_of(kEntity);
        } else {
            return {};
        }
    }

  public:
    using FunctionType   = std::remove_pointer_t<PointerType>;
    using ReturnType     = typename Signature::ReturnType;
    using ParameterTypes = typename Signature::ParameterTypes;

    static constexpr std::size_t      parameterCount           = std::tuple_size_v<ParameterTypes>;
    static constexpr bool             isNoexcept               = Signature::isNoexcept;
    static constexpr bool             isCVariadic              = Signature::isCVariadic;
    static constexpr bool             hasIdentifier            = std::meta::has_identifier(kEntity);
    static constexpr std::string_view name                     = FunctionIdentifier();
    static constexpr bool             hasExternalLinkage       = std::meta::has_external_linkage(kEntity);
    static constexpr bool             isTemplateSpecialization = std::meta::has_template_arguments(kEntity);
    static constexpr bool             isSpecialMember          = std::meta::is_special_member_function(kEntity);
    static constexpr bool             isOperator               = std::meta::is_operator_function(kEntity);
    static constexpr bool             isConversion             = std::meta::is_conversion_function(kEntity);
    static constexpr bool             isStaticMember           = std::meta::is_static_member(kEntity);
    static constexpr bool             isGlobalScope            = (std::meta::dealias(std::meta::parent_of(kEntity)) == ^^::);
    static constexpr bool             isFunctionLocal          = TemplatedDetail::IsEntityFunctionLocal(kEntity);
    static constexpr EntityToken      id                       = TemplatedDetail::MakeEntityToken<kEntity>();

    template <typename Visitor>
    static constexpr void ForEachEnclosingScope(Visitor&& visitor) {
        TemplatedDetail::WalkEnclosingScopes<kEntity>(visitor);
    }
};

template <auto Fn>
struct CallableInspector {
    using Callable = std::remove_cvref_t<decltype(Fn)>;
    static_assert(!std::is_member_function_pointer_v<Callable>, "Unbound member functions need a receiver; use a free/static function or a callable object");

  private:
    static consteval auto FunctionEntity() -> std::meta::info {
        if constexpr (std::is_pointer_v<Callable> && std::is_function_v<std::remove_pointer_t<Callable>>) {
            // Same NTTP-identity comparison as FunctionTraits, and for the same
            // GCC reason (PR 71962).
            static_assert(
                !std::is_same_v<std::integral_constant<Callable, Fn>, std::integral_constant<Callable, nullptr>>,
                "Cannot inspect a null function"
            );
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

#endif

} // namespace ZHLN::Reflect
