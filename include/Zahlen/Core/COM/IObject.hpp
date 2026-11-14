// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/COM/IObject.hpp
//
// The protocol. Three virtuals are the only things that cross the shared
// library boundary; everything above them is a template on the caller's side
// and is compiled into the caller, never into the callee.
//
// An interface is a pure abstract class deriving *non-virtually* from IObject,
// with no virtual destructor, noexcept methods, and fixed-layout return types.
// COM::Reflect.hpp audits that contract at compile time.

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN::COM {

struct InterfaceID {
    uint64_t high = 0;
    uint64_t low  = 0;

    constexpr bool operator==(const InterfaceID&) const noexcept = default;
};

static_assert(std::is_standard_layout_v<InterfaceID> && std::is_trivially_copyable_v<InterfaceID>);

// Compile-time 128-bit hash of the canonical interface name. The name is
// reflected, so there is no GUID registry and no IDL to keep in sync; the
// values follow the documented canonical grammar, not compiler display text.
template <typename Interface>
[[nodiscard]] consteval InterfaceID InterfaceIdOf() noexcept {
    constexpr std::string_view name = Reflect::CanonicalTypeName<Interface>();
    return InterfaceID {
        .high = Hash64(name),
        .low  = Hash64(name.substr(0, name.size() / 2)),
    };
}

struct IObject {
    virtual uint32_t AddRef() noexcept = 0;
    virtual uint32_t Release() noexcept = 0;

    // The single virtual seam that crosses the shared library boundary. A
    // successful query has already added one reference.
    virtual void* QueryRaw(InterfaceID id) noexcept = 0;

    template <typename TargetInterface>
    [[nodiscard]] TargetInterface* Query() noexcept {
        return static_cast<TargetInterface*>(QueryRaw(InterfaceIdOf<TargetInterface>()));
    }

protected:
    // Deletion goes through Release(), never through an interface pointer.
    ~IObject() = default;
};

} // namespace ZHLN::COM
