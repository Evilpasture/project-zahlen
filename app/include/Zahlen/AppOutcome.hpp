// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Host-level outcomes, shared by the Engine application and Kernel-only tools.
// No OS exit code crosses this boundary; executable entrypoints translate it.
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN {

enum class AppExitReason : uint8_t {
    UserClosed,          // The primary host was closed by the user or application.
    CliHelpRequested,    // --help was printed before host creation.
    CliVersionRequested, // --version was printed before host creation.
    CliGraphRequested,   // --print-graph was printed before host creation.
};

// Use AppError only where a lower layer has no ErrorCode to return. CLI
// parsing, Engine/Kernel creation and device recovery preserve their original
// (more specific) ErrorCode categories instead of erasing the underlying cause.
enum class AppError : uint8_t {
    InvalidArguments ZHLN_ANNOTATION(ZHLN::Description<"Invalid argc or argv passed to the application"> {}) = 1,
    SceneInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Default scene initialization failed"> {}),
    FrameTickFailed ZHLN_ANNOTATION(ZHLN::Description<"Engine tick failed during frame execution"> {}),
};

using AppRunResult = std::expected<AppExitReason, ErrorCode>;

} // namespace ZHLN
