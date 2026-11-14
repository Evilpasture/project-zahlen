// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/Reflection/LayoutHash.hpp
//
// A structural fingerprint of a value's byte layout, computed independently on
// each side of the boundary and compared at the call.
//
// Size and alignment of the whole struct cannot carry this check: any two
// sixteen-byte PODs agree on both, and nested sub-aggregates can reorder
// fields or consume existing padding without changing the outer size. Folding
// in every leaf scalar's absolute byte offset, size and alignment recursively
// through standard-layout subobjects and bounded arrays turns silent layout
// drift at any nesting depth into a refused call.
//
// What this deliberately does NOT fold in is member or wrapper *type names*.
// A consumer is expected to restate a component's layout from its own
// declarations -- that is the point of a byte-copy boundary, and requiring the
// engine's own type spellings would mean requiring the engine's headers.
// Flattening standard-layout wrappers and bounded arrays to their scalar leaves
// means a consumer's `alignas(16) float[16]` matches `ZHLN::Mat4x4`, and
// `float[3]` matches `ZHLN::FVec3`, while any structural drift inside nested
// settings or component aggregates changes the fingerprint.

#include <Zahlen/Core/Reflection/Core.hpp>

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

#if ZHLN_REFLECTION_AVAILABLE

template <typename U>
consteval void FoldType(uint64_t& hash, std::size_t baseOffset) {
    using Bare = std::remove_cv_t<U>;
    if constexpr (std::is_bounded_array_v<Bare>) {
        using Element               = std::remove_extent_t<Bare>;
        constexpr std::size_t count = std::extent_v<Bare>;
        for (std::size_t index = 0; index < count; ++index) {
            FoldType<Element>(hash, baseOffset + (index * sizeof(Element)));
        }
    } else if constexpr (std::is_class_v<Bare> && std::is_standard_layout_v<Bare>) {
        constexpr auto members =
            std::define_static_array(std::meta::nonstatic_data_members_of(^^Bare, std::meta::access_context::unchecked()));
        if constexpr (members.empty()) {
            hash = MixValue(hash, baseOffset);
            hash = MixValue(hash, sizeof(Bare));
            hash = MixValue(hash, alignof(Bare));
        } else {
            [:Expand(members):] >> [&]<auto member>() -> void {
                constexpr std::size_t memberOffset = std::meta::offset_of(member).bytes;
                using MemberType                   = typename[:std::meta::type_of(member):];
                FoldType<MemberType>(hash, baseOffset + memberOffset);
            };
        }
    } else {
        hash = MixValue(hash, baseOffset);
        hash = MixValue(hash, sizeof(Bare));
        hash = MixValue(hash, alignof(Bare));
    }
}

#endif

} // namespace LayoutHashDetail

// Never zero for a real type: the value is seeded with the type's own size and
// alignment before any leaf is folded in, so an empty tag struct still produces
// a distinguishing value.
template <typename T>
consteval auto LayoutHash() -> uint64_t {
    using Bare    = std::remove_cvref_t<T>;
    uint64_t hash = LayoutHashDetail::kFnvBasis;
    hash          = LayoutHashDetail::MixValue(hash, sizeof(Bare));
    hash          = LayoutHashDetail::MixValue(hash, alignof(Bare));
    LayoutHashDetail::FoldType<Bare>(hash, 0);
    return hash;
}

} // namespace ZHLN::Reflect
