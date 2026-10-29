// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <cstddef>
#include <span>
#include <string_view>

namespace ZHLN::Diagnostics {

// Signal-free low-level diagnostic hooks exposed for caller-owned crash/signal
// frontends (e.g. extensions/Signal). None of these touch OS signal tables or
// hold ambient crash state inside Core.
ZHLN_API void InitializeSymbolResolver() noexcept;
ZHLN_API auto CaptureStackTrace(std::span<char> out, int maxFrames = 128) noexcept -> size_t;
ZHLN_API void DumpFaultRegion(const void* faultAddress) noexcept;
ZHLN_API void WriteDiagnosticError(std::string_view msg) noexcept;
ZHLN_API void EmergencyRestoreTerminal() noexcept;

} // namespace ZHLN::Diagnostics
