// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Log.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN::Diagnostics {

void WriteToChannel(uint8_t channel, std::string_view msg) noexcept;

inline void WriteErr(std::string_view msg) noexcept {
    WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), msg);
}

auto SafeRead(std::span<const std::byte> src, std::span<std::byte> dest) noexcept -> bool;

void DumpFaultRegion(const void* faultAddress) noexcept;

auto CaptureStackTrace(std::span<char> out, int maxFrames) noexcept -> size_t;

void InitializeSymbolResolver() noexcept;

}
