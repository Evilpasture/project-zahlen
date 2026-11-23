// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// The COM adapter over the engine-first logger. This is the only file that
// knows both worlds: it owns a plain ZHLN::Logger and satisfies ILoggerHub in
// terms of it. Everything in src/engine/Logger.{hpp,cpp} stays COM-free
// (enforced by configure/check_com_boundary.py).

#include "Logger.hpp"
#include <Zahlen/COM/ILogger.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <mutex>
#include <Zahlen/FileSystem/Paths.hpp>
#include <cstdio>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::COM {

namespace {

auto SeverityForLevel(const LogLevel level) noexcept -> LogSeverity {
    switch (level) {
        case LogLevel::Quiet:
            return LogSeverity::Off;
        case LogLevel::Moderate:
            return LogSeverity::Info;
        case LogLevel::Verbose:
            return LogSeverity::Debug;
    }
    return LogSeverity::Info;
}

// The COM adapter over the engine-first logger: it owns a plain ZHLN::Logger
// and adds exactly what the COM contract needs -- the ILoggerHub sink list
// (in ILogger* terms) and the chain-driven configuration. All formatting,
// filtering and sink storage live in ZHLN::Logger.
class LoggerHub final: public Implements<LoggerHub, ILoggerHub> {
  public:
    [[nodiscard]] auto Engine() noexcept -> ZHLN::Logger& {
        return *_logger;
    }

    [[nodiscard]] bool IsEnabled(const LogSeverity severity) const noexcept override {
        return _logger->IsEnabled(severity);
    }

    void Write(const LogSeverity severity, const LogSource source, const StringView message) const noexcept override {
        _logger->Write(severity, source, std::string_view(message));
    }

    void Flush() const noexcept override {
        _logger->Flush();
    }

    void SetMinSeverity(const LogSeverity minSeverity) noexcept override {
        _logger->SetMinimumSeverity(minSeverity);
    }

    auto AddSink(ILogger* sink) noexcept -> Result<uint64_t> override {
        if (sink == nullptr) {
            return Error::NullArgument;
        }
        const uint64_t token = _logger->AddExternalSink(sink->AsSink());
        // The registered LogSink's userData points at this ILogger, so the hub
        // holds a strong reference of its own: a caller that drops (or never
        // keeps) its Ref cannot leave a dangling sink behind. Released by
        // RemoveSink, or when the hub itself dies.
        const std::scoped_lock lock(_sinkMutex);
        _held.emplace_back(token, Ref<ILogger>(sink));
        return token;
    }

    void RemoveSink(const uint64_t token) noexcept override {
        (void) _logger->RemoveExternalSink(token);
        const std::scoped_lock lock(_sinkMutex);
        std::erase_if(_held, [&](const auto& entry) { return entry.first == token; });
    }

    // A COM logger is still a Layer-0 sink: the engine below COM (and any
    // host that only wants the lines) installs this value.
    [[nodiscard]] auto AsSink() const noexcept -> ZHLN::LogSink {
        return _logger->AsLogSink();
    }

  private:
    // Declared before the held refs, so the logger outlives every sink it
    // still carries while the table is torn down.
    std::unique_ptr<ZHLN::Logger>                   _logger = std::make_unique<ZHLN::Logger>();
    mutable Mutex                                   _sinkMutex;
    std::vector<std::pair<uint64_t, Ref<ILogger>>>  _held;
};

} // namespace

auto CreateLoggerRaw(const ChainInNode* config) noexcept -> Result<ILoggerHub*> {
    auto* hub = new (std::nothrow) LoggerHub();
    if (hub == nullptr) {
        return Error::AllocationFailed;
    }

    bool sawSink = false;
    if (config != nullptr) {
        const Result<void> read = DispatchInChain<LoggerConfig, ConsoleSinkConfig, FileSinkConfig>(
            config, [&]<typename Node>(const Node& node) -> Result<void> {
                if constexpr (std::is_same_v<Node, LoggerConfig>) {
                    hub->SetMinSeverity(SeverityForLevel(node.level));
                    if (node.logger != nullptr) {
                        (void) hub->AddSink(node.logger);
                        sawSink = true;
                    }
                } else if constexpr (std::is_same_v<Node, ConsoleSinkConfig>) {
                    hub->Engine().AddConsoleSink(node.colorize, node.useStdErr);
                    sawSink = true;
                } else {
                    // FileSinkConfig: default location is the cache dir, never
                    // the process CWD; an unusable path just yields no sink.
                    const std::string path = node.path.size != 0
                                                 ? std::string(std::string_view(node.path))
                                                 : (FS::Paths::CacheDir() / "zahlen_runtime.log").string();
                    if (FILE* file = std::fopen(path.c_str(), node.append ? "a" : "w"); file != nullptr) {
                        hub->Engine().AddFileSink(file, false);
                        sawSink = true;
                    }
                }
                return {};
            }
        );
        if (!read.HasValue()) {
            hub->Release();
            return read.error;
        }
    }
    if (!sawSink) {
        hub->Engine().AddConsoleSink(true, true);
    }
    return static_cast<ILoggerHub*>(hub);
}

} // namespace ZHLN::COM
