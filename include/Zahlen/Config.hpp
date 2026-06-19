// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <bit>
#include <cstdint>
#include <detail/String.hpp>
#include <string_view>
#include <version>

#define ZHLN_VERSION_MAJOR 0
#define ZHLN_VERSION_MINOR 1
#define ZHLN_VERSION_PATCH 0

// 1. The "Stringize" helper macros
#define ZHLN_STR_HELPER(x) #x
#define ZHLN_STR(x) ZHLN_STR_HELPER(x)

// 2. Combine them using string literal concatenation
#define ZHLN_VERSION_STR                                                                           \
	ZHLN_STR(ZHLN_VERSION_MAJOR) "." ZHLN_STR(ZHLN_VERSION_MINOR) "." ZHLN_STR(ZHLN_VERSION_PATCH)

namespace ZHLN {

constexpr std::string_view GetSTLVersion() noexcept {
#if defined(_LIBCPP_VERSION)
	// LLVM libc++
	return "LLVM libc++ " ZHLN_STR(_LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
// GNU libstdc++
#if defined(_GLIBCXX_RELEASE)
	return "GNU libstdc++ " ZHLN_STR(_GLIBCXX_RELEASE) " (Date: " ZHLN_STR(__GLIBCXX__) ")";
#else
	return "GNU libstdc++ (Date: " ZHLN_STR(__GLIBCXX__) ")";
#endif
#elif defined(_MSVC_STL_VERSION)
	// MSVC STL
	return "MSVC STL " ZHLN_STR(_MSVC_STL_VERSION);
#else
	return "Unknown Standard Library";
#endif
}

struct Version {
	uint32_t major;
	uint32_t minor;
	uint32_t patch;
	static constexpr std::string_view String = ZHLN_VERSION_STR;
};

inline constexpr Version EngineVersion{
	.major = ZHLN_VERSION_MAJOR, .minor = ZHLN_VERSION_MINOR, .patch = ZHLN_VERSION_PATCH};

#if defined(__clang__)
inline constexpr std::string_view Compiler = "Clang (" __VERSION__ ")";
#elif defined(__GNUC__)
inline constexpr std::string_view Compiler = "GCC (" __VERSION__ ")";
#elif defined(_MSC_VER)
inline constexpr std::string_view Compiler = "MSVC";
#else
inline constexpr std::string_view Compiler = "Unknown Compiler";
#endif

#if defined(NDEBUG)
inline constexpr std::string_view BuildType = "Release";
#else
inline constexpr std::string_view BuildType = "Debug";
#endif

#if defined(ZHLN_DEV_MODE)
inline constexpr bool isDev = true;
#else
inline constexpr bool isDev = false;
#endif

#if defined(__ASAN_ENABLED__)
inline constexpr std::string_view Sanitizers = "enabled";
#else
inline constexpr std::string_view Sanitizers = "disabled";
#endif

// --- PLATFORM DETECTION ---
#if defined(_WIN32) || defined(_WIN64)
inline constexpr std::string_view PlatformName = "Windows";
inline constexpr bool isWindows = true;
inline constexpr bool isLinux = false;
inline constexpr bool isMac = false;
#elif defined(__linux__) && !defined(__ANDROID__)
inline constexpr std::string_view PlatformName = "Linux";
inline constexpr bool isWindows = false;
inline constexpr bool isLinux = true;
inline constexpr bool isMac = false;
#elif defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#if TARGET_OS_MAC && !TARGET_OS_IPHONE
inline constexpr std::string_view PlatformName = "macOS";
inline constexpr bool isWindows = false;
inline constexpr bool isLinux = false;
inline constexpr bool isMac = true;
#else
inline constexpr std::string_view PlatformName = "Apple iOS/Other";
inline constexpr bool isWindows = false;
inline constexpr bool isLinux = false;
inline constexpr bool isMac = false;
#endif
#else
inline constexpr std::string_view PlatformName = "Unknown Platform";
inline constexpr bool isWindows = false;
inline constexpr bool isLinux = false;
inline constexpr bool isMac = false;
#endif

// --- ARCHITECTURE DETECTION ---
#if defined(__x86_64__) || defined(_M_X64)
inline constexpr std::string_view Architecture = "x86_64";
inline constexpr bool isX64 = true;
inline constexpr bool isARM64 = false;
#elif defined(__aarch64__) || defined(_M_ARM64)
inline constexpr std::string_view Architecture = "ARM64";
inline constexpr bool isX64 = false;
inline constexpr bool isARM64 = true;
#else
inline constexpr std::string_view Architecture = "Unknown Arch";
inline constexpr bool isX64 = false;
inline constexpr bool isARM64 = false;
#endif

#if defined(__x86_64__) || defined(_M_X64)
// Virtually all modern x86_64 CPUs use 64-byte cache lines
inline constexpr size_t CacheLineSize = 64;
#elif defined(__aarch64__) || defined(_M_ARM64)
// Apple Silicon (M1/M2/M3) uses 128-byte cache lines for performance cores
#if defined(__APPLE__)
inline constexpr size_t CacheLineSize = 128;
#else
inline constexpr size_t CacheLineSize = 64;
#endif
#else
inline constexpr size_t CacheLineSize = 64; // Safe fallback
#endif

inline constexpr bool isLittleEndian = (std::endian::native == std::endian::little);
inline constexpr bool isBigEndian = (std::endian::native == std::endian::big);

// Check if the compiler supports a standardized debug break hook
inline void DebugBreak() noexcept {
#if defined(_WIN32) || defined(_WIN64)
// We are strictly on Windows
#if defined(_MSC_VER) || defined(__clang__)
	__debugbreak();
#endif
#elif defined(__linux__)
// We are strictly on Linux
#if defined(__GNUC__) || defined(__clang__)
	__builtin_trap();
#endif
#elif defined(__APPLE__)
// We are strictly on macOS
#if defined(__GNUC__) || defined(__clang__)
	__builtin_trap();
#endif
#endif
}

struct PhysicsConfig {
	uint32_t maxBodies = 1024;
	uint32_t maxBodyPairs = 1024;
	uint32_t maxContactConstraints = 1024;
	uint32_t tempAllocatorSize = 32 * 1024 * 1024; // 32MB
};

struct RenderConfig {
	String64 appName;
	uint32_t width = 1280;
	uint32_t height = 720;
	bool vsync = true;
	bool fullscreen = false;
	bool enableValidation = true;
};

struct EngineConfig {
	PhysicsConfig physics;
	RenderConfig render;
};

} // namespace ZHLN
