#pragma once

#include <cstdio>
#include <format>
#include <print>
#include <source_location>
#include <string_view>

namespace ZHLN {

/**
 * @brief Captures both the format string and the caller's source location.
 * Being a non-template struct prevents the deduction failures you encountered.
 */
struct LogContext {
	std::string_view fmt;
	std::source_location loc;

	// This constructor captures the location at the call-site
	template <typename T>
	consteval LogContext(const T& s, std::source_location l = std::source_location::current())
		: fmt(s), loc(l) {}
};

/**
 * @brief Modern C++23 Engine Logger
 * Usage: ZHLN::Log("Player {} moved to {}", name, position);
 */
template <typename... Args> void Log(LogContext ctx, Args&&... args) {
	// Extract filename from path
	std::string_view file = ctx.loc.file_name();
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos)
		file.remove_prefix(pos + 1);

	// Use vformat to handle the type-erased arguments
	// std::println to stderr is the modern C++23 way
	std::println(stderr, "[{}:{}] {}", file, ctx.loc.line(),
				 std::vformat(ctx.fmt, std::make_format_args(args...)));
}

/**
 * @brief The bridge for Jolt Physics (C-Style variadic)
 */
auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void;

/**
 * @brief The bridge for Jolt Physics Asserts
 */
auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
					  uint32_t inLine) noexcept -> bool;

} // namespace ZHLN