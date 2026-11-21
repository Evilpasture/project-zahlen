// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <string_view>
#include <type_traits>
namespace ZHLN::Scene { namespace {
template <typename Dst, typename Src>
void AssignConverted(Dst& dst, const Src& src) {
    using D = std::remove_cvref_t<Dst>;
    using S = std::remove_cvref_t<Src>;

    if constexpr (std::is_same_v<D, S>) {
        dst = src;
    } else if constexpr (std::is_same_v<D, bool> && std::is_arithmetic_v<S>) {
        dst = src != 0;
        // The description and the settings component spell their vectors with the
        // same containers now, so the only pair left with a shape difference is the
        // sky: the component carries a fourth lane, the description does not. Sky
        // alpha is not part of the description and is restored as one when a scene
        // is instantiated.
    } else if constexpr (std::is_same_v<D, ZHLN::FVec3> && std::is_same_v<S, ZHLN::FVec4>) {
        dst = {src.x, src.y, src.z};
    } else if constexpr (std::is_same_v<D, ZHLN::FVec4> && std::is_same_v<S, ZHLN::FVec3>) {
        dst = {src.x, src.y, src.z, 1.0f};
    } else if constexpr (std::is_arithmetic_v<D> && std::is_arithmetic_v<S>) {
        dst = static_cast<D>(src);
    }
    // No final branch, deliberately. VisitFieldByName filters by field *name* with
    // a run-time std::string_view comparison (Reflection/Structs.hpp), so this
    // function is instantiated for every (destination type, source type) pair in
    // the two structs -- the cartesian product -- and only the matching name ever
    // calls it. A pair with no rule is therefore expected: it belongs to two
    // fields that were never paired, and doing nothing is the correct answer.
    // Do not turn this into a static_assert: it fires on all of those pairs.
    //
    // What guards the rules instead: SharesEveryField below pins the name set, and
    // the pairs that do match are same-type except the sky, which the two
    // FVec3/FVec4 rules above cover in both directions.
}

template <typename Dst, typename Src>
void CopySharedFields(Dst& dst, const Src& src) {
    ZHLN::Reflect::ForEachFieldWithName(dst, [&](std::string_view name, auto& dstField) -> void {
        ZHLN::Reflect::VisitFieldByName(src, name, [&](const auto& srcField) -> void { AssignConverted(dstField, srcField); });
    });
}

template <typename Dst, typename Src>
consteval auto SharesEveryField() -> bool {
    for (const std::string_view name: ZHLN::Reflect::FieldNames<Dst>()) {
        if (!ZHLN::Reflect::HasField<Src>(name)) {
            return false;
        }
    }
    return true;
}

}} // namespace ZHLN::Scene
