// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Log.hpp
//
// The Layer-0 logging primitive: severity/domain/source values, the LogSink
// function-pointer type, the ambient sink entry point, and Panic/Assert.
//
// Logging in engine-owned code is dependency-injected through COM::ILogger
// (Zahlen/COM/ILogger.hpp): whoever creates an engine decides where its lines
// go. Leaf code that cannot hold a logger (a Vulkan helper, an audio backend,
// a task running on a worker thread) calls the ambient ZHLN::Log above, which
// writes through the one process-wide sink below. That sink is *not* an inline
// global in this header: it lives inside libzahlen_engine behind the exported
// functions declared at the bottom of this section, because every DSO would
// duplicate an inline copy under `local: *;` symbol hiding and TaskSystem
// worker threads -- plus fibers resuming on other workers -- would never see
// it. One engine per process installs it at creation; every thread in the
// process then reads the same published value.
//
// Panic/Assert are the deliberate exception to all of the above: when an
// invariant fails, the heap, the sinks, or the logger itself may already be
// broken, so they format into stack storage and write to fd 2 directly
// (src/engine/diagnostics/Diagnostics.cpp).

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <cstdint>
#include <cstdio>
#include <format>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <type_traits>
#include <functional>

namespace ZHLN {

extern void ASSERTION_FAILED_AT_COMPILE_TIME();

// Sink-side stream selector (a console/file target), unrelated to the channel a
// subsystem names in LogSource. Renamed from LogChannel when the open channel
// type below took that name.
enum class LogTarget : uint8_t { StdErr, StdOut, File };

// ---- Logging primitives (Core, Layer 0) -----------------------------------
// Severity, source and context live here in ZHLN::, not in ZHLN::COM: leaf code
// in pure libraries logs through these without naming a COM interface.
// COM::ILogger (Zahlen/COM/ILogger.hpp) declares its own wire-shaped copies
// (COM::LogSource over COM::StringView) and never includes this header.
//
// Layer 0 knows NO subsystem: there is no closed domain enum here. The channel
// a line belongs to is an optional open name (LogSource::channel), declared by
// whoever owns it, in its own header or file:
//     inline constexpr ZHLN::LogChannel kVkLog {"Vulkan"};
// so a new plugin, extension or test can add a channel with no change to Core.
enum class LogSeverity : uint8_t { Debug, Info, Warning, Error, Off };

// Where a line came from. The two views point into static storage: file and
// function out of std::source_location, channel out of a subsystem's own
// literal. Anything that must outlive the recording DSO (COM::ErrorRecord)
// copies them.
struct LogSource {
    std::string_view file {};

    std::string_view function {};
    //: Optional channel name -- "Vulkan", "DeviceLost", "MyPlugin". Empty when
    //: the call site names none. Matching on this is exact and structured, so
    //: no test or harness has to parse the message text.
    std::string_view channel {};
    uint32_t         line = 0;
};

// A format string plus where it was written. Implicitly constructible from a
// string literal at the call site, capturing source_location there.
struct LogContext {
    std::string_view     fmt;
    std::source_location loc;

    // Constrained to string-like types: an unconstrained template here would
    // make any pointer (say, an ILogger*) look convertible to a LogContext, and
    // the ambient/explicit-logger overloads would be ambiguous everywhere.
    template <typename T>
        requires std::is_convertible_v<const T&, std::string_view>
    consteval LogContext(const T& s, std::source_location l = std::source_location::current()): fmt(s), loc(l) {
    }

    constexpr LogContext(std::string_view s, std::source_location l = std::source_location::current()): fmt(s), loc(l) {
    }

    [[nodiscard]] auto Source() const noexcept -> LogSource {
        return LogSource {
            .file     = loc.file_name(),
            .function = loc.function_name(),
            .line     = static_cast<uint32_t>(loc.line()),
        };
    }
};

// ---- Ambient sink (engine-owned, DSO-single) -------------------------------
// The active sink lives inside libzahlen_engine behind these exported
// functions -- never as an inline global in a header, which every DSO would
// duplicate under `local: *;` symbol hiding and which TaskSystem worker
// threads (and fibers resuming on other workers) would never see. Engine
// creation installs it once; every thread in the process then reads the same
// published value.
// Where lines go, in priority order:
//   1. the ambient sink an engine installed at creation (its ILogger hub,
//      with whatever sinks the host configured -- console, file, capture);
//   2. the sink registered with SetLogSink, if any (test capture,
//      harness validation counters);
//   3. stderr, formatted like any other log line.
// The stderr default is deliberate: ZHLN::Log works in main(), in tools,
// in unit tests and in leaf libraries with zero initialization, like
// every normal logger. Hosts that want lines routed elsewhere install a
// sink; nobody has to install anything to avoid silence.
using LogFilterFn = bool (*)(LogSeverity severity, void* userData) noexcept;
using LogSinkFn   = void (*)(LogSeverity severity, LogSource source, std::string_view message, void* userData) noexcept;

// A sink is published, and read, as ONE value: the cheap enable-gate (which
// spares disabled severities the formatting cost) can never disagree with
// the writer it belongs to, and save/restore of the previous sink at an
// engine boundary carries the gate along with it.
using LogFlushFn = void (*)(void* userData) noexcept;

struct LogSink {
    LogFilterFn filter   = nullptr; // nullptr: no gate, SetLogLevel applies
    LogSinkFn   write    = nullptr; // nullptr: nothing installed
    LogFlushFn  flush    = nullptr; // optional; forwarded by a host that flushes
    void*       userData = nullptr;
};

static_assert(std::is_trivially_copyable_v<LogSink>, "LogSink is copied out of the engine as one indivisible value.");
static_assert(sizeof(LogSink) <= 4 * sizeof(void*), "LogSink is four words; keep it copyable by value, not lock-free-atomic.");

// Installation and the two read paths below are the only places that touch
// the published sink. Each ambient call takes exactly one snapshot of it: the
// gate and the writer that follow come from the same value, so a concurrent
// install can never split a line between a filter that accepted it and a
// writer that belongs to another sink. The snapshot is copied out under the
// engine's sink mutex; a sink is four pointers, so publishing it as a
// std::atomic would not be lock-free and would drag in libatomic.
ZHLN_API void SetLogSink(LogSink sink) noexcept;

inline void SetLogSink(const LogSinkFn write, void* const userData) noexcept {
    SetLogSink(LogSink {.filter = nullptr, .write = write, .flush = nullptr, .userData = userData});
}

[[nodiscard]] ZHLN_API auto GetLogSink() noexcept -> LogSink;

// The process-wide default minimum severity. It is the gate for the stderr
// fallback when no sink is installed, and the level a Logger created without an
// explicit one starts at -- which is how a host's --quiet/--verbose reaches
// engines the factory mints for itself. An explicit LoggerConfig level always
// wins over this.
ZHLN_API void SetLogLevel(LogSeverity minimum) noexcept;

[[nodiscard]] ZHLN_API auto AmbientLogEnabled(LogSeverity severity) noexcept -> bool;
ZHLN_API void               WriteAmbientLog(LogSeverity severity, LogSource source, std::string_view message) noexcept;

// ---- One formatting pass, shared by every sink -----------------------------
// "[file:line] [Fiber:tag] [channel] [SEV] message", the single rendering every
// sink uses (console, file, capture). Header-inline: it is pure text assembly
// over values, with no engine state and no allocation beyond the returned
// string, so a test capture ring, a harness counter and the console all render
// a line identically without resolving a symbol or agreeing on a std::string
// ABI across a DSO boundary.
namespace LogDetail {
constexpr auto SeverityTag(const LogSeverity severity) noexcept -> std::string_view {
    switch (severity) {
        case LogSeverity::Debug:
            return "DEBUG";
        case LogSeverity::Info:
            return "INFO";
        case LogSeverity::Warning:
            return "WARN";
        case LogSeverity::Error:
            return "ERROR";
        case LogSeverity::Off:
            return "OFF";
    }
    return "INFO";
}

constexpr auto SeverityColor(const LogSeverity severity) noexcept -> std::string_view {
    switch (severity) {
        case LogSeverity::Debug:
            return "\x1b[90m";
        case LogSeverity::Info:
            return {};
        case LogSeverity::Warning:
            return "\x1b[33m";
        case LogSeverity::Error:
            return "\x1b[31m";
        case LogSeverity::Off:
            return {};
    }
    return {};
}

constexpr std::string_view kColorReset = "\x1b[0m";

[[nodiscard]] inline auto BaseName(const std::string_view path) noexcept -> std::string_view {
    if (const size_t slash = path.find_last_of("/\\"); slash != std::string_view::npos) {
        return path.substr(slash + 1);
    }
    return path;
}
// The one ambient emission path: one snapshot of the published sink, so the
// gate and the writer that follows belong to the same value.
template <LogSeverity S, typename... Args>
inline void EmitAmbient(const std::string_view channelName, const LogContext ctx, Args&&... args) noexcept {
    if constexpr (S == LogSeverity::Debug && !isDev) {
        return;
    }
    const LogSink sink = GetLogSink();
    // An installed filter owns the decision; with no filter, the level does --
    // including for a sink published by SetLogSink(fn, userData), which carries
    // no filter and used to slip past SetLogLevel entirely.
    if (sink.write != nullptr && sink.filter != nullptr) {
        if (!sink.filter(S, sink.userData)) {
            return;
        }
    } else if (!AmbientLogEnabled(S)) {
        return;
    }
    LogSource source = ctx.Source();
    source.channel   = channelName;
    const std::string message = std::vformat(ctx.fmt, std::make_format_args(args...));
    if (sink.write != nullptr) {
        sink.write(S, source, message, sink.userData);
        return;
    }
    WriteAmbientLog(S, source, message);
}
} // namespace LogDetail

[[nodiscard]] inline auto FormatLogLine(LogSeverity severity, LogSource source, std::string_view message, bool colorize)
    -> std::string {
    const uint64_t fiberId = GetCurrentFiberID();
    char           fiberTag[24];
    if (fiberId == 0) {
        std::snprintf(fiberTag, sizeof(fiberTag), "Thread");
    } else if (fiberId == 1) {
        std::snprintf(fiberTag, sizeof(fiberTag), "Main");
    } else {
        std::snprintf(fiberTag, sizeof(fiberTag), "%#llx", static_cast<unsigned long long>(fiberId));
    }
    const std::string_view file = LogDetail::BaseName(source.file);
    // The channel is whatever the call site named. Core never enumerates them.
    std::string channelPrefix;
    if (!source.channel.empty()) {
        channelPrefix = std::format("[{}] ", source.channel);
    }
    if (severity == LogSeverity::Info && !colorize) {
        return std::format("[{}:{}] [Fiber:{}] {}{}", file, source.line, fiberTag, channelPrefix, message);
    }
    if (colorize) {
        return std::format(
            "[{}:{}] [Fiber:{}] {}[{}{}{}] {}",
            file,
            source.line,
            fiberTag,
            channelPrefix,
            LogDetail::SeverityColor(severity),
            LogDetail::SeverityTag(severity),
            LogDetail::kColorReset,
            message
        );
    }
    return std::format("[{}:{}] [Fiber:{}] {}[{}] {}", file, source.line, fiberTag, channelPrefix, LogDetail::SeverityTag(severity), message);
}

// ---- Channels and the ambient logging entry points -------------------------
// A channel is a name, not an enum value: the subsystem that owns it declares
// the constant, so Core compiles without knowing that Vulkan or Lua exist. The
// members exist for readability at call sites (kVkLog.Warn(...)); they forward
// to the severity-templated entry point below, which is what actually strips
// Debug lines at compile time outside dev builds.
struct LogChannel {
    std::string_view name {};

    template <typename... Args>
    void Debug(const LogContext ctx, Args&&... args) const noexcept {
        LogDetail::EmitAmbient<LogSeverity::Debug>(name, ctx, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Info(const LogContext ctx, Args&&... args) const noexcept {
        LogDetail::EmitAmbient<LogSeverity::Info>(name, ctx, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Warn(const LogContext ctx, Args&&... args) const noexcept {
        LogDetail::EmitAmbient<LogSeverity::Warning>(name, ctx, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Error(const LogContext ctx, Args&&... args) const noexcept {
        LogDetail::EmitAmbient<LogSeverity::Error>(name, ctx, std::forward<Args>(args)...);
    }
};

// The untagged channel: kLog.Warn("frame budget exceeded by {}us", over)
// renders as [file:line] [Fiber:x] with no [Channel] prefix. It names no
// channel, so it belongs here rather than in any subsystem header.
inline constexpr LogChannel kLog {};

// Untagged: Log("booting {}", app).
template <LogSeverity S = LogSeverity::Info, typename... Args>
inline void Log(const LogContext ctx, Args&&... args) noexcept {
    LogDetail::EmitAmbient<S>(std::string_view {}, ctx, std::forward<Args>(args)...);
}

// Channelled: Log<LogSeverity::Warning>(kVkLog, "swapchain {}", n).
template <LogSeverity S = LogSeverity::Info, typename... Args>
inline void Log(const LogChannel channel, const LogContext ctx, Args&&... args) noexcept {
    LogDetail::EmitAmbient<S>(channel.name, ctx, std::forward<Args>(args)...);
}

[[noreturn]] ZHLN_API void InternalPanic(const char* file, uint32_t line, std::string_view message);

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

template <std::ranges::contiguous_range A, std::ranges::contiguous_range B, typename... Args>
    requires std::ranges::sized_range<A> && std::ranges::sized_range<B>
void AssertNoAliasing(const A& src, const B& dst, LogContext ctx, const Args&... args) {
    // Half-open byte ranges [a, a+n) and [b, b+m) overlap iff a < b+m && b < a+n.
    const bool overlap = [](std::span<const std::byte> a, std::span<const std::byte> b) {
        const std::less<> less {}; // total order even for unrelated pointers
        return a.empty() || b.empty() ? false : less(a.data(), b.data() + b.size()) && less(b.data(), a.data() + a.size());
    }(std::as_bytes(std::span {std::ranges::data(src), std::ranges::size(src)}), std::as_bytes(std::span {std::ranges::data(dst), std::ranges::size(dst)}));

    if constexpr (isDev) {
        if (overlap) [[unlikely]] {
            const std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
            InternalPanic(ctx.loc.file_name(), ctx.loc.line(), formatted);
        }
    } else {
        [[assume(!overlap)]];
    }
}

} // namespace ZHLN
