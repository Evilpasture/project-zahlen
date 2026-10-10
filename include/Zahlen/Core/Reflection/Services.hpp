// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <cstddef>
#include <type_traits>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

namespace TemplatedDetail {

// Whether Bundle has a data member whose type (after removing cv/ref) is T.
// This is what makes a service *provided* rather than merely looked up: the
// bundle's member list is the whole answer, evaluated while compiling.
template <typename T, typename Bundle>
consteval auto HasMemberOfType() -> bool {
    bool found = false;
    [:Expand(NonStaticDataMembers<Bundle>()):] >> [&]<auto member>() -> auto {
        using MemberType = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
        if constexpr (std::is_same_v<MemberType, T>) {
            found = true;
        }
    };
    return found;
}

// The same list, checked for two members of one type. Two members of one type
// make by-type lookup ambiguous in a way the compiler cannot see (the second
// would simply never be found), so a bundle is required to be unambiguous and
// says so at the point the bundle is defined.
template <typename Bundle>
consteval auto BundleTypesAreUnique() -> bool {
    constexpr auto   members = NonStaticDataMembers<Bundle>();
    constexpr size_t count   = members.size();
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (std::meta::type_of(members[i]) == std::meta::type_of(members[j])) {
                return false;
            }
        }
    }
    return true;
}

} // namespace TemplatedDetail

// Whether the bundle provides T. Use as a constraint; the consteval body means
// it is answered by the member list rather than by a runtime lookup.
template <typename T, typename Bundle>
consteval auto BundleProvides() -> bool {
    return TemplatedDetail::HasMemberOfType<T, Bundle>();
}

// The T inside the bundle, or nullptr when the bundle does not provide one.
// Address-of-member: no table, no id, no indirection the optimizer has to see
// through.
template <typename T, typename Bundle>
auto FindService(Bundle& bundle) noexcept -> T* {
    T* found = nullptr;
    [:Expand(TemplatedDetail::NonStaticDataMembers<Bundle>()):] >> [&]<auto member>() -> auto {
        using MemberType = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
        if constexpr (std::is_same_v<MemberType, T>) {
            found = std::addressof(bundle.[:member:]);
        }
    };
    return found;
}

#else

namespace TemplatedDetail {

template <typename Bundle>
consteval auto BundleTypesAreUnique() -> bool {
    return true;
}

} // namespace TemplatedDetail

// Without reflection the member list cannot be read, so no bundle can be
// searched. The stub answers "yes" to every query so that stub-mode builds
// still compile -- the nullptr FindService returns is never dereferenced there
// because stub mode is a parse check, not a run.
template <typename T, typename Bundle>
consteval auto BundleProvides() -> bool {
    return true;
}

template <typename T, typename Bundle>
auto FindService(Bundle&) noexcept -> T* {
    return nullptr;
}

#endif

} // namespace ZHLN::Reflect
