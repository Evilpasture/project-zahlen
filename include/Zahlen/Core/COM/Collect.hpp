// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/COM/Collect.hpp
//
// The count-then-fill dance (call(nullptr, 0) -> allocate -> call(data, cap)
// -> resize to what was actually written) is written exactly once, here.
// Every COM method shaped `... (T* buffer, uint32_t capacity)` -- optionally
// returning Result<uint32_t> instead of uint32_t -- gets its owning
// std::vector<T> / std::string wrapper by naming the method:
//
//     auto windows = Collect<&IEngine::WindowsRaw>(*engine);
//     auto path    = Collect<&IAssetLibrary::PrefabVirtualPathRaw>(*assets, hash);
//     auto clip    = Collect<&IEngine::ClipboardTextRaw>(*engine); // std::expected<std::string, ErrorCode>
//
// Wire methods carry the codebase's `Raw` suffix (QueryRaw, EntitiesWithRaw,
// ...), so &Interface::MethodRaw names the vtable slot unambiguously and
// every wrapper is plain, type-checked pointer-to-member syntax -- no
// string lookup, no overload-resolution games.
//
// The element type, container type, error model and leading-argument pack
// are all deduced from the method pointer's type. Interfaces keep a 1-line
// delegate so call sites still read `assets->PrefabVirtualPath(hash)`.
//
// The fallible form returns std::expected, not COM::Result: Result<T>
// static-asserts a trivially copyable payload (it crosses the ABI), and a
// std::vector is not one. std::expected is the type Result already converts
// to in the caller's own translation unit.
//
// Never use this on a method whose fill call has side effects beyond
// writing the buffer (e.g. IAssetLibrary::Instantiate spawns the prefab
// even with a null buffer -- its wrapper stays hand-written).
//
// MethodSignature is a local trait rather than Reflect::FunctionTraits:
// FunctionTraits accepts only free/static function pointers, while every
// count-fill seam is a member function.

#include <Zahlen/Core/COM/Types.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::COM {

namespace CollectDetail {

template <typename Method>
struct MethodSignature;

template <typename Class, typename R, typename... Args>
struct MethodSignature<R (Class::*)(Args...)> {
    using ReturnType     = R;
    using ParameterTypes = std::tuple<Args...>;
};

template <typename Class, typename R, typename... Args>
struct MethodSignature<R (Class::*)(Args...) const> {
    using ReturnType     = R;
    using ParameterTypes = std::tuple<Args...>;
};

template <typename Class, typename R, typename... Args>
struct MethodSignature<R (Class::*)(Args...) noexcept> {
    using ReturnType     = R;
    using ParameterTypes = std::tuple<Args...>;
};

template <typename Class, typename R, typename... Args>
struct MethodSignature<R (Class::*)(Args...) const noexcept> {
    using ReturnType     = R;
    using ParameterTypes = std::tuple<Args...>;
};

} // namespace CollectDetail

template <auto Method>
struct CountFillTraits {
    using Signature = CollectDetail::MethodSignature<decltype(Method)>;
    using Params    = typename Signature::ParameterTypes;

    static constexpr size_t kParamCount = std::tuple_size_v<Params>;
    static_assert(kParamCount >= 2, "Count-fill method must have at least buffer and capacity parameters.");

    using LastArg   = std::tuple_element_t<kParamCount - 1, Params>;
    using BufferArg = std::tuple_element_t<kParamCount - 2, Params>;

    static_assert(std::is_integral_v<LastArg>, "Last parameter must be an integer capacity.");
    static_assert(std::is_pointer_v<BufferArg>, "Second-to-last parameter must be a pointer buffer.");

    using ElementType = std::remove_pointer_t<BufferArg>;
    using ValueType   = std::remove_cv_t<ElementType>;
    using RawReturn   = typename Signature::ReturnType;

    // Fallible iff the raw return is a COM Result (value + error + HasValue).
    static constexpr bool kIsFallible = requires(const RawReturn& r) {
        r.HasValue();
        r.value;
        r.error;
    };

    // std::string for char buffers, std::vector for everything else.
    using Container = std::conditional_t<std::is_same_v<ValueType, char>, std::string, std::vector<ValueType>>;

    using ReturnType = std::conditional_t<kIsFallible, std::expected<Container, ErrorCode>, Container>;
};

template <auto Method, typename Instance, typename... LeadingArgs>
[[nodiscard]] auto Collect(Instance&& inst, LeadingArgs&&... args) -> typename CountFillTraits<Method>::ReturnType {
    using Info      = CountFillTraits<Method>;
    using Container = typename Info::Container;

    constexpr bool kIsString = std::is_same_v<Container, std::string>;

    auto invoke = [&](typename Info::ElementType* ptr, uint32_t cap) {
        return (inst.*Method)(std::forward<LeadingArgs>(args)..., ptr, cap);
    };

    // Char buffers get one spare slot: NUL-terminated writers (e.g.
    // PrefabVirtualPath) need room for the terminator, exact-byte writers
    // (ClipboardText, SerializeBindings) simply never touch it, and the
    // resize to the reported count drops it either way.
    constexpr uint32_t kSpare = kIsString ? 1u : 0u;

    if constexpr (Info::kIsFallible) {
        const auto query = invoke(nullptr, 0);
        if (!query.HasValue()) {
            return std::unexpected(query.error);
        }
        const uint32_t count = query.value;
        if (count == 0) return Container {};

        Container  out(count + kSpare, typename Info::ValueType {});
        const auto filled = invoke(out.data(), count + kSpare);
        if (!filled.HasValue()) {
            return std::unexpected(filled.error);
        }
        out.resize(filled.value < count ? filled.value : count);
        return out;
    } else {
        const uint32_t count = static_cast<uint32_t>(invoke(nullptr, 0));
        if (count == 0) return Container {};

        Container      out(count + kSpare, typename Info::ValueType {});
        const uint32_t filled = static_cast<uint32_t>(invoke(out.data(), count + kSpare));
        out.resize(filled < count ? filled : count);
        return out;
    }
}


} // namespace ZHLN::COM
