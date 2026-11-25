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
#include "LogBridge.hpp"
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::COM {

namespace {

// A wire StringView is valid when it is empty or points at storage; building a
// std::string_view from {nullptr, n} is undefined.
[[nodiscard]] bool Valid(const StringView value) noexcept {
    return value.size == 0 || value.data != nullptr;
}

[[nodiscard]] auto Text(const StringView value) noexcept -> std::string_view {
    return value.size != 0 ? std::string_view(value.data, value.size) : std::string_view {};
}

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
        return _logger->IsEnabled(static_cast<ZHLN::LogSeverity>(severity));
    }

    void Write(const LogSeverity severity, const LogSource source, const StringView message) const noexcept override {
        // An empty channel means untagged and is legal; a non-empty one is a
        // wire string like any other and must be well-formed before it is read.
        if (!Valid(source.file) || !Valid(source.function) || !Valid(message) ||
            (source.channel.size != 0 && !Valid(source.channel))) {
            return;
        }
        // Wire -> native, once, at the only point that knows both shapes.
        _logger->Write(
            static_cast<ZHLN::LogSeverity>(severity),
            ZHLN::LogSource {
                .file     = Text(source.file),
                .function = Text(source.function),
                .channel  = Text(source.channel),
                .line     = source.line,
            },
            Text(message)
        );
    }

    void Flush() const noexcept override {
        _logger->Flush();
    }

    void SetMinSeverity(const LogSeverity minSeverity) noexcept override {
        _logger->SetMinimumSeverity(static_cast<ZHLN::LogSeverity>(minSeverity));
    }

    auto AddSink(ILogger* sink) noexcept -> Result<uint64_t> override {
        if (sink == nullptr) {
            return Error::NullArgument;
        }
        const uint64_t token = _logger->AddExternalSink(ZHLN::LogBridge::ToSink(sink));
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
                    if (!Valid(node.path)) {
                        return Error::NullArgument;
                    }
                    const std::string path = node.path.size != 0
                                                 ? std::string(std::string_view(node.path))
                                                 : (FS::Paths::CacheDir() / "zahlen_runtime.log").string();
                    // The default lives under CacheDir(), which on a first run
                    // does not exist yet -- creating only the file would fail
                    // with ENOENT and silently drop the one sink a host can
                    // configure by name.
                    if (const std::filesystem::path parent = std::filesystem::path(path).parent_path(); !parent.empty()) {
                        std::error_code ec;
                        std::filesystem::create_directories(parent, ec);
                        // A directory that cannot be created just yields no
                        // sink below, as does an unopenable file.
                    }
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
