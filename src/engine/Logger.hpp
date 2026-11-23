// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Log.hpp>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN {

// The engine's logger as a plain C++ class -- the same shape as the other
// engine subsystems (AudioContext, RenderContext, InputContext): owned and
// used natively inside the engine, adapted to COM::ILoggerHub only at
// src/engine/com/. It knows nothing about IObject, Chain.hpp or reflection.
//
// ZHLN::Log speaks LogSink, so AsLogSink() is the whole integration surface:
// whoever needs this logger's output -- KernelSession installing the ambient
// sink, a host forwarding to its own hub, a test capture -- installs that
// one value, and gets the level gate (filter) along with the writer.
class Logger {
  public:
    Logger();
    Logger(const Logger&)                    = delete;
    auto operator=(const Logger&) -> Logger& = delete;
    Logger(Logger&&)                         = delete;
    auto operator=(Logger&&) -> Logger&      = delete;
    ~Logger();

    [[nodiscard]] auto IsEnabled(LogSeverity severity) const noexcept -> bool;
    void               Write(LogSeverity severity, LogSource source, std::string_view message) const noexcept;
    void               Flush() const noexcept;

    void                        SetMinimumSeverity(LogSeverity minimum) noexcept;
    [[nodiscard]] auto          MinimumSeverity() const noexcept -> LogSeverity;

    void AddConsoleSink(bool colorize, bool useStdErr);
    // Takes ownership of `file` (closed on destruction).
    void AddFileSink(std::FILE* file, bool colorize);

    // External sinks are plain LogSink values, so the engine forwards to any
    // other logger (a host hub, a test capture) without naming COM.
    auto AddExternalSink(LogSink sink) noexcept -> uint64_t;
    bool RemoveExternalSink(uint64_t token) noexcept;

    // This logger as a Layer-0 sink: filter = IsEnabled, write = Write,
    // flush = Flush. Everything the engine below COM needs.
    [[nodiscard]] auto AsLogSink() const noexcept -> LogSink;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
