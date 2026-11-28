// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/TypeKey.hpp>
#include <cstdint>

namespace ZHLN::ECS {

enum class Access : uint8_t { Read, Write };

enum class AccessDomain : uint8_t {
    Component,
    Service,
    Event,
    AllComponents,
};

struct ResourceAccess {
    AccessDomain domain = AccessDomain::Component;
    Access       mode   = Access::Read;
    uint64_t     key    = 0; // 0 when domain == AccessDomain::AllComponents

    constexpr auto operator==(const ResourceAccess&) const noexcept -> bool = default;
};

template <typename T>
[[nodiscard]] consteval auto Read() noexcept -> ResourceAccess {
    return {AccessDomain::Component, Access::Read, Reflect::TypeKey<T>()};
}

template <typename T>
[[nodiscard]] consteval auto Write() noexcept -> ResourceAccess {
    return {AccessDomain::Component, Access::Write, Reflect::TypeKey<T>()};
}

template <typename T>
[[nodiscard]] consteval auto ReadService() noexcept -> ResourceAccess {
    return {AccessDomain::Service, Access::Read, Reflect::TypeKey<T>()};
}

template <typename T>
[[nodiscard]] consteval auto WriteService() noexcept -> ResourceAccess {
    return {AccessDomain::Service, Access::Write, Reflect::TypeKey<T>()};
}

template <typename T>
[[nodiscard]] consteval auto ReadEvent() noexcept -> ResourceAccess {
    return {AccessDomain::Event, Access::Read, Reflect::TypeKey<T>()};
}

template <typename T>
[[nodiscard]] consteval auto WriteEvent() noexcept -> ResourceAccess {
    return {AccessDomain::Event, Access::Write, Reflect::TypeKey<T>()};
}

// Immediate structural capabilities (ExclusiveQuery) can invalidate any
// component view and serialize against Component/AllComponents accesses,
// without serializing against pure Service or Event systems.
[[nodiscard]] consteval auto AllComponentsWrite() noexcept -> ResourceAccess {
    return {AccessDomain::AllComponents, Access::Write, 0};
}

[[nodiscard]] constexpr auto AccessesConflict(const ResourceAccess& a, const ResourceAccess& b) noexcept -> bool {
    if (a.mode != Access::Write && b.mode != Access::Write) {
        return false;
    }
    if (a.domain == b.domain && (a.domain == AccessDomain::AllComponents || a.key == b.key)) {
        return true;
    }
    if (a.domain == AccessDomain::AllComponents && b.domain == AccessDomain::Component) {
        return true;
    }
    if (b.domain == AccessDomain::AllComponents && a.domain == AccessDomain::Component) {
        return true;
    }
    return false;
}

} // namespace ZHLN::ECS
