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

// LogSeverity and LogContext live in Core (Zahlen/Log.hpp) as
// ZHLN::LogSeverity / ZHLN::LogContext; these aliases keep the COM
// spelling working for existing declarations.
using LogSeverity = ZHLN::LogSeverity;
using LogContext  = ZHLN::LogContext;

// Chain node carrying a caller-owned logger into CreateEngineRaw /
// CreateLogicWorldRaw / CreateAppLayerRaw. Null means "create a default hub".
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
