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

using ZHLN::LogSeverity;
using ZHLN::LogSource;

// Defined below, beside the sink that uses it; declared here because the console
// sink's Write() reaches it before that point in the file.
auto ShouldColorize(FILE* stream) noexcept -> bool;



struct ConsoleSink {
    bool colorize  = true;
    bool useStdErr = true;

    void Write(const LogSeverity severity, const LogSource source, const std::string_view message) const noexcept {
        FILE*                  stream = useStdErr ? stderr : stdout;
        const std::string      line   = ZHLN::FormatLogLine(severity, source, message, colorize && ShouldColorize(stream));
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
        const std::string line = ZHLN::FormatLogLine(severity, source, message, false);
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

namespace {

// Process-wide fallbacks, both of them only consulted when nothing better is
// installed:
//   g_minSeverity -- the minimum severity a freshly minted Logger starts at,
//                    and the gate the stderr fallback uses. SetLogLevel writes
//                    it; a logger given an explicit level (LoggerConfig) is not
//                    affected.
//   g_sink        -- the ambient sink (Zahlen/Log.hpp).
std::atomic<LogSeverity> g_minSeverity {LogSeverity::Info};

// Installed once per engine/session at most, read on every log call.
//
// Guarded by a mutex rather than published as a std::atomic<LogSink>: a sink is
// four pointers (32 bytes), which is wider than any lock-free CAS a target
// offers (16 bytes on x86-64/ARM64). GCC and Clang lower such an atomic to
// libatomic's __atomic_load/__atomic_store, which route through a hidden lock
// table -- a lock, at that point, that is both slower than this mutex and a link
// dependency (-latomic) that MinGW and some Linux toolchains do not add for you.
// A one-byte atomic (g_minSeverity) is genuinely lock-free everywhere and stays
// an atomic.
//
// The per-line cost is ONE acquisition, not three: the caller takes a single
// snapshot through GetLogSink(), gates on that snapshot's filter and writes
// through the same snapshot's writer. No lock is held while a sink runs, so a
// sink may log from inside its Write.
std::mutex g_sinkMutex;
LogSink    g_sink {};

} // namespace

using LogDetail::BaseName;
using LogDetail::kColorReset;
using LogDetail::SeverityColor;
using LogDetail::SeverityTag;

struct Logger::Impl {
    mutable Mutex                           mutex;
    LogSeverity                             minimum = LogSeverity::Info;
    std::vector<ConsoleSink>                consoles;
    std::vector<FileSink>                   files;
    std::vector<std::pair<uint64_t, LogSink>> external;
    uint64_t                                nextToken = 1;
};

Logger::Logger(): _impl(std::make_unique<Impl>()) {
    // A logger nobody gave an explicit level starts at the process default
    // (SetLogLevel). That is what makes a host's -q/-v useful for engines that
    // the factory mints for itself: the flag sets the default, and every logger
    // created afterwards inherits it instead of hard-coding Info.
    _impl->minimum = g_minSeverity.load(std::memory_order::acquire);
}

Logger::~Logger() = default;

auto Logger::IsEnabled(const LogSeverity severity) const noexcept -> bool {
    // One acquisition, and no foreign call under it: the answer is "would any
    // sink see this line?". The display sinks (console, file) honor the
    // logger's own minimum severity; an attached external sink is an observer
    // owned by the host -- a capture ring, a telemetry counter -- and is
    // delivered every line regardless of what the display is set to, so its
    // presence alone makes the line enabled. `Off` still means silence.
    if (severity == LogSeverity::Off) {
        return false;
    }
    const std::scoped_lock lock(_impl->mutex);
    return severity >= _impl->minimum || !_impl->external.empty();
}

void Logger::Write(const LogSeverity severity, const LogSource source, const std::string_view message) const noexcept {
    // Internal sinks never call back into this logger, so they are written
    // under the lock; external sinks are arbitrary consumer code (they may
    // add/remove sinks or flush), so they are snapshotted and invoked after
    // the lock is released.
    if (severity == LogSeverity::Off) {
        return;
    }
    SinkSnapshot external;
    {
        const std::scoped_lock lock(_impl->mutex);
        // Display sinks are gated by the logger's minimum severity; external
        // observers are not (see IsEnabled): silencing the console must not
        // blind a test's capture ring or a harness's validation counter.
        if (severity >= _impl->minimum) {
            for (const auto& sink: _impl->consoles) {
                sink.Write(severity, source, message);
            }
            for (const auto& sink: _impl->files) {
                sink.Write(severity, source, message);
            }
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

void SetLogSink(const LogSink sink) noexcept {
    const std::scoped_lock lock(g_sinkMutex);
    g_sink = sink;
}

auto GetLogSink() noexcept -> LogSink {
    const std::scoped_lock lock(g_sinkMutex);
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
        // The gate the caller consulted (AmbientLogEnabled) was read from the
        // same published value; a filter that changed its mind concurrently is
        // that sink's business, not a reason to drop the line here.
        sink.write(severity, source, message, sink.userData);
        return;
    }
    // Default sink: stderr, rendered by the one formatting pass, exactly like
    // any installed console sink -- logging must never be silently dropped just
    // because no host installed anything, and a line that survives must not
    // lose its file, fiber or severity on the way out.
    const std::string line = FormatLogLine(severity, source, message, false);
    std::fprintf(stderr, "%s\n", line.c_str());
}

} // namespace ZHLN
