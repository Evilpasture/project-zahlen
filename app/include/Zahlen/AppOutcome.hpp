// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Host-level outcomes, shared by ZHLN::App and bespoke host tools.
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN {

enum class AppExitReason : uint8_t {
    UserClosed,          // The primary host was closed by the user, frame limit, or capture.
    CliHelpRequested,    // --help was printed before host creation.
    CliVersionRequested, // --version was printed before host creation.
    CliGraphRequested,   // --print-graph was printed before host creation.
};

// Use AppError only where a lower layer has no ErrorCode to return. CLI
// parsing, Engine creation, frame capture, and plugin factories preserve their
// original (more specific) ErrorCode categories instead of erasing the cause.
enum class AppError : uint8_t {
    InvalidArguments ZHLN_ANNOTATION(ZHLN::Description<"Invalid argc or argv passed to the application"> {}) = 1,
    SceneInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Scene initialization failed"> {}),
    FrameTickFailed ZHLN_ANNOTATION(ZHLN::Description<"Engine tick failed during frame execution"> {}),
    PluginNotFound ZHLN_ANNOTATION(ZHLN::Description<"Plugin shared library could not be found"> {}),
    PluginLoadFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to open plugin shared library"> {}),
    PluginSymbolMissing ZHLN_ANNOTATION(ZHLN::Description<"Plugin shared library does not export ZHLN::COM::CreateAppLayer"> {}),
    PluginAbiMismatch ZHLN_ANNOTATION(ZHLN::Description<"Plugin factory ABI witness is missing or incompatible"> {}),
};

using AppRunResult = std::expected<AppExitReason, ErrorCode>;

} // namespace ZHLN
