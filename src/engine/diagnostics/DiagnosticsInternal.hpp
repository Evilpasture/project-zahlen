// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Diagnostics.hpp>
#include <Zahlen/Log.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN::Diagnostics {

void WriteToChannel(LogTarget channel, std::string_view msg) noexcept;

inline void WriteErr(std::string_view msg) noexcept {
    WriteToChannel(LogTarget::StdErr, msg);
}

auto SafeRead(std::span<const std::byte> src, std::span<std::byte> dest) noexcept -> bool;

} // namespace ZHLN::Diagnostics

namespace ZHLN {

// Engine-internal (formerly declared in Zahlen/Log.hpp, which is Layer 0 and
// carries no engine-only diagnostics): the raw backtrace formatter used by
// InternalPanic and the GPU fault dumps. The public capture/format entry
// points stay in Zahlen/Core/Diagnostics.hpp.
[[nodiscard]] auto GetPoorMansStacktrace() -> std::string;

} // namespace ZHLN
