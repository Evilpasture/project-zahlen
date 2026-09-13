// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Log.cpp
//
// Log levels, file routing and console printing -- the half of the old
// AssertHandler.cpp that has nothing to do with crashing. Everything here runs
// on the normal path, so unlike the diagnostics/ units it is allowed to
// allocate: std::println and std::format are the right tools when the process
// is healthy.
//
// The one exception is Diagnostics::WriteToChannel, which lives here because it
// writes to the same sinks but is defined for the crash path: raw descriptor
// writes, no formatting, no allocation. It sits beside GetCustomLogFile because
// that is the log sink it needs, and the diagnostics units call it rather than
// each growing their own writer.

#include "diagnostics/DiagnosticsInternal.hpp"
#include <Zahlen/Core/Platform.hpp> // windows.h on Windows, unistd.h on Unix
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Thread.hpp> // GetCurrentFiberID()
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>

namespace ZHLN {

namespace {

std::atomic<LogLevel> s_LogLevel {LogLevel::Moderate};

} // namespace

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

void InternalWriteLog(uint8_t channel, const char* file, uint32_t line, std::string_view message) {
    // Guard against Quiet mode
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

    std::println(outStream, "[{}:{}] [Fiber:{}] {}", file_name, line, fiberTag, message);
}

// Note: Emergency panic / crash dumps are kept unfiltered to preserve crash visibility.
[[noreturn]] void InternalPanic(const char* file, uint32_t line, std::string_view message) {
    // Force enable output for catastrophic crashes
    s_LogLevel.store(LogLevel::Verbose, std::memory_order::release);
    InternalWriteLog(static_cast<uint8_t>(LogChannel::StdErr), file, line, message);
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

} // namespace ZHLN

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

} // namespace ZHLN::Diagnostics
