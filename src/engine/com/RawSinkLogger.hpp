// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// src/engine/com/RawSinkLogger.hpp  (engine-private)
//
// The reverse of Zahlen/LogBridge.hpp::ToSink: a Layer-0 LogSink exposed as a
// COM::ILogger, so a hub created by CreateEngineRaw can chain a sink the host
// installed before the engine existed (a test capture ring, a harness counter)
// through the ordinary AddSink path instead of a bespoke raw-sink slot on the
// COM interface. Only src/engine/com/ includes it.

#include <Zahlen/COM/ILogger.hpp>
#include <Zahlen/Log.hpp>

#include <string_view>

namespace ZHLN::COM {

class RawSinkLogger final : public Implements<RawSinkLogger, ILogger> {
  public:
    explicit RawSinkLogger(ZHLN::LogSink sink) noexcept: _sink(sink) {
    }

    [[nodiscard]] auto IsEnabled(const LogSeverity severity) const noexcept -> bool override {
        if (_sink.filter != nullptr) {
            return _sink.filter(static_cast<ZHLN::LogSeverity>(severity), _sink.userData);
        }
        return true;
    }

    void Write(const LogSeverity severity, const LogSource source, const StringView message) const noexcept override {
        if (_sink.write == nullptr) {
            return;
        }
        _sink.write(
            static_cast<ZHLN::LogSeverity>(severity),
            ZHLN::LogSource {
                .file     = std::string_view(source.file.data, source.file.size),
                .function = std::string_view(source.function.data, source.function.size),
                .channel  = std::string_view(source.channel.data, source.channel.size),
                .line     = source.line,
            },
            std::string_view(message.data, message.size),
            _sink.userData
        );
    }

    void Flush() const noexcept override {
        if (_sink.flush != nullptr) {
            _sink.flush(_sink.userData);
        }
    }

  private:
    ZHLN::LogSink _sink;
};

} // namespace ZHLN::COM
