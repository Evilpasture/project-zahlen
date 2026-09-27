// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/CrashState.hpp>
#include <string_view>

namespace ZHLN::Diagnostics {

auto RegisterCrashObserver(CrashState& state, std::string_view name, CrashObserver observer, void* context) noexcept -> bool;

void ClearCrashObservers(CrashState& state) noexcept;

void WriteCrashOutput(std::string_view text) noexcept;

}
