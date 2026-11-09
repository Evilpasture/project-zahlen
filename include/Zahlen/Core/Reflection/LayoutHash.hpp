// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/Reflection/LayoutHash.hpp
//
// A structural fingerprint of a component's layout, computed independently on
// each side of the boundary and compared at the call.
//
// Size and alignment of the whole struct cannot carry this check: any two
// sixteen-byte PODs agree on both. If a consumer is built against a header in
// which a component gained a field, lost one, or had two differently sized
// ones reordered, that check still passes and the engine then writes through
// a wrongly shaped view of the caller's bytes. Folding in each member's
// offset, size and alignment turns that silent corruption into a refused
// call.
//
// What this deliberately does NOT fold in is member *type names*. A consumer
// is expected to restate a component's layout from its own declarations --
// that is the point of a byte-copy boundary, and requiring the engine's own
// type spellings would mean requiring the engine's headers, which is exactly
// what the COM layer exists to avoid. So a consumer's `alignas(16) float[16]`
// matches the engine's `ZHLN::Mat44`, as it should: the bytes are the same.
//
// The cost is that this cannot distinguish a float from an int32 of the same
// size at the same offset. That is an accepted limit, not an oversight: it
// catches every structural drift -- added, removed, resized or shifted
// members -- and trades away only same-shape retyping, which no amount of
// layout inspection could catch without naming types.

#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN::Reflect {

namespace LayoutHashDetail {

inline constexpr uint64_t kFnvBasis = 1469598103934665603ULL;
inline constexpr uint64_t kFnvPrime = 1099511628211ULL;

constexpr auto MixBytes(uint64_t hash, std::string_view text) -> uint64_t {
    for (const char character: text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= kFnvPrime;
    }
    return hash;
}

constexpr auto MixValue(uint64_t hash, uint64_t value) -> uint64_t {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

// A member template is ill-formed in a local class, so the visitor lives
// here rather than inside LayoutHash.
struct Fold {
    uint64_t* hash;

    template <typename FieldType>
    constexpr void operator()(std::string_view /*name*/, std::size_t offset) const {
        *hash = MixValue(*hash, offset);
        *hash = MixValue(*hash, sizeof(FieldType));
        *hash = MixValue(*hash, alignof(FieldType));
    }
};

} // namespace LayoutHashDetail

// Never zero for a real type: the value is seeded with the type's own size and
// alignment before any member is folded in, so a component with no members
// still produces a distinguishing value.
template <typename T>
consteval auto LayoutHash() -> uint64_t {
    uint64_t hash = LayoutHashDetail::kFnvBasis;
    hash          = LayoutHashDetail::MixValue(hash, sizeof(T));
    hash          = LayoutHashDetail::MixValue(hash, alignof(T));

    // Splicing the member's type back to a real type is what makes sizeof and
    // alignof available here; the raw reflection of it stays in this header's
    // neighbours, where the layering rule keeps it.
    ForEachFieldInfo<T>(LayoutHashDetail::Fold {&hash});
    return hash;
}

} // namespace ZHLN::Reflect
