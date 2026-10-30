// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Array.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

struct ShapeKey {
    uint32_t type = 0;
    float    p1   = 0.0f;
    float    p2   = 0.0f;
    float    p3   = 0.0f;
    float    p4   = 0.0f;

    [[nodiscard]] constexpr auto operator==(const ShapeKey&) const noexcept -> bool = default;
};

struct ShapeEntry {
    ShapeKey       key {};
    JPH::ShapeRefC shape;
};

// Isolated shape deduplication cache. Jolt shapes are immutable and atomically
// ref-counted once constructed, so only cache lookups and cold-path insertions
// touch this private mutex; simulation and transform reads never contend here.
struct alignas(64) ShapeCache {
    mutable ZHLN::Mutex    mutex {};
    JPH::Array<ShapeEntry> entries;

    [[nodiscard]] auto GetOrCreate(ShapeType type, float p1, float p2, float p3, float p4) -> JPH::ShapeRefC;
    void               Clear() noexcept;
};

static_assert(std::is_standard_layout_v<ShapeKey>);

} // namespace ZHLN::Physics
