// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <detail/String.hpp>
#include <string_view>

#define ZHLN_VERSION_MAJOR 1
#define ZHLN_VERSION_MINOR 0
#define ZHLN_VERSION_PATCH 0

// 1. The "Stringize" helper macros
#define ZHLN_STR_HELPER(x) #x
#define ZHLN_STR(x) ZHLN_STR_HELPER(x)

// 2. Combine them using string literal concatenation
#define ZHLN_VERSION_STR                                                                           \
	ZHLN_STR(ZHLN_VERSION_MAJOR) "." ZHLN_STR(ZHLN_VERSION_MINOR) "." ZHLN_STR(ZHLN_VERSION_PATCH)

namespace ZHLN {

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

#if defined(__ASAN_ENABLED__)
inline constexpr std::string_view Sanitizers = "enabled";
#else
inline constexpr std::string_view Sanitizers = "disabled";
#endif

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
	bool enableValidation = true;
};

struct EngineConfig {
	PhysicsConfig physics;
	RenderConfig render;
};

} // namespace ZHLN
