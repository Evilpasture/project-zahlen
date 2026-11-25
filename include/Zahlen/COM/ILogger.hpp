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
// The interface is six calls over fixed-layout values and names no Layer-0
// type. The two free helpers below do format -- one std::vformat per *enabled*
// line, and the severity gate runs first, so a line that is off costs a branch
// and nothing else. They exist so a COM consumer can write a line without
// acquiring Layer-0's ambient sink; the other direction (publishing a COM
// logger as the ambient sink) is the optional Zahlen/LogBridge.hpp.
//
// There is no per-call category: LogSource captures file/function/line via
// std::source_location, and the channel names the subsystem.
// Nothing here is a global: whoever creates an engine decides where its
// lines go.

#include <Zahlen/COM/ErrorTypes.hpp>
#include <Zahlen/COM/LoggerTypes.hpp>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN::COM {

struct ILogger : IObject {
    [[nodiscard]] virtual bool IsEnabled(LogSeverity severity) const noexcept = 0;
    virtual void               Write(LogSeverity severity, LogSource source, StringView message) const noexcept = 0;
    virtual void               Flush() const noexcept                   = 0;

  protected:
    ~ILogger() = default;
};

struct ILoggerHub : ILogger {
    virtual void SetMinSeverity(LogSeverity minSeverity) noexcept     = 0;
    // Takes a second reference on `sink`; RemoveSink drops it.
    virtual auto AddSink(ILogger* sink) noexcept -> Result<uint64_t>  = 0;
    virtual void RemoveSink(uint64_t token) noexcept                  = 0;

  protected:
    ~ILoggerHub() = default;
};

// ---- Consumer-TU logging (never in the vtable) -----------------------------
// Formats one line and hands it over. Deliberately COM-shaped: COM::LogSeverity,
// a channel name, COM::StringView. It names no Layer-0 type and includes no
// Layer-0 header, so a strict COM consumer can log without acquiring
// std::source_location plumbing or the ambient sink.
//
// The trailing source_location parameter is defaulted at each call site, which
// is what gives a COM line the same [file:line] the ambient path has.
// Channelled: LogTo(logger, "Vulkan", "swapchain {}", n). Gate first, format
// second -- vformat plus a heap string is the expensive half, and a line nobody
// will read must not pay it: a Quiet or gated hub costs one branch, and a Debug
// line is not compiled at all in a release build.
template <LogSeverity S = LogSeverity::Info, typename... Args>
inline void LogTo(ILogger* logger, const std::string_view channel, const LogContext ctx, Args&&... args) noexcept {
    if constexpr (S == LogSeverity::Debug && !isDev) {
        return;
    } else {
        if (logger == nullptr || !logger->IsEnabled(S)) {
            return;
        }
        const std::string message = std::vformat(ctx.fmt, std::make_format_args(args...));
        const LogSource   source {
            .file     = StringView(ctx.loc.file_name()),
            .function = StringView(ctx.loc.function_name()),
            .channel  = StringView(channel),
            .line     = static_cast<uint32_t>(ctx.loc.line()),
        };
        logger->Write(S, source, StringView(message.data(), message.size()));
    }
}

// Untagged: Log(logger, "loaded {} parts", n) -- LogTo with no channel, so the
// line renders without a [Channel] prefix.
template <LogSeverity S = LogSeverity::Info, typename... Args>
inline void Log(ILogger* logger, const LogContext ctx, Args&&... args) noexcept {
    LogTo<S>(logger, {}, ctx, std::forward<Args>(args)...);
}

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
// render managers) cannot always hold an ILogger, and must never fall back to
// a private global of its own. The ambient sink is a single value published by
// ZHLN::SetLogSink (Zahlen/Log.hpp) and stored inside libzahlen_engine -- never
// an inline object in a header, which every DSO would duplicate under
// `local: *;` symbol hiding and which TaskSystem worker threads (and fibers
// resuming on other workers) would never see. Engine creation installs it once
// (KernelSession installs the hub it was handed; LogicWorld installs its own
// only when no session already published one). With no sink installed,
// ZHLN::Log writes to stderr through the same formatting pass -- never to
// silence, and never without the file/fiber/severity the line came from.
// Installing a logger as the ambient sink goes through the COM bridge
// (ZHLN::COM::LogBridge::ToSink, or the opt-in Zahlen/LogBridge.hpp): that
// publishes the logger's gate and writer as one Layer-0 value, and
// ZHLN::GetLogSink() is what a host saves to restore it. ILogger itself has no
// AsSink member -- the interface stays a pure vtable.


} // namespace ZHLN::COM


