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

/// How a resource context asks whether an owner still exists.
///
/// Same shape as Scene::MaterialLookup: a function pointer plus the userdata
/// it closes over, so Render/Audio/Physics never name ECS::Registry. Engine
/// systems pass Registry::AliveQuery(); tests can supply any predicate.
struct EntityAliveQuery {
    const void* userdata = nullptr;
    bool (*isAlive)(const void* userdata, Entity entity) noexcept = nullptr;

    [[nodiscard]] auto operator()(Entity entity) const noexcept -> bool {
        return isAlive != nullptr && isAlive(userdata, entity);
    }
};

static_assert((std::is_trivially_default_constructible_v<Entity> && std::is_trivially_copyable_v<Entity>) && sizeof(Entity) == 8);

} // namespace ZHLN
