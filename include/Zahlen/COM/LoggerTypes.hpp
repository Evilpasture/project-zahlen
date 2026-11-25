// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/LoggerTypes.hpp
//
// Fixed-layout logging primitives shared by ILogger, the chain nodes that
// carry a logger into the factories, and (via an alias in Zahlen/Log.hpp)
// the Panic/Assert traps.

#include <Zahlen/COM/ErrorTypes.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <cstdint>
#include <source_location>
#include <string_view>
#include <type_traits>

namespace ZHLN::COM {

struct ILogger;


// A format string plus where it was written, captured at the call site. The
// COM twin of ZHLN::LogContext, declared here rather than aliased so a COM
// consumer never includes Zahlen/Log.hpp to log one line.
struct LogContext {
    std::string_view     fmt {};
    std::source_location loc {};

    template <typename T>
        requires std::is_convertible_v<const T&, std::string_view>
    consteval LogContext(const T& s, std::source_location l = std::source_location::current()): fmt(s), loc(l) {
    }

    constexpr LogContext(std::string_view s, std::source_location l = std::source_location::current()): fmt(s), loc(l) {
    }
};

struct LoggerConfig {
    ILogger* logger = nullptr;
    LogLevel level  = LogLevel::Moderate;
};

struct ConsoleSinkConfig {
    bool colorize = true;
    bool useStdErr = true;
};

struct FileSinkConfig {
    StringView path {};
    bool       append = false;
};

static_assert(std::is_standard_layout_v<LoggerConfig> && std::is_trivially_copyable_v<LoggerConfig>);
static_assert(std::is_standard_layout_v<ConsoleSinkConfig> && std::is_trivially_copyable_v<ConsoleSinkConfig>);
static_assert(std::is_standard_layout_v<FileSinkConfig> && std::is_trivially_copyable_v<FileSinkConfig>);

} // namespace ZHLN::COM
