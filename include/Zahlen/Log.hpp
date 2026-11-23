// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Log.hpp
//
// Fatal traps only. Normal logging is dependency-injected through
// COM::ILogger (Zahlen/COM/ILogger.hpp): whoever creates an engine decides
// where its lines go, and no logging global exists. Panic/Assert are the
// deliberate exception -- when an invariant fails, the heap, the sinks, or
// the logger itself may already be broken, so they format into stack storage
// and write to fd 2 directly (src/engine/diagnostics/Diagnostics.cpp).

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <cstdint>
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

enum class LogChannel : uint8_t { StdErr, StdOut, File };

// ---- Logging primitives (Core, Layer 0) -----------------------------------
// Severity, source and context live here in ZHLN::, not in ZHLN::COM: leaf
// code in pure libraries logs through these without naming a COM interface.
// COM::ILogger (Zahlen/COM/ILogger.hpp) is one sink implementation; the
// COM headers alias these names for ABI compatibility.
enum class LogSeverity : uint8_t { Debug, Info, Warning, Error, Off };

// Structured origin domain for a log line. Filtering/matching on this --
// never on substrings of the source path ("vulkan" in file) -- is what the
// tag API below feeds. At the COM virtual boundary it travels as this plain
// fixed-layout value inside LogSource; the vtable stays non-templated.
enum class LogDomain : uint8_t { General, Vulkan, DeviceLost, Render, Audio, Network, Physics, Script };

// ABI-safe replacement for std::source_location (whose layout is not
// guaranteed identical across STLs).
struct LogSource {
    std::string_view file {};
    std::string_view function {};
    uint32_t         line   = 0;
    LogDomain        domain = LogDomain::General;
};

// ---- Log tags ---------------------------------------------------------------
// Empty tag structs, not enum values, as template parameters: severity and
// domain are orthogonal axes, and tags let a call site pass either, both,
// or neither, in any order:
//   Log("booting")                              -> Info + General
//   Log<LogTag::Warning>("bad {}", id)          -> Warning + General
//   Log<LogTag::Error, LogTag::Vulkan>("{}", m) -> Error + Vulkan
//   Log<LogTag::Vulkan>("{}", m)               -> Info + Vulkan
// The tags collapse to plain LogSeverity/LogDomain values before the COM
// virtual boundary, so ILogger::Write stays a single non-templated slot.
namespace LogTag {
struct Debug { static constexpr auto severity = LogSeverity::Debug; };
struct Info { static constexpr auto severity = LogSeverity::Info; };
struct Warning { static constexpr auto severity = LogSeverity::Warning; };
struct Error { static constexpr auto severity = LogSeverity::Error; };
struct General { static constexpr auto domain = LogDomain::General; };
struct Vulkan { static constexpr auto domain = LogDomain::Vulkan; };
struct DeviceLost { static constexpr auto domain = LogDomain::DeviceLost; };
struct Render { static constexpr auto domain = LogDomain::Render; };
struct Audio { static constexpr auto domain = LogDomain::Audio; };
struct Network { static constexpr auto domain = LogDomain::Network; };
struct Physics { static constexpr auto domain = LogDomain::Physics; };
struct Script { static constexpr auto domain = LogDomain::Script; };
} // namespace LogTag

namespace TemplatedDetail {
template <typename T>
inline constexpr bool kTagHasSeverity = requires { T::severity; };
template <typename T>
inline constexpr bool kTagHasDomain = requires { T::domain; };

// A tag pair is (at most) one severity tag plus (at most) one domain tag.
// Rejecting the ambiguous/typo'd forms here makes Log<LogTag::Warning,
// LogTag::Error>(...) and Log<SomeTypo>(...) compile errors instead of
// silently dropping half the tag.
template <typename T>
inline constexpr bool kIsLogTag = std::is_empty_v<T> && (kTagHasSeverity<T> || kTagHasDomain<T>);

template <typename A, typename B>
constexpr bool ValidateLogTags() {
    static_assert(std::is_void_v<A> || kIsLogTag<A>, "ZHLN::Log tag must be a ZHLN::LogTag struct exposing ::severity or ::domain");
    if constexpr (!std::is_void_v<B>) {
        static_assert(kIsLogTag<B>, "ZHLN::Log tag must be a ZHLN::LogTag struct exposing ::severity or ::domain");
        static_assert(
            !(kTagHasSeverity<A> && kTagHasSeverity<B>), "ZHLN::Log takes at most one severity tag (e.g. Log<LogTag::Warning>)"
        );
        static_assert(!(kTagHasDomain<A> && kTagHasDomain<B>), "ZHLN::Log takes at most one domain tag (e.g. Log<LogTag::Vulkan>)");
    } else if constexpr (!std::is_void_v<A>) {
        static_assert(!(kTagHasSeverity<A> && kTagHasDomain<A>), "ZHLN::Log tag must carry either ::severity or ::domain, not both");
    }
    return true;
}

template <typename A, typename B>
consteval auto ResolveLogSeverity() noexcept -> LogSeverity {
    static_assert(ValidateLogTags<A, B>());
    if constexpr (kTagHasSeverity<A>) {
        return A::severity;
    } else if constexpr (!std::is_void_v<B> && kTagHasSeverity<B>) {
        return B::severity;
    } else {
        return LogSeverity::Info;
    }
}

template <typename A, typename B>
consteval auto ResolveLogDomain() noexcept -> LogDomain {
    if constexpr (kTagHasDomain<A>) {
        return A::domain;
    } else if constexpr (!std::is_void_v<B> && kTagHasDomain<B>) {
        return B::domain;
    } else {
        return LogDomain::General;
    }
}
} // namespace TemplatedDetail


// A format string plus where it was written. Implicitly constructible from
// a string literal at the call site, capturing source_location there.
struct LogContext {
    std::string_view     fmt;
    std::source_location loc;

    // Constrained to string-like types: an unconstrained template here
    // would make any pointer (say, an ILogger*) look convertible to a
    // LogContext, and the ambient/explicit Log overloads would be
    // ambiguous at every call site.
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
// The active sink pointer lives inside libzahlen_engine behind these
// exported functions -- never as an inline thread_local in a header, which
// every DSO would duplicate under `local: *;` symbol hiding and which
// TaskSystem worker threads (and fibers resuming on other workers) would
// never see. Engine creation installs the sink once; every thread in the
// process reads the same atomic pointer.
// Where lines go, in priority order:
//   1. the ambient sink an engine installed at creation (its ILogger hub,
//      with whatever sinks the host configured -- console, file, capture);
//   2. the sink registered with SetLogSink, if any (test capture,
//      harness validation counters);
//   3. stderr, formatted like any other log line.
// The stderr default is deliberate: ZHLN::Log works in main(), in tools,
// in unit tests and in leaf libraries with zero initialization, like
// every normal logger. Hosts that want lines routed elsewhere install a
// sink or an observer; nobody has to install anything to avoid silence.
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

ZHLN_API void SetLogSink(LogSink sink) noexcept;

inline void SetLogSink(const LogSinkFn write, void* const userData) noexcept {
    SetLogSink(LogSink {.filter = nullptr, .write = write, .flush = nullptr, .userData = userData});
}

[[nodiscard]] ZHLN_API auto GetLogSink() noexcept -> LogSink;
ZHLN_API void               SetLogLevel(LogSeverity minimum) noexcept;

[[nodiscard]] ZHLN_API auto AmbientLogEnabled(LogSeverity severity) noexcept -> bool;
ZHLN_API void               WriteAmbientLog(LogSeverity severity, LogSource source, std::string_view message) noexcept;

// ---- The one ambient logging entry point -----------------------------------
// Tags (above) carry severity/domain: Log<LogTag::Warning>("...", args...)
// logs through the ambient sink.
// The default severity is Info. Debug calls are stripped at compile time
// outside dev builds; AmbientLogEnabled still suppresses formatting
// whenever the severity is disabled. (The explicit-logger overload,
// Log<Severity>(logger, ...), lives in Zahlen/COM/ILogger.hpp.)
template <typename Tag1 = LogTag::Info, typename Tag2 = void, typename... Args>
inline void Log(const LogContext ctx, Args&&... args) noexcept {
    constexpr LogSeverity severity = TemplatedDetail::ResolveLogSeverity<Tag1, Tag2>();
    constexpr LogDomain   domain   = TemplatedDetail::ResolveLogDomain<Tag1, Tag2>();
    if constexpr (severity == LogSeverity::Debug && !isDev) {
        return;
    } else if (AmbientLogEnabled(severity)) {
        LogSource source = ctx.Source();
        source.domain    = domain;
        WriteAmbientLog(severity, source, std::vformat(ctx.fmt, std::make_format_args(args...)));
    }
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

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void;
auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine) noexcept -> bool;

} // namespace ZHLN
