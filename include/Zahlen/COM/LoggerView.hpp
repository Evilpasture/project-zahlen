// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/LoggerView.hpp
//
// The ergonomic face of ILogger for consumers: one non-owning value that pairs a
// logger with an optional channel, so a call site reads `log.Warn("...", x)`
// instead of naming the severity in a template argument and the channel in a
// second one.
//
// It is a struct of one pointer and one string_view: not virtual, not in the
// vtable, no exported symbol, zero ABI. The logging still goes through the same
// free LogTo -- gate first, format second -- so a disabled line costs a branch.
//
//   const ZHLN::COM::LoggerView log {engine->Logger(), "RemoteGLB"};
//   log.Info("Imported '{}'", virtualPath);
//   log.Warn("Font asset failed to load ({})", fontID.error());
//
// An empty channel is untagged and renders without a [Channel] prefix.

#include <Zahlen/COM/ILogger.hpp>
#include <string_view>
#include <utility>

namespace ZHLN::COM {

struct LoggerView {
    ILogger*         logger  = nullptr;
    std::string_view channel {};

    template <typename... Args>
    void Debug(const LogContext ctx, Args&&... args) const noexcept {
        LogTo<LogSeverity::Debug>(logger, channel, ctx, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Info(const LogContext ctx, Args&&... args) const noexcept {
        LogTo<LogSeverity::Info>(logger, channel, ctx, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Warn(const LogContext ctx, Args&&... args) const noexcept {
        LogTo<LogSeverity::Warning>(logger, channel, ctx, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void Error(const LogContext ctx, Args&&... args) const noexcept {
        LogTo<LogSeverity::Error>(logger, channel, ctx, std::forward<Args>(args)...);
    }
};

} // namespace ZHLN::COM
