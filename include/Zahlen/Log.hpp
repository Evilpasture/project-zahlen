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
		std::string line = strs[i];

		// Try to find the mangled name in the string
		// macOS format: "index  binary  address  mangled_name + offset"
		size_t name_start = line.find("_Z");
		size_t name_end = line.find(" + ", name_start);

		if (name_start != std::string::npos && name_end != std::string::npos) {
			std::string mangled = line.substr(name_start, name_end - name_start);
			int status = 0;
			char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);

			if (status == 0) {
				out += line.substr(0, name_start) + demangled + line.substr(name_end) + "\n";
			} else {
				out += line + "\n";
			}
			std::free(demangled);
		} else {
			out += line + "\n";
		}
	}
	std::free(strs);
#else
	out = "Stacktrace not implemented.\n";
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
	size_t bytes_per_line = 16; // Standard hex dump width
	bool show_ascii = true;		// Show readable characters
	bool show_interpret = true; // Show float/int guesses
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

// ANSI Color Helpers
namespace Color {
inline const char* Reset = "\033[0m";
inline const char* Gray = "\033[90m";
inline const char* Cyan = "\033[36m";
inline const char* Yellow = "\033[33m";
inline const char* Green = "\033[32m";
inline const char* Red = "\033[31m";
} // namespace Color

inline void MemoryDump(const void* ptr, size_t size, std::string_view label, LogContext ctx,
					   DumpOptions opts = {}) {
	const uint8_t* byte_ptr = static_cast<const uint8_t*>(ptr);

	std::println(stderr, "{}┌─── DUMP: {} ({}) ───{}", Color::Cyan, label, ctx.fmt, Color::Reset);
	std::println(stderr, "│ Source:  {}:{}", ctx.loc.file_name(), ctx.loc.line());
	std::println(stderr, "│ Address: {}{}{} ({} bytes)", Color::Yellow, ptr, Color::Reset, size);
	std::println(stderr, "├────────────┬────────────────────────────────────────────────┬──────────"
						 "────────┬─────────────────────────┐");
	std::println(stderr, "│  Address   │ Hex Data                                       │ ASCII    "
						 "        │ Interpretation          │");
	std::println(stderr, "├────────────┼────────────────────────────────────────────────┼──────────"
						 "────────┼─────────────────────────┤");

	for (size_t i = 0; i < size; i += opts.bytes_per_line) {
		// 1. Address
		std::print(stderr, "│ {}{:#010x}{} │ ", Color::Cyan,
				   reinterpret_cast<uintptr_t>(byte_ptr + i), Color::Reset);

		// 2. Hex Data (with color for non-zero)
		for (size_t j = 0; j < opts.bytes_per_line; ++j) {
			if (i + j < size) {
				uint8_t b = byte_ptr[i + j];
				if (b == 0)
					std::print(stderr, "{}00{} ", Color::Gray, Color::Reset);
				else
					std::print(stderr, "{:02X} ", b);
			} else {
				std::print(stderr, "   ");
			}
			if ((j + 1) % 4 == 0 && j + 1 < opts.bytes_per_line)
				std::print(stderr, " ");
		}

		// 3. ASCII
		std::print(stderr, "│ ");
		for (size_t j = 0; j < opts.bytes_per_line; ++j) {
			if (i + j < size) {
				uint8_t c = byte_ptr[i + j];
				if (std::isprint(c))
					std::print(stderr, "{}", (char)c);
				else
					std::print(stderr, "{}·{}", Color::Gray, Color::Reset);
			} else
				std::print(stderr, " ");
		}

		// 4. Smart Interpretation
		std::print(stderr, " │ ");
		if (i + 8 <= size) { // Check for 64-bit Pointer/Long first
			uint64_t val64;
			std::memcpy(&val64, byte_ptr + i, 8);

			// Heuristic: Does it look like a 64-bit pointer?
			// On macOS/Linux, pointers usually start with 0x00007 or 0x00000001
			if (val64 > 0x100000000 && val64 < 0x00007FFFFFFFFFFF) {
				std::print(stderr, "{}ptr: {:#014x}{}", Color::Green, val64, Color::Reset);
			} else {
				// Fallback to 32-bit float/int logic
				float f32;
				std::memcpy(&f32, byte_ptr + i, 4);
				if (!std::isnan(f32) && std::abs(f32) > 0.00001f && std::abs(f32) < 1000000.0f)
					std::print(stderr, "flt: {:<12.4f}", f32);
				else
					std::print(stderr, "int: {:<12}", *(int32_t*)(byte_ptr + i));
			}
		}

		std::println(stderr, " │");
	}

	std::println(stderr,
				 "{}"
				 "└────────────┴────────────────────────────────────────────────┴──────────────────"
				 "┴─────────────────────────┘{}",
				 Color::Cyan, Color::Reset);
}

template <typename T>
concept HasData = requires(T a) {
	a.data();
	a.size();
};

template <typename T> void SmartDumpInternal(const T& var, std::string_view name, LogContext ctx) {
	// Inside a template, if constexpr TRULY discards the code
	if constexpr (requires {
					  var.data();
					  var.size();
				  }) {
		// It's a container (vector, string, array)
		MemoryDump(var.data(), var.size() * sizeof(var[0]), name, ctx);
	} else {
		// It's just a regular object or struct
		MemoryDump(&var, sizeof(var), name, ctx);
	}
}

// Updated macro: uses the 'fmt' field of LogContext to pass a custom sub-label
#define ZHLN_DUMP(var) ZHLN::SmartDumpInternal(var, #var, "Manual Dump")
#define ZHLN_DUMP_EXT(var, label) ZHLN::SmartDumpInternal(&var, sizeof(var), #var, label)

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