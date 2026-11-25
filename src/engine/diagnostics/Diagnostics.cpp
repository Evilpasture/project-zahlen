// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiagnosticsInternal.hpp"
#include "tty/TTYBackend.hpp"
#include <Zahlen/Core/Diagnostics.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ZHLN {

namespace {

void WriteFd2(const std::string_view msg) noexcept {
#if defined(_WIN32)
    ::_write(2, msg.data(), static_cast<unsigned int>(msg.size()));
#else
    ::write(2, msg.data(), msg.size());
#endif
}

} // namespace

// Strictly allocation-free: when this runs, the heap, the logger, or user
// sinks may already be broken. Stack buffers and raw fd writes only.
[[noreturn]] void InternalPanic(const char* file, const uint32_t line, const std::string_view message) {
    Diagnostics::EmergencyRestoreTerminal();

    const std::string_view fileName = (file != nullptr) ? file : "unknown";
    const auto             basePos  = fileName.find_last_of("/\\");
    const std::string_view base     = (basePos != std::string_view::npos) ? fileName.substr(basePos + 1) : fileName;

    char header[1024];
    const int headerLen = std::snprintf(header, sizeof(header), "[%.*s:%u] [PANIC] ", static_cast<int>(base.size()), base.data(), line);
    if (headerLen > 0) {
        WriteFd2(std::string_view(header, static_cast<size_t>(headerLen)));
    }
    WriteFd2(message);
    WriteFd2("\nStack Trace:\n");

    std::array<char, 16384> stackBuf {};
    const size_t            written = Diagnostics::CaptureStackTrace(stackBuf);
    if (written != 0) {
        WriteFd2(std::string_view(stackBuf.data(), written));
    }
    std::abort();
}

} // namespace ZHLN

namespace ZHLN::Diagnostics {

void WriteToChannel(const LogTarget channel, const std::string_view msg) noexcept {
    switch (channel) {
        case LogTarget::StdOut:
#if defined(_WIN32)
            ::_write(1, msg.data(), static_cast<unsigned int>(msg.size()));
#else
            ::write(1, msg.data(), msg.size());
#endif
            return;
        case LogTarget::File:
            // No ambient log file exists anymore; file output belongs to a
            // FileSink on a logger hub. Diagnostics fall back to fd 2.
            break;
        case LogTarget::StdErr:
            break;
    }
    WriteFd2(msg);
}

void WriteDiagnosticError(const std::string_view msg) noexcept {
    WriteErr(msg);
}

void EmergencyRestoreTerminal() noexcept {
    TTYBackend::EmergencyRestore();
}

} // namespace ZHLN::Diagnostics
