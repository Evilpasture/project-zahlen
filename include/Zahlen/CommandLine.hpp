// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdlib>
#include <expected>
#include <span>
#include <string>

namespace ZHLN {

struct CommandLineOptions {
	std::span<char* const> args;
	bool enableValidation = true;
	bool launchEditor = false;
};

struct EngineError {
	std::string msg;
	int code = EXIT_FAILURE;
	bool silent = false;
};

std::expected<CommandLineOptions, EngineError> HandleCommandLine(std::span<char* const> args);

} // namespace ZHLN
