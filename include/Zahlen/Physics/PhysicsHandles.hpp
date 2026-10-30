// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

// Physics slots are not ECS entities. The 64-bit packing is shared with Jolt
// user data, but the distinct type prevents accidental cross-service use.
struct BodyHandle {
    uint32_t index;
    uint32_t generation;

    [[nodiscard]] constexpr auto Pack() const noexcept -> uint64_t {
        return (static_cast<uint64_t>(generation) << 32) | index;
    }
    [[nodiscard]] static constexpr auto Unpack(uint64_t raw) noexcept -> BodyHandle {
        return {.index = static_cast<uint32_t>(raw), .generation = static_cast<uint32_t>(raw >> 32)};
    }
    [[nodiscard]] static constexpr auto Null() noexcept -> BodyHandle {
        return {.index = UINT32_MAX, .generation = UINT32_MAX};
    }
    constexpr auto operator==(const BodyHandle&) const noexcept -> bool = default;
};

// A ragdoll uses a separate generational pool from rigid bodies.
enum class RagdollHandle : uint64_t { Invalid = UINT64_MAX };

static_assert(sizeof(BodyHandle) == 8 && std::is_trivially_default_constructible_v<BodyHandle> && std::is_trivially_copyable_v<BodyHandle>);
static_assert(sizeof(RagdollHandle) == 8 && std::is_trivially_copyable_v<RagdollHandle>);

}
