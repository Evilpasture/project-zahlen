// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/CommandLine.hpp"

#include "Zahlen/Config.hpp"

#include <expected>
#include <filesystem>
#include <print>
#include <span>
namespace {
enum class ArgType : uint8_t { Unknown, Editor, Version, Help, NoValidation };

// Simple helper to map strings to the enum
constexpr ArgType GetArgType(std::string_view arg) {
	if (arg == "--editor") {
		return ArgType::Editor;
	}
	if (arg == "--version") {
		return ArgType::Version;
	}
	if (arg == "--help" || arg == "-h") {
		return ArgType::Help;
	}
	if (arg == "--no-validation") {
		return ArgType::NoValidation;
	}
	return ArgType::Unknown;
}
} // namespace

namespace ZHLN {
std::expected<CommandLineOptions, EngineError> HandleCommandLine(std::span<char* const> args) {
	CommandLineOptions options{.args = args, .enableValidation = true, .launchEditor = false};

	// Check environment variables first (allows easy IDE configuration profiles)
	if (const char* envVal = std::getenv("ZHLN_VALIDATION")) {
		if (std::string_view(envVal) == "0" || std::string_view(envVal) == "false") {
			options.enableValidation = false;
		}
	}

	for (std::string_view arg : args.subspan(1)) {
		switch (GetArgType(arg)) {
			case ArgType::Editor:
				options.launchEditor = true;
				break;

			case ArgType::Version:
				std::println("Zahlen Engine - version {}.{}.{}", EngineVersion.major,
							 EngineVersion.minor, EngineVersion.patch);
				std::println("Built on:      {} (UTC)", __DATE__);
				std::println("Build Profile: {} | Sanitizers: {}", BuildType, Sanitizers);
				std::println("Compiler:      {}", Compiler);

				std::println("\nLicense GPLv3+: GNU GPL version 3 or later "
							 "<https://gnu.org/licenses/gpl.html>.");
				std::println("This is free software: you are free to change and redistribute it.");
				std::println("There is NO WARRANTY, to the extent permitted by law.");

				return std::unexpected(
					EngineError{.msg = {}, .code = EXIT_SUCCESS, .silent = true});
			case ArgType::Help: {
				// Evaluated at compile-time, zero runtime overhead
				static constexpr std::string_view HelpMenu =
					R"(
Usage: {} [options]

Options:
  -h, --help           Display this help menu and exit
  --version            Display engine version information and exit
  --editor             Launch the world editor instead of the game
  --no-validation      Disable Vulkan validation layers completely

Environment Variables:
  ZHLN_VALIDATION=0    Disable Vulkan validation layers

)";

				std::string exeName = "zahlen";

				if (!args.empty() && args[0] != nullptr) {
					exeName = std::filesystem::path(args[0]).filename().string();
				}

				std::print(HelpMenu, exeName);
				return std::unexpected(
					EngineError{.msg = {}, .code = EXIT_SUCCESS, .silent = true});
			}
			case ArgType::NoValidation:
				options.enableValidation = false;
				break;
			case ArgType::Unknown:
				// Optional: Handle or ignore unknown flags
				break;
		}
	}
	return options;
}
} // namespace ZHLN
