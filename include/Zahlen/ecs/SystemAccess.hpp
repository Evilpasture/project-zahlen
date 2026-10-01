// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/ecs/ECS.hpp>
#include <cstdint>
#include <limits>

namespace ZHLN::ECS {

enum class Access : uint8_t { Read, Write };

struct ComponentAccess {
    uint32_t familyId;
    Access   mode;
};

// An explicitly injected Registry& can access or structurally change *any*
// component. Serialise it against all declared component accesses rather than
// pretending that the graph can infer its effects from the function body.
inline constexpr uint32_t AllComponents = std::numeric_limits<uint32_t>::max();

template <typename T>
constexpr auto Read() noexcept -> ComponentAccess {
    return {ComponentFamily::GetTypeID<T>(), Access::Read};
}

template <typename T>
constexpr auto Write() noexcept -> ComponentAccess {
    return {ComponentFamily::GetTypeID<T>(), Access::Write};
}

} // namespace ZHLN::ECS
