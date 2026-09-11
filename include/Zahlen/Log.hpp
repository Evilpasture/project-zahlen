// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CommandLine.hpp"
#include "Config.hpp"
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <cstdio>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace ZHLN {

extern void ASSERTION_FAILED_AT_COMPILE_TIME();

class Engine;
void SetupSignalHandler();
void CheckForCrashes(Engine* engine);

// GetCurrentFiberID() lives in Zahlen/Threading/Thread.hpp with the rest of the
// fiber API; include that header instead of declaring it here.
auto GetCustomLogFile(FILE* overrideFile = nullptr) -> FILE*;
auto GetPoorMansStacktrace() -> std::string;

enum class LogChannel : uint8_t { StdErr, StdOut, File };
void SetLogLevel(LogLevel level) noexcept;
auto GetLogLevel() noexcept -> LogLevel;

struct LogContext {
    std::string_view     fmt;
    std::source_location loc;

    template <typename T>
    consteval LogContext(const T& s, std::source_location l = std::source_location::current()): fmt(s), loc(l) {
    }

    constexpr LogContext(std::string_view s, std::source_location l = std::source_location::current()): fmt(s), loc(l) {
    }
};

void              InternalWriteLog(uint8_t channel, const char* file, uint32_t line, std::string_view message);
[[noreturn]] void InternalPanic(const char* file, uint32_t line, std::string_view message);

/**
 * @brief Modern C++23 Engine Logger with Fiber awareness and compile-time channel dispatch.
 * Restored to standard dynamic formatting for stable general-purpose runtime use.
 */
template <LogChannel Channel = LogChannel::StdErr, LogLevel Level = LogLevel::Moderate, typename... Args>
void Log(LogContext ctx, Args&&... args) {
    if (static_cast<uint8_t>(GetLogLevel()) < static_cast<uint8_t>(Level)) {
        return;
    }
    std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
    InternalWriteLog(static_cast<uint8_t>(Channel), ctx.loc.file_name(), ctx.loc.line(), formatted);
}

/*
 * @brief ONLY USE FOR EXTREMELY EXCEPTIONAL CASES.
 * Change return type to std::expected<void, Error> and return an error code.
 */
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

/*
 * @brief Runtime assertion.
 */
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

struct DumpOptions {
    size_t bytes_per_line = 16;
    bool   show_ascii     = true;
    bool   show_interpret = true;
};

// ANSI Color Helpers
namespace Color {
inline constexpr char Reset[]  = "\033[0m";
inline constexpr char Gray[]   = "\033[90m";
inline constexpr char Cyan[]   = "\033[36m";
inline constexpr char Yellow[] = "\033[33m";
inline constexpr char Green[]  = "\033[32m";
inline constexpr char Red[]    = "\033[31m";
} // namespace Color

void LogManual(std::string_view file, int line, std::string_view message, const char* color = "");

auto TraceStructCallback(const char* fmt, ...) -> int;
void TraceStructHeader(std::string_view name, std::string_view label, const char* file, uint32_t line);
void TraceStructFooter();

template <typename T>
inline void TraceStructInternal(const T& obj, std::string_view name, LogContext ctx) {
    TraceStructHeader(name, ctx.fmt, ctx.loc.file_name(), ctx.loc.line());

    auto dumpObject = [](const auto& val) -> auto {
        using Decayed = std::remove_cvref_t<decltype(val)>;
        if constexpr (Reflect::FieldCount<Decayed>() > 0) {
            Reflect::ForEachFieldWithName(val, [](std::string_view fieldName, const auto& fieldVal) -> auto {
                std::string debugVal = Reflect::ToDebugString(fieldVal);
                Println(stderr, "│   {}: {}", fieldName, debugVal);
            });
        } else {
            std::string debugVal = Reflect::ToDebugString(val);
            Println(stderr, "│   {}", debugVal);
        }
    };

    if constexpr (std::is_pointer_v<T>) {
        if (obj != nullptr) {
            dumpObject(*obj);
        } else {
            Println(stderr, "│   (null pointer)");
        }
    } else if constexpr (requires { obj.get(); }) {
        if (obj.get() != nullptr) {
            dumpObject(*obj.get());
        } else {
            Println(stderr, "│   (null pointer)");
        }
    } else {
        dumpObject(obj);
    }

    TraceStructFooter();
}

void MemoryDump(const void* ptr, size_t size, std::string_view label, LogContext ctx, DumpOptions opts = {});

template <typename T>
void SmartDumpInternal(const T& var, std::string_view name, LogContext ctx) {
    if constexpr (requires {
                      var.data();
                      var.size();
                  }) {
        MemoryDump(var.data(), var.size() * sizeof(var[0]), name, ctx);
    } else {
        MemoryDump(&var, sizeof(var), name, ctx);
    }
}

/**
 * @brief Dumps raw memory contents of a variable with automatic or custom label.
 * Uses C++26 reflection to infer the type name when label is omitted.
 *
 * Usage:
 *   ZHLN::Dump(myStruct);
 *   ZHLN::Dump(myStruct, "Custom Label");
 */
template <typename T>
void Dump(const T& var, std::string_view label = {}, std::source_location loc = std::source_location::current()) {
    std::string_view name = label.empty() ? Reflect::TypeName<T>() : label;
    LogContext       ctx(name, loc);
    SmartDumpInternal(var, name, ctx);
}

/**
 * @brief Reflects and prints structured fields of an object.
 * Uses C++26 reflection to infer the type name when label is omitted.
 *
 * Usage:
 *   ZHLN::Trace(myObject);
 *   ZHLN::Trace(myObject, "Custom Label");
 */
template <typename T>
void Trace(const T& var, std::string_view label = {}, std::source_location loc = std::source_location::current()) {
    std::string_view name = label.empty() ? Reflect::TypeName<T>() : label;
    LogContext       ctx(name, loc);
    TraceStructInternal(var, name, ctx);
}

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void;
auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine) noexcept -> bool;

} // namespace ZHLN
