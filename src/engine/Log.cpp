// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "diagnostics/DiagnosticsInternal.hpp"
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ZHLN {

#if defined(_WIN32)
// The one definition of the error-category registry's head, imported by every
// module that formats an Error. See the declaration in <Zahlen/ErrorCode.hpp>:
// PE does not merge the header-inline static across modules the way ELF's
// STB_GNU_UNIQUE does, so without this an Error built in libzahlen_engine.dll
// reads an empty list in zahlen.exe and prints "None".
auto TemplatedDetail::GetRegistryHead() noexcept -> std::atomic<TemplatedDetail::RegistryNode*>& {
    static std::atomic<TemplatedDetail::RegistryNode*> head {nullptr};
    return head;
}
#endif

namespace {

std::atomic<LogLevel> s_LogLevel {LogLevel::Moderate};

constexpr auto SeverityTag(LogSeverity severity) noexcept -> std::string_view {
    switch (severity) {
        case LogSeverity::Debug:   return "DEBUG";
        case LogSeverity::Info:    return "INFO";
        case LogSeverity::Warning: return "WARN";
        case LogSeverity::Error:   return "ERROR";
    }
    return "INFO";
}

constexpr auto SeverityColor(LogSeverity severity) noexcept -> const char* {
    switch (severity) {
        case LogSeverity::Debug:   return Color::Gray;
        case LogSeverity::Info:    return "";
        case LogSeverity::Warning: return Color::Yellow;
        case LogSeverity::Error:   return Color::Red;
    }
    return "";
}

auto ShouldColorize(FILE* stream) noexcept -> bool {
    // NO_COLOR (https://no-color.org/): present and non-empty disables color.
    if (const char* noColor = std::getenv("NO_COLOR"); (noColor != nullptr) && (noColor[0] != '\0')) {
        return false;
    }
    // Piped or redirected output stays plain so logs and captures never
    // contain escape sequences.
#if defined(_WIN32)
    return _isatty(_fileno(stream)) != 0;
#else
    return ::isatty(::fileno(stream)) != 0;
#endif
}

}

void SetLogLevel(LogLevel level) noexcept {
    s_LogLevel.store(level, std::memory_order::release);
}

auto GetLogLevel() noexcept -> LogLevel {
    return s_LogLevel.load(std::memory_order::acquire);
}

auto GetCustomLogFile(FILE* overrideFile) -> FILE* {
    static FILE* logFile = nullptr;
    if (overrideFile != nullptr) {
        if ((logFile != nullptr) && logFile != stdout && logFile != stderr) {
            std::fclose(logFile);
        }
        logFile = overrideFile;
    }
    if (logFile == nullptr) {
        logFile = std::fopen("zahlen_runtime.log", "w");
    }
    return logFile;
}

void InternalWriteLog(uint8_t channel, uint8_t severity, const char* file, uint32_t line, std::string_view message) {
    if (s_LogLevel.load(std::memory_order::acquire) == LogLevel::Quiet) {
        return;
    }

    std::string_view file_name = file;
    if (auto pos = file_name.find_last_of("/\\"); pos != std::string_view::npos) {
        file_name.remove_prefix(pos + 1);
    }

    uint64_t fid = GetCurrentFiberID();

    std::string fiberTag;
    if (fid == 0) {
        fiberTag = "Thread";
    } else if (fid == 1) {
        fiberTag = "Main";
    } else {
        fiberTag = std::format("{:#x}", fid);
    }

    FILE* outStream = nullptr;
    if (channel == static_cast<uint8_t>(LogChannel::StdOut)) {
        outStream = stdout;
    } else if (channel == static_cast<uint8_t>(LogChannel::File)) {
        outStream = GetCustomLogFile();
    } else {
        outStream = stderr;
    }

    const auto level = static_cast<LogSeverity>(severity);
    if (level == LogSeverity::Info) {
        std::println(outStream, "[{}:{}] [Fiber:{}] {}", file_name, line, fiberTag, message);
        return;
    }
    // The log file keeps the tag but never ANSI escapes, whatever the TTY.
    if ((channel == static_cast<uint8_t>(LogChannel::File)) || !ShouldColorize(outStream)) {
        std::println(outStream, "[{}:{}] [Fiber:{}] [{}] {}", file_name, line, fiberTag, SeverityTag(level), message);
        return;
    }
    std::println(
        outStream, "[{}:{}] [Fiber:{}] [{}{}{}] {}", file_name, line, fiberTag, SeverityColor(level), SeverityTag(level), Color::Reset, message
    );
}

[[noreturn]] void InternalPanic(const char* file, uint32_t line, std::string_view message) {
    s_LogLevel.store(LogLevel::Verbose, std::memory_order::release);
    InternalWriteLog(static_cast<uint8_t>(LogChannel::StdErr), static_cast<uint8_t>(LogSeverity::Error), file, line, message);
    std::println(stderr, "Stack Trace:\n{}", GetPoorMansStacktrace());
    std::abort();
}

void LogManual(std::string_view file, int line, std::string_view message, const char* color) {
    if (s_LogLevel.load(std::memory_order::acquire) == LogLevel::Quiet) {
        return;
    }

    uint64_t    fid      = GetCurrentFiberID();
    std::string fiberTag = (fid == 0) ? "Thread" : (fid == 1) ? "Main" : std::format("{:#x}", fid);

    if (color[0] != '\0') {
        std::println(stderr, "{}[{}:{}] [Fiber:{}] [LUA] {}{}", color, file, line, fiberTag, message, Color::Reset);
    } else {
        std::println(stderr, "[{}:{}] [Fiber:{}] [LUA] {}", file, line, fiberTag, message);
    }
}

}

namespace ZHLN::Diagnostics {

void WriteToChannel(uint8_t channel, std::string_view msg) noexcept {
    if (channel == static_cast<uint8_t>(LogChannel::StdOut)) {
#if defined(_WIN32)
        ::_write(1, msg.data(), static_cast<unsigned int>(msg.size()));
#else
        ::write(1, msg.data(), msg.size());
#endif
    } else if (channel == static_cast<uint8_t>(LogChannel::File)) {
        FILE* f = ZHLN::GetCustomLogFile();
        if (f != nullptr) {
            std::fwrite(msg.data(), 1, msg.size(), f);
            std::fflush(f);
        }
    } else {
#if defined(_WIN32)
        ::_write(2, msg.data(), static_cast<unsigned int>(msg.size()));
#else
        ::write(2, msg.data(), msg.size());
#endif
    }
}

}
