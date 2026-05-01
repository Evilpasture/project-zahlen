#pragma once

#include <format>
#include <print>
#include <utility>

namespace ZHLN {

/**
 * @brief Modern C++23 Engine Logger
 * Usage: ZHLN::Log("Player {} moved to {}", name, position);
 */
template <typename... Args> void Log(std::format_string<Args...> fmt, Args&&... args) {
	std::print(fmt, std::forward<Args>(args)...);
}

/**
 * @brief The bridge for Jolt Physics (C-Style variadic)
 * This must maintain this signature to be assigned to JPH::Trace.
 */
auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void;

/**
 * @brief The bridge for Jolt Physics Asserts
 */
auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
					  uint32_t inLine) noexcept -> bool;

} // namespace ZHLN