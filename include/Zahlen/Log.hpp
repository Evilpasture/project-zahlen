// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection/EnumFormat.hpp>
#include <cstdint>
#include <cstdio>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN {

extern void ASSERTION_FAILED_AT_COMPILE_TIME();

auto GetCustomLogFile(FILE* overrideFile = nullptr) -> FILE*;
auto GetPoorMansStacktrace() -> std::string;

enum class LogChannel : uint8_t { StdErr, StdOut, File };

// Severity is orthogonal to LogLevel (verbosity): a Verbose warning is still
// a warning, it is just hidden unless --verbose is passed. Debug renders gray,
// Warning yellow and Error red; Info keeps the historical plain layout.
enum class LogSeverity : uint8_t { Debug, Info, Warning, Error };

using LogObserver = void (*)(LogSeverity severity, std::string_view message) noexcept;

void SetLogLevel(LogLevel level) noexcept;
auto GetLogLevel() noexcept -> LogLevel;

ZHLN_API void SetLogObserver(LogObserver observer) noexcept;
ZHLN_API auto GetLogObserver() noexcept -> LogObserver;

// Test-quiet log capture. While capturing, InternalWriteLog records every
// formatted line into a bounded buffer; when suppressOutput is set the lines
// are additionally withheld from their channel. The test runner captures
// per-test and prints the buffer only when the test fails, so a green run
// shows RUN/PASS/FAIL instead of engine chatter. Not nestable: a second
// BeginLogCapture clears the buffer and restarts. Panics bypass suppression
// (InternalPanic cancels the capture first), so a fatal always prints.
void BeginLogCapture(bool suppressOutput = true);
auto EndLogCapture() -> std::vector<std::string>;
void CancelLogCapture();

struct LogContext {
    std::string_view     fmt;
    std::source_location loc;

    template <typename T>
    consteval LogContext(const T& s, std::source_location l = std::source_location::current()): fmt(s), loc(l) {
    }

    constexpr LogContext(std::string_view s, std::source_location l = std::source_location::current()): fmt(s), loc(l) {
    }
};

void              InternalWriteLog(uint8_t channel, uint8_t severity, const char* file, uint32_t line, std::string_view message);
[[noreturn]] void InternalPanic(const char* file, uint32_t line, std::string_view message);

template <LogChannel Channel = LogChannel::StdErr, LogLevel Level = LogLevel::Moderate, LogSeverity Severity = LogSeverity::Info, typename... Args>
void Log(LogContext ctx, Args&&... args) {
    if (static_cast<uint8_t>(GetLogLevel()) < static_cast<uint8_t>(Level)) {
        if (const LogObserver observer = GetLogObserver(); observer != nullptr) {
            std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
            observer(Severity, formatted);
        }
        return;
    }
    std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
    InternalWriteLog(static_cast<uint8_t>(Channel), static_cast<uint8_t>(Severity), ctx.loc.file_name(), ctx.loc.line(), formatted);
}

// Severity shorthands. Prefer these over hand-written "ERROR: "/"WARNING: "
// prefixes so the tag is colorized consistently on every terminal.
template <LogChannel Channel = LogChannel::StdErr, LogLevel Level = LogLevel::Verbose, typename... Args>
void LogDebug(LogContext ctx, Args&&... args) {
    Log<Channel, Level, LogSeverity::Debug>(ctx, std::forward<Args>(args)...);
}

template <LogChannel Channel = LogChannel::StdErr, LogLevel Level = LogLevel::Moderate, typename... Args>
void LogInfo(LogContext ctx, Args&&... args) {
    Log<Channel, Level, LogSeverity::Info>(ctx, std::forward<Args>(args)...);
}

template <LogChannel Channel = LogChannel::StdErr, LogLevel Level = LogLevel::Moderate, typename... Args>
void LogWarning(LogContext ctx, Args&&... args) {
    Log<Channel, Level, LogSeverity::Warning>(ctx, std::forward<Args>(args)...);
}

template <LogChannel Channel = LogChannel::StdErr, LogLevel Level = LogLevel::Moderate, typename... Args>
void LogError(LogContext ctx, Args&&... args) {
    Log<Channel, Level, LogSeverity::Error>(ctx, std::forward<Args>(args)...);
}

template <typename... Args>
[[noreturn]] void Panic(LogContext ctx, Args&&... args) {
    std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
    InternalPanic(ctx.loc.file_name(), ctx.loc.line(), formatted);
}

template <typename... Args>
void PanicIf(bool condition, LogContext ctx, Args&&... args) {
    if (condition) {
        Panic(ctx, std::forward<Args>(args)...);
    }
}

template <typename... Args>
inline void Assert(bool condition, LogContext ctx, Args&&... args) {
    if consteval {
        if (!condition) {
            ASSERTION_FAILED_AT_COMPILE_TIME();
        }
    }

    if (!condition) {
        if constexpr (isDev) {
            std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
            InternalPanic(ctx.loc.file_name(), ctx.loc.line(), formatted);
        } else {
            [[assume(false)]];
        }
    }
}

inline void Assert(bool condition, std::source_location loc = std::source_location::current()) {
    if consteval {
        if (!condition) {
            ASSERTION_FAILED_AT_COMPILE_TIME();
        }
    }

    if (!condition) {
        if constexpr (isDev) {
            InternalPanic(loc.file_name(), loc.line(), "Assertion failed.");
        } else {
            [[assume(false)]];
        }
    }
}

void LogManual(std::string_view file, int line, std::string_view message, const char* color = "");

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void;
auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine) noexcept -> bool;

} // namespace ZHLN
