// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Log.hpp"

#include <expected>
#include <span>

using namespace ZHLN;

int RunGame(const CommandLineOptions& options);
int RunEditor(const CommandLineOptions& options);

int main(int argc, char* argv[]) {
	return HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
		.and_then([](const CommandLineOptions& options) -> std::expected<int, EngineError> {
			ZHLN::SetLogLevel(options.logLevel);
			return options.launchEditor ? RunEditor(options) : RunGame(options);
		})
		.transform_error([](const EngineError& err) -> int {
			if (!err.msg.empty() && !err.silent) {
				ZHLN::Log("Error: {}", err.msg);
			}
			return err.code;
		})
		.value_or(0); // value_or handles the final extraction safely
}
