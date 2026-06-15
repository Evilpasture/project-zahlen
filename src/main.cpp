// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/main.cpp
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Log.hpp"

#include <expected>
#include <span>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif
#include <print>

using namespace ZHLN;

int RunGame(const CommandLineOptions& options);
int RunEditor(const CommandLineOptions& options);

namespace ZHLN {
inline bool LoadRenderDocLibrary() noexcept {
#if defined(_WIN32)
	// Try the standard system search path first
	HMODULE mod = LoadLibraryA("renderdoc.dll");
	if (!mod) {
		// Fallback to the default installation directory
		mod = LoadLibraryA("C:\\Program Files\\RenderDoc\\renderdoc.dll");
	}
	if (mod) {
		std::println("[RenderDoc] Core library loaded successfully.");
		return true;
	}
#elif defined(__linux__)
	// Try standard search path
	void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_GLOBAL);
	if (mod == nullptr) {
		// Fallback to common library paths
		mod = dlopen("/usr/lib/librenderdoc.so", RTLD_NOW | RTLD_GLOBAL);
	}
	if (mod == nullptr) {
		mod = dlopen("/usr/lib/x86_64-linux-gnu/librenderdoc.so", RTLD_NOW | RTLD_GLOBAL);
	}
	if (mod != nullptr) {
		std::println("[RenderDoc] Core library loaded successfully.");
		return true;
	}
#endif
	std::println(stderr, "[RenderDoc] WARNING: Failed to locate RenderDoc library. Is it installed "
						 "and in your system PATH?");
	return false;
}
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
