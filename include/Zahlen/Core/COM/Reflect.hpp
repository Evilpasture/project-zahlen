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
concept Interface = std::is_base_of_v<IObject, T> &&
                       !std::is_same_v<T, IObject> &&
                       !std::is_base_of_v<ImplementationTag, T>;

// Reflect::ForEachBase expands direct bases into typed calls. Implements holds
// the interfaces one level below Derived, and interface inheritance can put
// more below those, so the walk recurses through the whole base graph.
template <typename T, typename F>
constexpr void ForEachInterfaceBase(F&& f) {
    Reflect::ForEachBase<T>([&]<typename Base>() -> auto {
        if constexpr (Interface<Base>) {
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
[[nodiscard]] consteval bool ReturnCompliant() noexcept {
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
[[nodiscard]] consteval bool ParameterCompliant() noexcept {
    using Bare = std::remove_cvref_t<Parameter>;
    if constexpr (std::is_pointer_v<Bare>) {
        return true;
    } else {
        return std::is_standard_layout_v<Bare> && std::is_trivially_copyable_v<Bare>;
    }
}

template <typename ParameterPack>
struct ParameterAudit;

template <typename... Parameters>
struct ParameterAudit<std::tuple<Parameters...>> {
    static constexpr bool kCompliant = (... && ParameterCompliant<Parameters>());
};

// A method that takes std::string by value is noexcept and returns void, so
// auditing the return type alone would pass it. Parameters are half the
// signature and half the ABI.
template <typename Signature>
[[nodiscard]] consteval bool MethodCompliant() noexcept {
    if constexpr (!Signature::IsNoexcept) {
        return false;
    } else if constexpr (!ReturnCompliant<typename Signature::ReturnType>()) {
        return false;
    } else {
        return ParameterAudit<typename Signature::ParameterTypes>::kCompliant;
    }
}

// The layout half of the audit, usable on any type so a violating
// interface can be rejected at compile time without deriving from IObject.
//
// Only the virtual members are audited, because only they are the wire. A
// virtual is a vtable slot: the library's code and the consumer's code must
// agree on its signature byte for byte, and neither recompiles the other.
// A non-virtual member of an interface is the opposite -- it is compiled in
// the consumer's own translation unit from this header, calls the vtable for
// anything it needs from the library, and its parameters and return value
// never leave that translation unit. Auditing it would forbid the consumer
// from using std::optional, std::vector or std::string in code the library
// never sees, which is a restriction the ABI does not ask for. Member
// templates were already skipped for exactly this reason; this makes the rule
// consistent instead of accidental.
//
// What this trusts rather than checks: a non-virtual member declared here must
// be *defined* here. A non-virtual member left to be defined inside the shared
// library would be a cross-boundary symbol that no longer passes under the
// audit's eye.
template <typename Iface>
[[nodiscard]] consteval bool InterfaceLayoutCompliant() noexcept {
    if constexpr (std::has_virtual_destructor_v<Iface>) {
        return false;
    }

    bool compliant = true;
    Reflect::ForEachVirtualMethodSignature<Iface>([&]<typename Signature>() -> auto {
        if constexpr (!MethodCompliant<Signature>()) {
            compliant = false;
        }
    });
    return compliant;
}

template <typename Iface>
[[nodiscard]] consteval bool AuditInterfaceABI() noexcept {
    static_assert(std::is_base_of_v<IObject, Iface>, "A COM interface must derive from IObject.");
    static_assert(!std::has_virtual_destructor_v<Iface>, "A COM interface must not have a virtual destructor.");
    return InterfaceLayoutCompliant<Iface>();
}

template <typename FnPtr>
struct FunctionTypeAudit {
    static constexpr bool kCompliant = false;
};

template <typename Return, typename... Parameters>
struct FunctionTypeAudit<Return (*)(Parameters...) noexcept> {
    static constexpr bool kCompliant =
        MethodCompliant<Reflect::MethodSignature<Return, true, Parameters...>>();
};

// Free/static COM entry-point audit (factories, plugin entry points). Applies
// the same noexcept + return + parameter wire rules as virtual interface slots;
// throwing, C-variadic, and non-function-pointer types evaluate to false.
template <typename FnPtr>
[[nodiscard]] consteval bool FunctionTypeCompliant() noexcept {
    return FunctionTypeAudit<std::remove_cv_t<FnPtr>>::kCompliant;
}

template <auto Fn>
    requires (std::is_pointer_v<decltype(Fn)> && std::is_function_v<std::remove_pointer_t<decltype(Fn)>>)
[[nodiscard]] consteval bool AuditFunctionABI() noexcept {
    using FnPtr = decltype(Fn);
    static_assert(
        !std::is_same_v<std::integral_constant<FnPtr, Fn>, std::integral_constant<FnPtr, nullptr>>,
        "AuditFunctionABI requires a non-null function pointer."
    );
    return FunctionTypeCompliant<FnPtr>();
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
