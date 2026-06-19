// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/main.cpp
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Log.hpp"

#include <expected>
#include <span>

using namespace ZHLN;

int RunGame(const CommandLineOptions& options);
int RunEditor(const CommandLineOptions& options);
namespace ZHLN {
bool LoadRenderDocLibrary() noexcept;
} // namespace ZHLN
int main(int argc, char* argv[]) {
	return HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
		.and_then([](const CommandLineOptions& options) -> std::expected<int, EngineError> {
			ZHLN::SetLogLevel(options.logLevel);

			// Load RenderDoc before starting any windowing or rendering modules
			if (options.enableRenderDoc) {
				ZHLN::LoadRenderDocLibrary();
			}

			return options.launchEditor ? RunEditor(options) : RunGame(options);
		})
		.transform_error([](const EngineError& err) -> int {
			if (!err.msg.empty() && !err.silent) {
				ZHLN::Log("Error: {}", err.msg);
			}
			return err.code;
		})
		.value_or(0);
}
