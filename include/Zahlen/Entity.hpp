// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <type_traits>

namespace ZHLN {

struct Entity {
    uint32_t index;
    uint32_t generation;

    [[nodiscard]] constexpr auto Pack() const noexcept -> uint64_t {
        return (static_cast<uint64_t>(generation) << 32) | index;
    }

    [[nodiscard]] static constexpr auto Unpack(uint64_t raw) noexcept -> Entity {
        return {.index = static_cast<uint32_t>(raw & 0xFFFFFFFF), .generation = static_cast<uint32_t>(raw >> 32)};
    }

    constexpr auto operator==(const Entity& other) const noexcept -> bool = default;

    [[nodiscard]] static constexpr auto Null() noexcept -> Entity {
        return {.index = 0xFFFFFFFF, .generation = 0xFFFFFFFF};
    }
};

static_assert((std::is_trivially_default_constructible_v<Entity> && std::is_trivially_copyable_v<Entity>) && sizeof(Entity) == 8);

// Sentinel value
inline constexpr Entity NullEntity = {.index = 0xFFFFFFFF, .generation = 0xFFFFFFFF};

} // namespace ZHLN
