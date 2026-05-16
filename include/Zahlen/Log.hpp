#pragma once

#include <cstdarg>
#include <cstdio>
#include <format>
#include <print>
#include <source_location>
#include <string_view>

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <execinfo.h>
#else
#include <detail/Platform.hpp>

// --- FIX: Restore tokens required by dbghelp.h ---
#define IN
#define OUT

#include <dbghelp.h>

// --- Re-sanitize so these don't leak into your engine ---
#undef IN
#undef OUT

#pragma comment(lib, "dbghelp.lib")
#endif

namespace ZHLN {
class Engine;
void SetupSignalHandler();
/**
 * @brief Bridge function forward-declared here, implemented in Thread.cpp.
 * This prevents Log.hpp from needing to include the heavy Thread.hpp.
 */
auto GetCurrentFiberID() -> uint64_t;

inline auto GetPoorMansStacktrace() -> std::string {
	std::string out = "";
#if defined(__APPLE__) || defined(__linux__)
	void* callstack[128];
	int frames = backtrace(callstack, 128);
	char** strs = backtrace_symbols(callstack, frames);

	for (int i = 0; i < frames; ++i) {
		std::string line = strs[i];

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
	void* stack[100];
	unsigned short frames = CaptureStackBackTrace(0, 100, stack, nullptr);
	HANDLE process = GetCurrentProcess();
	SymInitialize(process, nullptr, true);

	char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
	PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen = MAX_SYM_NAME;

	for (unsigned int i = 0; i < frames; i++) {
		SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol);
		out += std::format("{}: {} - {:#x}\n", i, symbol->Name, symbol->Address);
	}
#endif
	if (out.length() < 1) {
		out += "Not implemented";
	}
	return out;
}

struct LogContext {
	std::string_view fmt;
	std::source_location loc;

	template <typename T>
	consteval LogContext(const T& s, std::source_location l = std::source_location::current())
		: fmt(s), loc(l) {}

	constexpr LogContext(std::string_view s,
						 std::source_location l = std::source_location::current())
		: fmt(s), loc(l) {}
};

/**
 * @brief Modern C++23 Engine Logger with Fiber awareness
 */
template <typename... Args> void Log(LogContext ctx, Args&&... args) {
	std::string_view file = ctx.loc.file_name();
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos)
		file.remove_prefix(pos + 1);

	uint64_t fid = GetCurrentFiberID();

	// If fid is 0, it means we are on a raw OS thread or the fiber system isn't init yet.
	// If fid is 1, it's usually the Main thread/fiber.
	std::string fiberTag;
	if (fid == 0)
		fiberTag = "Thread";
	else if (fid == 1)
		fiberTag = "Main";
	else
		fiberTag = std::format("{:#x}", fid);

	std::println(stderr, "[{}:{}] [Fiber:{}] {}", file, ctx.loc.line(), fiberTag,
				 std::vformat(ctx.fmt, std::make_format_args(args...)));
}

template <typename... Args> [[noreturn]] void Panic(LogContext ctx, Args&&... args) {
	Log(ctx, std::forward<Args>(args)...);
	std::println(stderr, "Stack Trace:\n{}", GetPoorMansStacktrace());
	std::abort();
}

struct DumpOptions {
	size_t bytes_per_line = 16;
	bool show_ascii = true;
	bool show_interpret = true;
};

// ANSI Color Helpers
namespace Color {
constexpr char Reset[] = "\033[0m";
constexpr char Gray[] = "\033[90m";
constexpr char Cyan[] = "\033[36m";
constexpr char Yellow[] = "\033[33m";
constexpr char Green[] = "\033[32m";
constexpr char Red[] = "\033[31m";
} // namespace Color

/**
 * @brief Manual override for Log (Used by Scripting/Lua)
 */
inline void LogManual(std::string_view file, int line, std::string_view message,
					  const char* color = "") {
	uint64_t fid = GetCurrentFiberID();
	std::string fiberTag = (fid == 0) ? "Thread" : (fid == 1) ? "Main" : std::format("{:#x}", fid);

	// If a color is provided, wrap the message
	if (color[0] != '\0') {
		std::println(stderr, "{}[{}:{}] [Fiber:{}] [LUA] {}{}", color, file, line, fiberTag,
					 message, Color::Reset);
	} else {
		std::println(stderr, "[{}:{}] [Fiber:{}] [LUA] {}", file, line, fiberTag, message);
	}
}

#if defined(__clang__)
// Callback used by __builtin_dump_struct
// We route it to stderr so it matches ZHLN::Log and ZHLN::MemoryDump
inline int TraceStructCallback(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int ret = std::vfprintf(stderr, fmt, args);
	va_end(args);
	return ret;
}

template <typename T>
inline void TraceStructInternal(const T& obj, std::string_view name, LogContext ctx) {
	std::println(stderr, "{}┌─── STRUCT TRACE: {} ({}) ───{}", Color::Cyan, name, ctx.fmt,
				 Color::Reset);
	std::println(stderr, "│ Source:  {}:{}", ctx.loc.file_name(), ctx.loc.line());
	std::println(stderr,
				 "├──────────────────────────────────────────────────────────────────────────────");

	// Smart detection: If you pass a pointer, we dereference it automatically!
	if constexpr (std::is_pointer_v<T>) {
		if (obj)
			__builtin_dump_struct(obj, &TraceStructCallback);
		else
			std::println(stderr, "  (null pointer)");
	} else if constexpr (requires { obj.get(); }) { // Detects std::unique_ptr / std::shared_ptr
		if (obj.get())
			__builtin_dump_struct(obj.get(), &TraceStructCallback);
		else
			std::println(stderr, "  (null smart pointer)");
	} else {
		__builtin_dump_struct(&obj, &TraceStructCallback);
	}

	std::println(
		stderr,
		"{}└──────────────────────────────────────────────────────────────────────────────{}",
		Color::Cyan, Color::Reset);
}
#else
template <typename T>
inline void TraceStructInternal(const T& obj, std::string_view name, LogContext ctx) {
	std::println(stderr,
				 "[{}:{}] TraceStruct not supported on this compiler. Falling back to MemoryDump.",
				 ctx.loc.file_name(), ctx.loc.line());
	SmartDumpInternal(obj, name, ctx);
}
#endif

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
		if (i + 8 <= size) {
			uint64_t val64;
			std::memcpy(&val64, byte_ptr + i, 8);

			if (val64 != 0) {
				// Valid pointer range for macOS Silicon / Linux x64
				if (val64 > 0x100000000 && val64 < 0x00007FFFFFFFFFFF) {
					std::print(stderr, "{}ptr: {:#014x}{}", Color::Green, val64, Color::Reset);
				} else {
					// If not a pointer, try 32-bit interpretations
					float f32;
					int32_t i32;
					std::memcpy(&f32, byte_ptr + i, 4);
					std::memcpy(&i32, byte_ptr + i, 4);

					if (!std::isnan(f32) && std::abs(f32) > 0.0001f && std::abs(f32) < 1000000.0f)
						std::print(stderr, "flt: {:<12.4f}", f32);
					else
						std::print(stderr, "int: {:<12}", i32);
				}
			} else {
				std::print(stderr, "{}---{}", Color::Gray,
						   Color::Reset); // Just show a dash for zero
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
#define ZHLN_TRACE(var) ZHLN::TraceStructInternal(var, #var, "Struct Reflection")

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