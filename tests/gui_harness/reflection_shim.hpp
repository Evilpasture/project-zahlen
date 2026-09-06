// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Force-included (-include) into EVERY harness translation unit, before anything
// else, so that ZHLN::Components::InputStateComponent sees the same
// `std::bitset<Reflect::EnumCount<KeyCode>()>` size it sees in a real build.
//
// Why this is needed:
//   ZHLN::Reflect::EnumCount<E>() is spelled two ways in
//   include/Zahlen/Core/Reflection.hpp:
//     * with C++26 static reflection (P2996, the project's Linux toolchain):
//       `std::meta::enumerators_of(^^E).size()`  -> 72 for KeyCode
//     * without it (clang/zig, which is what the GPU-less harness uses):
//       `return 0`
//   A `std::bitset<0>` makes every InputStateComponent::SetKey() hit its
//   `key >= keys.size()` guard and silently drop the key, so no harness could
//   ever see a mouse button go down. That is a harness artifact, not engine
//   behaviour -- but it is invisible unless you look for it, so it is pinned
//   here and re-asserted on every build.

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Input.hpp>

// KeyCode's enumerator count. Verified against include/Zahlen/Input.hpp; the
// static_assert below makes the real compiler check it for us on any toolchain
// that has reflection, so this can never silently drift.
#define ZHLN_HARNESS_KEYCODE_ENUM_COUNT 72

#if defined(__cpp_impl_reflection) || (defined(__has_feature) && __has_feature(reflection))

static_assert(ZHLN::Reflect::EnumCount<ZHLN::KeyCode>() == ZHLN_HARNESS_KEYCODE_ENUM_COUNT,
              "KeyCode gained/lost enumerators. Update ZHLN_HARNESS_KEYCODE_ENUM_COUNT in "
              "tests/gui_harness/reflection_shim.hpp so the no-reflection harness build keeps "
              "the same InputStateComponent key bitset width as the real build.");

#else

namespace ZHLN::Reflect {

template <>
consteval std::size_t EnumCount<ZHLN::KeyCode>() {
    return ZHLN_HARNESS_KEYCODE_ENUM_COUNT;
}

} // namespace ZHLN::Reflect

#endif
