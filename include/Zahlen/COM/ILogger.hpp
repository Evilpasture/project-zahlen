// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/ILogger.hpp
//
// Logging is dependency-injected: a logger is a standalone refcounted COM
// object created via CreateLoggerRaw *before* CreateEngineRaw and handed to
// it through a LoggerConfig chain node, so startup failures log into the
// caller's sink and a failed CreateEngineRaw never strands the log. ILogger
// is the minimal sink interface (trivial to mock in tests); ILoggerHub is
// the C# ILoggerFactory analogue that owns sinks and the minimum severity.
// There is deliberately no per-call category: LogSource already captures
// file/function/line via std::source_location, which answers "where from"
// better than a hand-maintained string repeated at every call site (and
// nobody ever called SetCategorySeverity, so the filter it fed is gone).
// Nothing here is a global: whoever creates an engine decides where its
// lines go.

#include <Zahlen/COM/LoggerTypes.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdio>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ZHLN::COM {

struct ILogger : IObject {
    [[nodiscard]] virtual bool IsEnabled(LogSeverity severity) const noexcept = 0;
    virtual void               Write(LogSeverity severity, LogSource source, StringView message) const noexcept = 0;
    virtual void               Flush() const noexcept = 0;

    // ---- Layer-0 adapter (never in the vtable) ----------------------------
    // A COM logger is also a plain ZHLN::LogSink, so engine code below the
    // COM boundary -- KernelSession installing the ambient sink, LogicWorld,
    // any leaf library -- forwards lines here without naming ILogger, and the
    // level gate (IsEnabled) travels with the writer as one value.
    [[nodiscard]] auto AsSink() const noexcept -> ZHLN::LogSink {
        return ZHLN::LogSink {
            .filter   = &FilterThunk,
            .write    = &WriteThunk,
            .flush    = &FlushThunk,
            .userData = const_cast<ILogger*>(this),
        };
    }

    // ---- Consumer-TU ergonomics (never in the vtable) ---------------------
    // The IsEnabled gate runs before any formatting; a disabled line costs
    // one virtual call and no allocation.

    template <typename... Args>
    void Log(LogSeverity severity, LogContext ctx, Args&&... args) const noexcept {
        if (!IsEnabled(severity)) {
            return;
        }
        const std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
        Write(severity, ctx.Source(), StringView(formatted));
    }


  protected:
    ~ILogger() = default;

  private:
    static auto FilterThunk(const LogSeverity severity, void* userData) noexcept -> bool {
        const auto* logger = static_cast<const ILogger*>(userData);
        return logger != nullptr && logger->IsEnabled(severity);
    }

    static void WriteThunk(const LogSeverity severity, const LogSource source, const std::string_view message, void* userData) noexcept {
        if (const auto* logger = static_cast<const ILogger*>(userData); logger != nullptr) {
            logger->Write(severity, source, StringView(message.data(), message.size()));
        }
    }

    static void FlushThunk(void* userData) noexcept {
        if (const auto* logger = static_cast<const ILogger*>(userData); logger != nullptr) {
            logger->Flush();
        }
    }
};

struct ILoggerHub : ILogger {
    virtual void            SetMinSeverity(LogSeverity minSeverity) noexcept = 0;
    virtual auto            AddSink(ILogger* sink) noexcept -> Result<uint64_t> = 0;
    virtual void            RemoveSink(uint64_t token) noexcept = 0;

  protected:
    ~ILoggerHub() = default;
};

// Exported by libzahlen_engine. Accepts LoggerConfig (initial level),
// ConsoleSinkConfig and FileSinkConfig chain nodes; with no sink nodes the
// hub gets one console sink. The returned hub has one reference, already
// taken -- adopt it, do not AddRef it.
ZHLN_API auto CreateLoggerRaw(const ChainInNode* config = nullptr) noexcept -> Result<ILoggerHub*>;

[[nodiscard]] inline auto CreateLogger(const ChainInNode* config = nullptr) noexcept
    -> std::expected<Ref<ILoggerHub>, ErrorCode> {
    const Result<ILoggerHub*> created = CreateLoggerRaw(config);
    if (!created.HasValue()) return std::unexpected(created.error);
    return Ref<ILoggerHub>::Adopt(created.value);
}


// ---- Ambient (execution-context) logger ----------------------------------
// Logging is a Layer-0 primitive: leaf code (codecs, protocol clients,
// render managers) cannot always hold an ILogger, and must never fall back
// to stderr or a mutable global. The active sink pointer lives inside
// libzahlen_engine behind these exported functions -- never as an inline
// thread_local in a header, which every DSO would duplicate under
// `local: *;` symbol hiding and which TaskSystem worker threads (and
// fibers resuming on other workers) would never see. Engine creation
// installs the sink once (KernelSession / LogicWorld). With no sink
// installed, ZHLN::Log falls back to an observer or stderr (Zahlen/Log.hpp)
// -- never to silence.
// Installing a logger as the ambient sink needs no dedicated entry point:
// ZHLN::SetLogSink(logger->AsSink()) publishes its gate and writer as one
// value, and ZHLN::GetLogSink() is what a host saves to restore it.


} // namespace ZHLN::COM

namespace ZHLN {

// ---- The one explicit-logger logging entry point ---------------------------
// Log<LogTag::Warning>(logger, "...", args...) targets an explicit
// logger; Log<LogTag::Warning>("...", args...) (Zahlen/Log.hpp)
// targets the ambient sink. The default severity is Info. A null logger is
// a deliberate "do not log" (C#'s NullLogger), never an stderr fallback.
// Debug calls are stripped at compile time outside dev builds; the
// IsEnabled gate in ILogger::Log still suppresses formatting whenever the
// severity is disabled.
template <typename Tag1 = LogTag::Info, typename Tag2 = void, typename... Args>
inline void Log(const COM::ILogger* logger, const LogContext ctx, Args&&... args) noexcept {
    constexpr LogSeverity severity = TemplatedDetail::ResolveLogSeverity<Tag1, Tag2>();
    constexpr LogDomain   domain   = TemplatedDetail::ResolveLogDomain<Tag1, Tag2>();
    if constexpr (severity == LogSeverity::Debug && !isDev) {
        return;
    } else if (logger != nullptr && logger->IsEnabled(severity)) {
        LogSource source = ctx.Source();
        source.domain    = domain;
        logger->Write(severity, source, COM::StringView(std::vformat(ctx.fmt, std::make_format_args(args...))));
    }
}

} // namespace ZHLN
