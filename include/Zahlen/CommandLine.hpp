// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <span>
#include <string>

namespace ZHLN {

enum class LogLevel : uint8_t { Quiet, Moderate, Verbose };

struct CommandLineOptions {
	std::span<char* const> args;
	bool enableValidation = true;
	bool launchEditor = false;
	bool vsync = true;
	bool fullscreen = false;
	LogLevel logLevel = LogLevel::Moderate;
	uint32_t fpsLimit = 0;
};

struct EngineError {
	std::string msg;
	int code = EXIT_FAILURE;
	bool silent = false;
};

std::expected<CommandLineOptions, EngineError> HandleCommandLine(std::span<char* const> args);

} // namespace ZHLN
