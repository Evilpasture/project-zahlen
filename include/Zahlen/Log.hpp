// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CommandLine.hpp"

#include <cstdio>
#include <detail/Print.hpp>
#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace ZHLN {

class Engine;
void SetupSignalHandler();
void CheckForCrashes(Engine* engine);

auto GetCurrentFiberID() -> uint64_t;
auto GetCustomLogFile(FILE* overrideFile = nullptr) -> FILE*;
auto GetPoorMansStacktrace() -> std::string;

enum class LogChannel : uint8_t { StdErr, StdOut, File };
void SetLogLevel(LogLevel level) noexcept;
LogLevel GetLogLevel() noexcept;

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

void InternalWriteLog(uint8_t channel, const char* file, uint32_t line, std::string_view message);
[[noreturn]] void InternalPanic(const char* file, uint32_t line, std::string_view message);

/**
 * @brief Modern C++23 Engine Logger with Fiber awareness and compile-time channel dispatch.
 * Restored to standard dynamic formatting for stable general-purpose runtime use.
 */
template <LogChannel Channel = LogChannel::StdErr, LogLevel Level = LogLevel::Moderate,
		  typename... Args>
void Log(LogContext ctx, Args&&... args) {
	if (static_cast<uint8_t>(GetLogLevel()) < static_cast<uint8_t>(Level)) {
		return;
	}
	std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
	InternalWriteLog(static_cast<uint8_t>(Channel), ctx.loc.file_name(), ctx.loc.line(), formatted);
}

template <typename... Args> [[noreturn]] void Panic(LogContext ctx, Args&&... args) {
	std::string formatted = std::vformat(ctx.fmt, std::make_format_args(args...));
	InternalPanic(ctx.loc.file_name(), ctx.loc.line(), formatted);
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

void LogManual(std::string_view file, int line, std::string_view message, const char* color = "");

int TraceStructCallback(const char* fmt, ...);
void TraceStructHeader(std::string_view name, std::string_view label, const char* file,
					   uint32_t line);
void TraceStructFooter();

#if defined(__clang__)
template <typename T>
inline void TraceStructInternal(const T& obj, std::string_view name, LogContext ctx) {
	TraceStructHeader(name, ctx.fmt, ctx.loc.file_name(), ctx.loc.line());

	if constexpr (std::is_pointer_v<T>) {
		if (obj) {
			__builtin_dump_struct(obj, &TraceStructCallback);
		} else {
			ZHLN::Println(stderr, "  (null pointer)");
		}
	} else if constexpr (requires { obj.get(); }) { // Detects std::unique_ptr / std::shared_ptr
		if (obj.get()) {
			__builtin_dump_struct(obj.get(), &TraceStructCallback);
		} else {
			ZHLN::Println(stderr, "  (null smart pointer)");
		}
	} else {
		__builtin_dump_struct(&obj, &TraceStructCallback);
	}

	TraceStructFooter();
}
#else
template <typename T>
inline void TraceStructInternal(const T& obj, std::string_view name, LogContext ctx) {
	std::fprintf(
		stderr, "[%s:%d] TraceStruct not supported on this compiler. Falling back to MemoryDump.\n",
		ctx.loc.file_name(), (int)ctx.loc.line());
	SmartDumpInternal(obj, name, ctx);
}
#endif

void MemoryDump(const void* ptr, size_t size, std::string_view label, LogContext ctx,
				DumpOptions opts = {});

template <typename T> void SmartDumpInternal(const T& var, std::string_view name, LogContext ctx) {
	if constexpr (requires {
					  var.data();
					  var.size();
				  }) {
		MemoryDump(var.data(), var.size() * sizeof(var[0]), name, ctx);
	} else {
		MemoryDump(&var, sizeof(var), name, ctx);
	}
}

#define ZHLN_DUMP(var) ZHLN::SmartDumpInternal(var, #var, "Manual Dump")
#define ZHLN_DUMP_EXT(var, label) ZHLN::SmartDumpInternal(var, #var, label)
#define ZHLN_TRACE(var) ZHLN::TraceStructInternal(var, #var, "Struct Reflection")

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void;
auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
					  uint32_t inLine) noexcept -> bool;

} // namespace ZHLN
