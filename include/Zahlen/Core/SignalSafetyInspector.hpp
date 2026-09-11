// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/SignalSafetyInspector.hpp
//
// Compile-time gate for callables that may run in a signal context. Built on
// Reflect::TypeHasAnnotation so this header never spells ^^ or <meta>.

#pragma once

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/SignalSafe.hpp>
#include <concepts>
#include <type_traits>

namespace ZHLN {

template <typename F, typename... Args>
concept AsyncSignalSafeCallable = std::is_nothrow_invocable_v<std::remove_cvref_t<F>, Args...> &&
                                  Reflect::TypeHasAnnotation<SignalSafe, std::remove_cvref_t<F>>();

} // namespace ZHLN
