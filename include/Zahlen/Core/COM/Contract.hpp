// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/ItaniumMangle.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace ZHLN::COM {

// Hash inline values recursively. Pointer fields have a wire representation
// and a declared type, but do not recursively own the pointed-to object.
template <typename T>
[[nodiscard]] consteval auto WireContractHash() -> uint64_t {
    using Bare       = std::remove_cvref_t<T>;
    std::size_t hash = Hash64(Reflect::CanonicalTypeName<T>());
    HashCombine(hash, static_cast<std::size_t>(std::is_const_v<std::remove_reference_t<T>>),
                      static_cast<std::size_t>(std::is_volatile_v<std::remove_reference_t<T>>),
                      static_cast<std::size_t>(std::is_lvalue_reference_v<T>),
                      static_cast<std::size_t>(std::is_rvalue_reference_v<T>));
    if constexpr (!std::is_void_v<Bare>) {
        HashCombine(hash, Reflect::LayoutHash<Bare>());
        if constexpr (std::is_enum_v<Bare>) {
            HashCombine(hash, WireContractHash<std::underlying_type_t<Bare>>());
        } else if constexpr (std::is_bounded_array_v<Bare>) {
            HashCombine(hash, WireContractHash<std::remove_extent_t<Bare>>());
        } else if constexpr (std::is_class_v<Bare> && std::is_standard_layout_v<Bare>) {
            Reflect::ForEachBase<Bare>([&]<typename Base>() consteval { HashCombine(hash, WireContractHash<Base>()); });
            Reflect::ForEachDeclaredFieldType<Bare>([&]<typename Field>(std::string_view name) consteval {
                HashCombine(hash, Hash64(name), WireContractHash<Field>());
            });
        }
    }
    return hash;
}

// Hash the declared base graph and virtual methods, not a guessed physical
// vtable layout. Pointers in virtual signatures are nominal; following them
// into other interfaces would make cycles and incidental includes affect ABI.
template <typename Interface>
[[nodiscard]] consteval auto InterfaceContractHash() -> uint64_t {
    static_assert(AuditInterfaceABI<Interface>());
    static_assert(!Reflect::HasVirtualBases<Interface>());
    std::size_t hash = Hash64(Reflect::CanonicalTypeName<Interface>());
    HashCombine(hash, sizeof(Interface), alignof(Interface));
    std::size_t bases = 0;
    Reflect::ForEachBase<Interface>([&]<typename Base>() consteval {
        ++bases;
        HashCombine(hash, InterfaceContractHash<Base>());
    });
    HashCombine(hash, bases);
    std::size_t methods = 0;
    Reflect::ForEachNamedVirtualMethodSignature<Interface>([&]<typename Signature, typename DeclaredType>(std::string_view name) consteval {
        ++methods;
        HashCombine(hash, Hash64(name), Signature::IsNoexcept,
                      WireContractHash<typename Signature::ReturnType>());
        [&]<typename... Args>(std::type_identity<std::tuple<Args...>>) consteval {
            using Return                  = typename Signature::ReturnType;
            constexpr bool     nt         = Signature::IsNoexcept;
            constexpr unsigned qualifiers = std::is_same_v<DeclaredType, Return(Args...) noexcept(nt)> * 1u +
                                                std::is_same_v<DeclaredType, Return(Args...) const noexcept(nt)> * 2u +
                                                std::is_same_v<DeclaredType, Return(Args...) volatile noexcept(nt)> * 3u +
                                                std::is_same_v<DeclaredType, Return(Args...) const volatile noexcept(nt)> * 4u +
                                                std::is_same_v<DeclaredType, Return(Args...) & noexcept(nt)> * 5u +
                                                std::is_same_v<DeclaredType, Return(Args...) const & noexcept(nt)> * 6u +
                                                std::is_same_v<DeclaredType, Return(Args...) volatile & noexcept(nt)> * 7u +
                                                std::is_same_v<DeclaredType, Return(Args...) const volatile & noexcept(nt)> * 8u + std::is_same_v <
                                            DeclaredType,
                               Return(Args...) && noexcept(nt) > *9u + std::is_same_v < DeclaredType,
                               Return(Args...) const && noexcept(nt) > *10u + std::is_same_v < DeclaredType,
                               Return(Args...) volatile && noexcept(nt) > *11u + std::is_same_v < DeclaredType,
                               Return(Args...) const volatile && noexcept(nt) > *12u;
            static_assert(qualifiers != 0, "Unsupported virtual method type in COM contract.");
            HashCombine(hash, qualifiers, sizeof...(Args));
            (HashCombine(hash, WireContractHash<Args>()), ...);
        }(std::type_identity<typename Signature::ParameterTypes> {});
    });
    HashCombine(hash, methods);
    return hash;
}

template <auto Fn>
[[nodiscard]] consteval auto EntryPointContract() -> uint64_t {
    static_assert(AuditFunctionABI<Fn>());
    using Traits     = Reflect::FunctionTraits<Fn>;
    std::size_t hash = Hash64(MangleFunction<Fn>());
    // Find interface pointers inside inline wire records (e.g. Result<T*>).
    // Do not chase arbitrary pointers in those records, including self-links.
    const auto foldInterfaces = [&]<typename T>(auto&& self) consteval -> void {
        using Bare = std::remove_cvref_t<T>;
        if constexpr (std::is_pointer_v<Bare>) {
            using Target = std::remove_cv_t<std::remove_pointer_t<Bare>>;
            if constexpr (std::is_class_v<Target>) {
                static_assert(requires { sizeof(Target); }, "COM entry point contracts require complete class pointees.");
                if constexpr (requires { sizeof(Target); }) {
                    if constexpr (std::is_base_of_v<IObject, Target>) {
                        HashCombine(hash, InterfaceContractHash<Target>());
                    }
                }
            }
        } else if constexpr (std::is_bounded_array_v<Bare>) {
            self.template operator()<std::remove_extent_t<Bare>>(self);
        } else if constexpr (std::is_class_v<Bare> && std::is_standard_layout_v<Bare>) {
            Reflect::ForEachBase<Bare>([&]<typename Base>() consteval { self.template operator()<Base>(self); });
            Reflect::ForEachDeclaredFieldType<Bare>([&]<typename Field>(std::string_view) consteval { self.template operator()<Field>(self); });
        }
    };
    using Return = typename Traits::ReturnType;
    HashCombine(hash, WireContractHash<Return>());
    foldInterfaces.template operator()<Return>(foldInterfaces);
    [&]<typename... Args>(std::type_identity<std::tuple<Args...>>) consteval {
        const auto foldParameter = [&]<typename Arg>() consteval {
            HashCombine(hash, WireContractHash<Arg>());
            foldInterfaces.template operator()<Arg>(foldInterfaces);
            using Bare = std::remove_cvref_t<Arg>;
            if constexpr (std::is_pointer_v<Bare>) {
                using Target = std::remove_cv_t<std::remove_pointer_t<Bare>>;
                if constexpr (std::is_object_v<Target> && requires { sizeof(Target); }) {
                    if constexpr (std::is_standard_layout_v<Target>) {
                        HashCombine(hash, WireContractHash<Target>());
                        foldInterfaces.template operator()<Target>(foldInterfaces);
                    }
                }
            }
        };
        (foldParameter.template operator()<Args>(), ...);
    }(std::type_identity<typename Traits::ParameterTypes> {});
    return hash;
}

// Define and export one Seal specialization per entry point. The function pointer
// template argument encodes the full signature, including the return type.
// The seal is looked up but never called.
template <typename FnPtr, uint64_t ContractHolder>
struct Contract {
    static void Seal() noexcept;
};

template <auto Fn>
using EntryPointWitness = Contract<decltype(Fn), EntryPointContract<Fn>()>;

template <auto Fn, typename Library>
[[nodiscard]] auto ResolveVerifiedSymbol(const Library& library) noexcept -> decltype(Fn) {
    if (ResolveSymbol<&EntryPointWitness<Fn>::Seal>(library) == nullptr) {
        return nullptr;
    }
    return ResolveSymbol<Fn>(library);
}

} // namespace ZHLN::COM
