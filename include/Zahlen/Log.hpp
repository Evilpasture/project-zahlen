#pragma once

#include <cstdio>
#include <format>
#include <print>
#include <source_location>
#include <string_view>
#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <execinfo.h>
#endif

namespace ZHLN {

inline auto GetPoorMansStacktrace() -> std::string {
	std::string out = "";
#if defined(__APPLE__) || defined(__linux__)
	void* callstack[128];
	int frames = backtrace(callstack, 128);
	char** strs = backtrace_symbols(callstack, frames);

	for (int i = 0; i < frames; ++i) {
		// backtrace_symbols gives us strings like:
		// 0   zahlen   0x0000000100003f44 _Z5Panicv + 20
		// We can try to demangle if it looks like a C++ symbol
		out += strs[i];
		out += "\n";
	}
	std::free(strs);
#else
	out = "Stacktrace not implemented for this OS.\n";
#endif
	return out;
}

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

	// For runtime labels (like from a macro)
	constexpr LogContext(std::string_view s,
						 std::source_location l = std::source_location::current())
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
 * @brief Log a fatal error and terminate immediately.
 * Use this for unrecoverable state (e.g., Vulkan device lost).
 */
template <typename... Args> [[noreturn]] void Panic(LogContext ctx, Args&&... args) {
	// Log the actual error message
	Log(ctx, std::forward<Args>(args)...);

	// Use std::println to stderr (standard in C++23)
	std::println(stderr, "Stack Trace:\n{}", GetPoorMansStacktrace());

	std::abort();
}

struct DumpOptions {
	size_t bytes_per_line = 4;
	bool interpret_as_float = true;
};

#if defined(__clang__)
template <typename T> inline void TraceStruct(const T& obj) {
	// Clang's magic intrinsic that prints member names and values
	// It requires a printf-style callback
	__builtin_dump_struct(&obj, [](const char* fmt, ...) {
		va_list args;
		va_start(args, fmt);
		std::vprintf(fmt, args);
		va_end(args);
		return 0;
	});
}
#endif

inline void MemoryDump(const void* ptr, size_t size, std::string_view label,
					   LogContext ctx, // Move this to after the mandatory args
					   DumpOptions opts = {}) {
	const uint8_t* byte_ptr = static_cast<const uint8_t*>(ptr);

	std::println(stderr, "[{}:{}] Dumping '{}' at {}", ctx.loc.file_name(), ctx.loc.line(), label,
				 ptr);

	for (size_t i = 0; i < size; i += opts.bytes_per_line) {
		// 1. Print Address Offset
		std::print(stderr, "{:#010x} | ", reinterpret_cast<uintptr_t>(byte_ptr + i));

		// 2. Print Hex Bytes
		for (size_t j = 0; j < opts.bytes_per_line; ++j) {
			if (i + j < size)
				std::print(stderr, "{:02X} ", byte_ptr[i + j]);
			else
				std::print(stderr, "   ");
		}

		// 3. Optional Type Interpretation (e.g., Float)
		if (opts.interpret_as_float && i + 4 <= size) {
			float val;
			std::memcpy(&val, byte_ptr + i, 4);
			std::print(stderr, "| [float: {:>8.3f}] ", val);
		}

		// 4. Label (Only on the first line of the object)
		if (i == 0)
			std::print(stderr, " <-- {}", label);

		std::println(stderr, "");
	}
}

#define ZHLN_DUMP(var) ZHLN::MemoryDump(&var, sizeof(var), #var, #var)

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