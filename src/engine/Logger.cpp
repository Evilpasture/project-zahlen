// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include "Logger.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

using ZHLN::LogDomain;
using ZHLN::LogSeverity;
using ZHLN::LogSource;

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

auto ShouldColorize(FILE* stream) noexcept -> bool {
    // NO_COLOR (https://no-color.org/): present and non-empty disables color.
    // Cached once per stream: isatty does not change its answer.
    static const bool stderrColor = [] {
        if (const char* noColor = std::getenv("NO_COLOR"); (noColor != nullptr) && (noColor[0] != '\0')) {
            return false;
        }
#if defined(_WIN32)
        return _isatty(_fileno(stderr)) != 0;
#else
        return ::isatty(::fileno(stderr)) != 0;
#endif
    }();
    static const bool stdoutColor = [] {
        if (const char* noColor = std::getenv("NO_COLOR"); (noColor != nullptr) && (noColor[0] != '\0')) {
            return false;
        }
#if defined(_WIN32)
        return _isatty(_fileno(stdout)) != 0;
#else
        return ::isatty(::fileno(stdout)) != 0;
#endif
    }();
    return (stream == stdout) ? stdoutColor : stderrColor;
}

auto BaseName(const std::string_view path) noexcept -> std::string_view {
    if (const auto pos = path.find_last_of("/\\"); pos != std::string_view::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

auto DomainTag(const LogDomain domain) noexcept -> std::string_view {
    switch (domain) {
        case LogDomain::General: return {};
        case LogDomain::Vulkan: return "Vulkan";
        case LogDomain::DeviceLost: return "DeviceLost";
        case LogDomain::Render: return "Render";
        case LogDomain::Audio: return "Audio";
        case LogDomain::Network: return "Network";
        case LogDomain::Physics: return "Physics";
        case LogDomain::Script: return "Script";
    }
    return {};
}

// One formatting pass shared by every sink:
// [file:line] [Fiber:tag] [Domain] [SEV] message
auto FormatLine(const LogSeverity severity, const LogSource source, const std::string_view message, const bool colorize)
    -> std::string {
    const uint64_t fiberId = ZHLN::GetCurrentFiberID();
    char           fiberTag[24];
    if (fiberId == 0) {
        std::snprintf(fiberTag, sizeof(fiberTag), "Thread");
    } else if (fiberId == 1) {
        std::snprintf(fiberTag, sizeof(fiberTag), "Main");
    } else {
        std::snprintf(fiberTag, sizeof(fiberTag), "%#llx", static_cast<unsigned long long>(fiberId));
    }
    const std::string_view file = BaseName(source.file);
    // The domain tag Log<LogTag::Vulkan>/<DeviceLost>/<Script>... selects is
    // rendered here, in the one place that formats a line, so call sites no
    // longer hand-prefix message strings with "[Tag] ".
    std::string domainPrefix;
    if (const std::string_view domainTag = DomainTag(source.domain); !domainTag.empty()) {
        domainPrefix = std::format("[{}] ", domainTag);
    }
    if (severity == LogSeverity::Info && !colorize) {
        return std::format("[{}:{}] [Fiber:{}] {}{}", file, source.line, fiberTag, domainPrefix, message);
    }
    if (colorize) {
        return std::format(
            "[{}:{}] [Fiber:{}] {}[{}{}{}] {}",
            file,
            source.line,
            fiberTag,
            domainPrefix,
            SeverityColor(severity),
            SeverityTag(severity),
            kColorReset,
            message
        );
    }
    return std::format("[{}:{}] [Fiber:{}] {}[{}] {}", file, source.line, fiberTag, domainPrefix, SeverityTag(severity), message);
}

struct ConsoleSink {
    bool colorize  = true;
    bool useStdErr = true;

    void Write(const LogSeverity severity, const LogSource source, const std::string_view message) const noexcept {
        FILE*                  stream = useStdErr ? stderr : stdout;
        const std::string      line   = FormatLine(severity, source, message, colorize && ShouldColorize(stream));
        std::fwrite(line.data(), 1, line.size(), stream);
        std::fputc('\n', stream);
    }
};

struct FileSink {
    FILE* file = nullptr;

    FileSink() = default;
    FileSink(const FileSink&)            = delete;
    FileSink& operator=(const FileSink&) = delete;
    FileSink(FileSink&& other) noexcept: file(std::exchange(other.file, nullptr)) {
    }
    FileSink& operator=(FileSink&& other) noexcept {
        if (this != &other) {
            if (file != nullptr) std::fclose(file);
            file = std::exchange(other.file, nullptr);
        }
        return *this;
    }
    ~FileSink() {
        if (file != nullptr) std::fclose(file);
    }

    void Write(const LogSeverity severity, const LogSource source, const std::string_view message) const noexcept {
        if (file == nullptr) {
            return;
        }
        const std::string line = FormatLine(severity, source, message, false);
        std::fwrite(line.data(), 1, line.size(), file);
        std::fputc('\n', file);
    }
    void Flush() const noexcept {
        if (file != nullptr) std::fflush(file);
    }
};

// Snapshot of the external sinks that costs no heap on the common path:
// zero to eight sinks (every engine, test and sample configuration) stay
// inline; only a host that registers more pays for a vector, and a logger
// with no external sink never builds one at all.
class SinkSnapshot {
  public:
    static constexpr size_t kInlineCapacity = 8;

    void Push(const ZHLN::LogSink sink) {
        if (_count < kInlineCapacity) {
            _inline[_count] = sink;
        } else {
            if (_spill.empty()) _spill.reserve(_count + 1);
            _spill.push_back(sink);
        }
        ++_count;
    }

    [[nodiscard]] auto Empty() const noexcept -> bool {
        return _count == 0;
    }

    template <typename Fn>
    void ForEach(Fn&& fn) const {
        const size_t inlineCount = _count < kInlineCapacity ? _count : kInlineCapacity;
        for (size_t i = 0; i < inlineCount; ++i) {
            fn(_inline[i]);
        }
        for (const auto& sink: _spill) {
            fn(sink);
        }
    }

  private:
    std::array<ZHLN::LogSink, kInlineCapacity> _inline {};
    std::vector<ZHLN::LogSink>                 _spill;
    size_t                                     _count = 0;
};

auto LoggerFilterThunk(const LogSeverity severity, void* userData) noexcept -> bool {
    const auto* logger = static_cast<const ZHLN::Logger*>(userData);
    return logger != nullptr && logger->IsEnabled(severity);
}

void LoggerWriteThunk(const LogSeverity severity, const LogSource source, const std::string_view message, void* userData) noexcept {
    if (const auto* logger = static_cast<const ZHLN::Logger*>(userData); logger != nullptr) {
        logger->Write(severity, source, message);
    }
}

void LoggerFlushThunk(void* userData) noexcept {
    if (const auto* logger = static_cast<const ZHLN::Logger*>(userData); logger != nullptr) {
        logger->Flush();
    }
}

} // namespace

namespace ZHLN {

struct Logger::Impl {
    mutable Mutex                           mutex;
    LogSeverity                             minimum = LogSeverity::Info;
    std::vector<ConsoleSink>                consoles;
    std::vector<FileSink>                   files;
    std::vector<std::pair<uint64_t, LogSink>> external;
    uint64_t                                nextToken = 1;
};

Logger::Logger(): _impl(std::make_unique<Impl>()) {
}

Logger::~Logger() = default;

auto Logger::IsEnabled(const LogSeverity severity) const noexcept -> bool {
    const std::scoped_lock lock(_impl->mutex);
    return severity != LogSeverity::Off && severity >= _impl->minimum;
}

void Logger::Write(const LogSeverity severity, const LogSource source, const std::string_view message) const noexcept {
    // Internal sinks never call back into this logger, so they are written
    // under the lock; external sinks are arbitrary consumer code (they may
    // add/remove sinks or flush), so they are snapshotted and invoked after
    // the lock is released.
    SinkSnapshot external;
    {
        const std::scoped_lock lock(_impl->mutex);
        if (severity < _impl->minimum) {
            return;
        }
        for (const auto& sink: _impl->consoles) {
            sink.Write(severity, source, message);
        }
        for (const auto& sink: _impl->files) {
            sink.Write(severity, source, message);
        }
        if (_impl->external.empty()) {
            return; // nothing forwards: no snapshot, no allocation
        }
        for (const auto& [token, sink]: _impl->external) {
            if (sink.write != nullptr) external.Push(sink);
        }
    }
    external.ForEach([&](const ZHLN::LogSink& sink) {
        if (sink.filter == nullptr || sink.filter(severity, sink.userData)) {
            sink.write(severity, source, message, sink.userData);
        }
    });
}

void Logger::Flush() const noexcept {
    SinkSnapshot external;
    {
        const std::scoped_lock lock(_impl->mutex);
        for (const auto& sink: _impl->files) {
            sink.Flush();
        }
        for (const auto& [token, sink]: _impl->external) {
            if (sink.flush != nullptr) external.Push(sink);
        }
    }
    external.ForEach([](const ZHLN::LogSink& sink) { sink.flush(sink.userData); });
    std::fflush(stderr);
    std::fflush(stdout);
}

void Logger::SetMinimumSeverity(const LogSeverity minimum) noexcept {
    const std::scoped_lock lock(_impl->mutex);
    _impl->minimum = minimum;
}

auto Logger::MinimumSeverity() const noexcept -> LogSeverity {
    const std::scoped_lock lock(_impl->mutex);
    return _impl->minimum;
}

void Logger::AddConsoleSink(const bool colorize, const bool useStdErr) {
    const std::scoped_lock lock(_impl->mutex);
    _impl->consoles.push_back(ConsoleSink {.colorize = colorize, .useStdErr = useStdErr});
}

void Logger::AddFileSink(FILE* file, const bool colorize) {
    if (file == nullptr) {
        return;
    }
    const std::scoped_lock lock(_impl->mutex);
    FileSink               sink;
    sink.file = file;
    (void) colorize; // files are never colorized; kept for call-site symmetry
    _impl->files.push_back(std::move(sink));
}

auto Logger::AddExternalSink(const LogSink sink) noexcept -> uint64_t {
    const std::scoped_lock lock(_impl->mutex);
    const uint64_t         token = _impl->nextToken++;
    _impl->external.emplace_back(token, sink);
    return token;
}

bool Logger::RemoveExternalSink(const uint64_t token) noexcept {
    const std::scoped_lock lock(_impl->mutex);
    return std::erase_if(_impl->external, [&](const auto& entry) { return entry.first == token; }) != 0;
}

auto Logger::AsLogSink() const noexcept -> LogSink {
    // Write()/IsEnabled()/Flush() are const, so handing out `this` through
    // the Layer-0 sink's void* userData adds no mutability: the thunks cast
    // straight back to a const Logger*.
    return LogSink {
        .filter   = &LoggerFilterThunk,
        .write    = &LoggerWriteThunk,
        .flush    = &LoggerFlushThunk,
        .userData = const_cast<Logger*>(this),
    };
}

} // namespace ZHLN


namespace ZHLN {

namespace {
// Default minimum severity for the no-sink path (stderr). The engine-sink
// path defers to the sink's own filter instead.
std::atomic<LogSeverity> g_minSeverity {LogSeverity::Info};

// Installed once per engine/session at most, read on every log call. The
// mutex (rather than separate atomics per field) keeps filter/write/flush/
// userData a consistent set without allocating; the lock is never held
// across the sink call itself, so a sink may log from inside its own Write.
std::mutex g_sinkMutex;
LogSink    g_sink;

auto SeverityName(const LogSeverity severity) noexcept -> const char* {
    switch (severity) {
        case LogSeverity::Debug: return "DBG";
        case LogSeverity::Info: return "INF";
        case LogSeverity::Warning: return "WRN";
        case LogSeverity::Error: return "ERR";
        case LogSeverity::Off: return "OFF";
    }
    return "???";
}
} // namespace

void SetLogSink(const LogSink sink) noexcept {
    const std::lock_guard lock(g_sinkMutex);
    g_sink = sink;
}

auto GetLogSink() noexcept -> LogSink {
    const std::lock_guard lock(g_sinkMutex);
    return g_sink;
}

void SetLogLevel(const LogSeverity minimum) noexcept {
    g_minSeverity.store(minimum, std::memory_order::release);
}

auto AmbientLogEnabled(const LogSeverity severity) noexcept -> bool {
    const LogSink sink = GetLogSink();
    if (sink.write != nullptr && sink.filter != nullptr) {
        return sink.filter(severity, sink.userData);
    }
    // No gated sink installed: gated only by the process minimum severity.
    return severity != LogSeverity::Off && severity >= g_minSeverity.load(std::memory_order::acquire);
}

void WriteAmbientLog(const LogSeverity severity, const LogSource source, const std::string_view message) noexcept {
    const LogSink sink = GetLogSink();
    if (sink.write != nullptr) {
        sink.write(severity, source, message, sink.userData);
        return;
    }
    // Default sink: stderr, one line, severity-tagged -- logging must never
    // be silently dropped just because no host installed anything.
    std::fprintf(stderr, "[%s] %.*s\n", SeverityName(severity), static_cast<int>(message.size()), message.data());
}

} // namespace ZHLN
