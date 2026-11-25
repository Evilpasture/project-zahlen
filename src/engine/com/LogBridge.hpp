// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// src/engine/com/LogBridge.hpp  (engine-private)
//
// The one place allowed to know both worlds -- rule 2 of
// configure/check_com_boundary.py puts it here, next to the other adapters:
//   * ToSink(ILogger*) publishes a COM logger as the Layer-0 ambient sink, so
//     the kernel below COM (KernelSession, LogicWorld, the frame loop) forwards
//     lines without naming ILogger;
//   * InstallAmbientSink(ILogger*) does that and returns the sink it replaced.
// Header-inline and allocation-free: the sink is four words over a function
// pointer set, so nothing is added to the engine's export surface.

#include <Zahlen/COM/ILogger.hpp>
#include <Zahlen/Log.hpp>

#include <string_view>

namespace ZHLN::LogBridge {

// The logger must outlive the sink; the engine holds a Ref<ILogger> for its
// whole life, which is what guarantees that.
//
// The three thunks are non-generic captureless noexcept lambdas, so each one
// converts to its plain function pointer at compile time: no namespace to hide
// them in, no exported symbol, nothing added to the ABI. Nothing compares these
// pointers for equality (only against nullptr), so being inline in a header
// several engine TUs include is ODR-safe.
[[nodiscard]] inline auto ToSink(COM::ILogger* logger) noexcept -> LogSink {
    if (logger == nullptr) {
        return {};
    }
    return LogSink {
        .filter = [](const LogSeverity severity, void* const userData) noexcept -> bool {
            const auto* target = static_cast<const COM::ILogger*>(userData);
            return target != nullptr && target->IsEnabled(static_cast<COM::LogSeverity>(severity));
        },
        .write = [](const LogSeverity severity, const LogSource source, const std::string_view message, void* const userData) noexcept -> void {
            if (const auto* target = static_cast<const COM::ILogger*>(userData); target != nullptr) {
                const COM::LogSource wire {
                    .file     = COM::StringView(source.file),
                    .function = COM::StringView(source.function),
                    .channel  = COM::StringView(source.channel),
                    .line     = source.line,
                };
                target->Write(static_cast<COM::LogSeverity>(severity), wire, COM::StringView(message.data(), message.size()));
            }
        },
        .flush = [](void* const userData) noexcept -> void {
            if (const auto* target = static_cast<const COM::ILogger*>(userData); target != nullptr) {
                target->Flush();
            }
        },
        .userData = logger,
    };
}

[[nodiscard]] inline auto InstallAmbientSink(COM::ILogger* logger) noexcept -> LogSink {
    const LogSink previous = GetLogSink();
    SetLogSink(ToSink(logger));
    return previous;
}

} // namespace ZHLN::LogBridge
