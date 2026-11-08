// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/COM/Reflect.hpp
//
// The generated half of the protocol. There is no interface map, no switch
// statement, no GUID table and no macro: the implementation declares its
// interfaces once as template arguments and reflection walks the base graph to
// synthesise QueryRaw's dispatch. No std::meta token leaves Reflection/.
//
// The interfaces are aggregated by Implements itself. That is what makes a
// plain `: IObject` inheritance chain work without virtual inheritance: the
// three IObject virtuals are declared in a class derived from *every* interface
// branch, so one declaration is the final overrider for all of them.

#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>

#include <atomic>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::COM {

// Lets the reflected base walk distinguish the implementation helper from the
// interfaces it aggregates.
struct ImplementationTag {};

template <typename T>
concept ComInterface = std::is_base_of_v<IObject, T> &&
                       !std::is_same_v<T, IObject> &&
                       !std::is_base_of_v<ImplementationTag, T>;

// Reflect::ForEachBase expands direct bases into typed calls. Implements holds
// the interfaces one level below Derived, and interface inheritance can put
// more below those, so the walk recurses through the whole base graph.
template <typename T, typename F>
constexpr void ForEachInterfaceBase(F&& f) {
    Reflect::ForEachBase<T>([&]<typename Base>() -> auto {
        if constexpr (ComInterface<Base>) {
            f.template operator()<Base>();
        }
        if constexpr (!std::is_same_v<Base, IObject> && std::is_class_v<Base>) {
            ForEachInterfaceBase<Base>(f);
        }
    });
}

// All ABI policy lives here, in the COM domain. Reflection supplies only
// MethodSignature<Return, Noexcept, Parameters...> and a way to enumerate
// them.
//
// A returned object is either nothing, an address, or a value the caller
// copies out of a register or a hidden-pointer slot. A reference return is
// rejected: the boundary cannot express how long the referent stays alive,
// which is exactly what refcounting a pointer is for.
template <typename Return>
[[nodiscard]] consteval bool ComReturnCompliant() noexcept {
    if constexpr (std::is_void_v<Return> || std::is_pointer_v<Return>) {
        return true;
    } else {
        return std::is_standard_layout_v<Return> && std::is_trivially_copyable_v<Return>;
    }
}

// A parameter lives only for the duration of the call, so a reference is
// just an address the callee reads and carries no lifetime question. It is
// therefore audited as the type referred to, which keeps `const Transform&`
// usable while still rejecting `const std::string&` for the real reason:
// std::string is not trivially copyable, so its layout and its allocator
// would have to match on both sides.
template <typename Parameter>
[[nodiscard]] consteval bool ComParameterCompliant() noexcept {
    using Bare = std::remove_cvref_t<Parameter>;
    if constexpr (std::is_pointer_v<Bare>) {
        return true;
    } else {
        return std::is_standard_layout_v<Bare> && std::is_trivially_copyable_v<Bare>;
    }
}

template <typename ParameterPack>
struct ComParameterAudit;

template <typename... Parameters>
struct ComParameterAudit<std::tuple<Parameters...>> {
    static constexpr bool kCompliant = (... && ComParameterCompliant<Parameters>());
};

// A method that takes std::string by value is noexcept and returns void, so
// auditing the return type alone would pass it. Parameters are half the
// signature and half the ABI.
template <typename Signature>
[[nodiscard]] consteval bool ComMethodCompliant() noexcept {
    if constexpr (!Signature::IsNoexcept) {
        return false;
    } else if constexpr (!ComReturnCompliant<typename Signature::ReturnType>()) {
        return false;
    } else {
        return ComParameterAudit<typename Signature::ParameterTypes>::kCompliant;
    }
}

// The layout half of the audit, usable on any type so a violating
// interface can be rejected at compile time without deriving from IObject.
template <typename Interface>
[[nodiscard]] consteval bool ComInterfaceLayoutCompliant() noexcept {
    if constexpr (std::has_virtual_destructor_v<Interface>) {
        return false;
    }

    bool compliant = true;
    Reflect::ForEachMethodSignature<Interface>([&]<typename Signature>() -> auto {
        if constexpr (!ComMethodCompliant<Signature>()) {
            compliant = false;
        }
    });
    return compliant;
}

template <typename Interface>
[[nodiscard]] consteval bool AuditInterfaceABI() noexcept {
    static_assert(std::is_base_of_v<IObject, Interface>, "A COM interface must derive from IObject.");
    static_assert(!std::has_virtual_destructor_v<Interface>, "A COM interface must not have a virtual destructor.");
    return ComInterfaceLayoutCompliant<Interface>();
}

template <typename Derived, typename... Interfaces>
class Implements : public ImplementationTag, public Interfaces... {
    static_assert(sizeof...(Interfaces) > 0, "Implements<Derived, ...> needs at least one interface.");
    static_assert((std::is_base_of_v<IObject, Interfaces> && ...), "Every Implements argument must be a COM interface.");
    static_assert((AuditInterfaceABI<Interfaces>() && ...), "An interface violates the COM ABI rules.");

public:
    uint32_t AddRef() noexcept override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t Release() noexcept override {
        const uint32_t count = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete static_cast<Derived*>(this);
        }
        return count;
    }

    void* QueryRaw(InterfaceID id) noexcept override {
        void* result = nullptr;
        ForEachInterfaceBase<Derived>([&]<typename Base>() -> auto {
            static_assert(AuditInterfaceABI<Base>(), "A reflected base interface violates the COM ABI rules.");
            if (!result && id == InterfaceIdOf<Base>()) {
                result = static_cast<Base*>(static_cast<Derived*>(this));
            }
        });

        // With non-virtual inheritance there is one IObject subobject per
        // interface branch. Any of them dispatches to the same three overrides;
        // pick the first declared interface so the answer is deterministic.
        if (!result && id == InterfaceIdOf<IObject>()) {
            using FirstInterface = std::tuple_element_t<0, std::tuple<Interfaces...>>;
            result = static_cast<IObject*>(static_cast<FirstInterface*>(this));
        }

        if (result != nullptr) {
            AddRef();
        }
        return result;
    }

protected:
    ~Implements() = default;

private:
    std::atomic<uint32_t> m_refCount {1};
};

} // namespace ZHLN::COM
