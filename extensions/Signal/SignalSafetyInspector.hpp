// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Signal/SignalSafe.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <concepts>
#include <type_traits>

namespace ZHLN {

template <typename F, typename... Args>
concept AsyncSignalSafeCallable = std::is_nothrow_invocable_v<std::remove_cvref_t<F>, Args...>
                                  && Reflect::TypeHasAnnotation<SignalSafe, std::remove_cvref_t<F>>();

} // namespace ZHLN
