// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <print>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace ZHLN {
bool LoadRenderDocLibrary() noexcept {
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
