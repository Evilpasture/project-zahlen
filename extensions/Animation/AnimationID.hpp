// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <type_traits>
namespace ZHLN {
struct AnimationID {
    uint32_t                  id        = InvalidID;
    static constexpr uint32_t InvalidID = ~0u;

    [[nodiscard]] static constexpr auto Null() noexcept -> AnimationID {
        return {};
    }

    [[nodiscard]] constexpr auto IsValid() const noexcept -> bool {
        return id != InvalidID;
    }

    constexpr auto operator==(const AnimationID&) const noexcept -> bool = default;
};

static_assert(std::is_standard_layout_v<AnimationID> && std::is_trivially_copyable_v<AnimationID>);
} // namespace ZHLN
