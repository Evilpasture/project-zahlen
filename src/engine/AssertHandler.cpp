// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/Camera.hpp"
#include "engine/TTYBackend.hpp"
#include "physics/Physics.hpp"

#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <atomic>
#include <cctype> // For std::isprint
#include <cmath>  // For std::isnan, std::abs
#include <csignal>
#include <cstdarg>			   // For va_list, va_start, va_end
#include <cstdint>			   // For uint8_t, uint32_t, uint64_t
#include <cstdio>			   // For FILE, stderr, stdout, vfprintf
#include <cstdlib>			   // For std::abort, std::free
#include <cstring>			   // For std::memcpy
#include <detail/Platform.hpp> // This handles windows.h and includes unistd.h on Unix
#include <detail/Print.hpp>
#include <physics/PhysicsWorld.hpp> // Required to fully define PhysicsWorld for ZHLN_TRACE
#include <print>					// Restored for stable general-purpose printing
#include <string>					// For std::string
#include <string_view>				// For std::string_view
#ifdef _WIN32
#include <process.h> // For _exit
#define HALT_THREAD() Sleep(INFINITE)
#else
#include <unistd.h> // Included via Platform.hpp, but here for clarity
#define HALT_THREAD() pause()
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <execinfo.h>
#else
#define IN
#define OUT
#include <dbghelp.h>
#undef IN
#undef OUT
#pragma comment(lib, "dbghelp.lib")
#endif

namespace ZHLN {

static std::atomic<int> s_PendingSignal{0};
static std::atomic<void*> s_FaultAddr{nullptr};
static std::atomic<LogLevel> s_LogLevel{LogLevel::Moderate};

void SetLogLevel(LogLevel level) noexcept {
	s_LogLevel.store(level, std::memory_order_release);
}

LogLevel GetLogLevel() noexcept {
	return s_LogLevel.load(std::memory_order_acquire);
}

// Low-level writer strictly dedicated to signal handler pathways
static void WriteToChannel(uint8_t channel, std::string_view msg) noexcept {
	if (channel == static_cast<uint8_t>(LogChannel::StdOut)) {
#if defined(_WIN32)
		::_write(1, msg.data(), static_cast<unsigned int>(msg.size()));
#else
		::write(1, msg.data(), msg.size());
#endif
	} else if (channel == static_cast<uint8_t>(LogChannel::File)) {
		FILE* f = GetCustomLogFile();
		if (f != nullptr) {
			std::fwrite(msg.data(), 1, msg.size(), f);
			std::fflush(f);
		}
	} else {
#if defined(_WIN32)
		::_write(2, msg.data(), static_cast<unsigned int>(msg.size()));
#else
		::write(2, msg.data(), msg.size());
#endif
	}
}

// --- Log Implementation Helpers ---

auto GetCustomLogFile(FILE* overrideFile) -> FILE* {
	static FILE* logFile = nullptr;
	if (overrideFile != nullptr) {
		if ((logFile != nullptr) && logFile != stdout && logFile != stderr) {
			std::fclose(logFile);
		}
		logFile = overrideFile;
	}
	if (logFile == nullptr) {
		logFile = std::fopen("zahlen_runtime.log", "w");
	}
	return logFile;
}

auto GetPoorMansStacktrace() -> std::string {
	std::string out;
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
	std::free(static_cast<void*>(strs));
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
		// Note: Stack traces are evaluated under stable conditions on Windows, so std::format is
		// safe
		out += std::format("{}: {} - {:#x}\n", i, symbol->Name, symbol->Address);
	}
#endif
	if (out.length() < 1) {
		out += "Not implemented";
	}
	return out;
}

void InternalWriteLog(uint8_t channel, const char* file, uint32_t line, std::string_view message) {
	// Guard against Quiet mode
	if (s_LogLevel.load(std::memory_order_acquire) == LogLevel::Quiet) {
		return;
	}

	std::string_view file_name = file;
	if (auto pos = file_name.find_last_of("/\\"); pos != std::string_view::npos) {
		file_name.remove_prefix(pos + 1);
	}

	uint64_t fid = GetCurrentFiberID();

	std::string fiberTag;
	if (fid == 0) {
		fiberTag = "Thread";
	} else if (fid == 1) {
		fiberTag = "Main";
	} else {
		fiberTag = std::format("{:#x}", fid);
	}

	FILE* outStream = nullptr;
	if (channel == static_cast<uint8_t>(LogChannel::StdOut)) {
		outStream = stdout;
	} else if (channel == static_cast<uint8_t>(LogChannel::File)) {
		outStream = GetCustomLogFile();
	} else {
		outStream = stderr;
	}

	std::println(outStream, "[{}:{}] [Fiber:{}] {}", file_name, line, fiberTag, message);
}

// Note: Emergency panic / crash dumps are kept unfiltered to preserve crash visibility.
[[noreturn]] void InternalPanic(const char* file, uint32_t line, std::string_view message) {
	// Force enable output for catastrophic crashes
	s_LogLevel.store(LogLevel::Verbose, std::memory_order_release);
	InternalWriteLog(static_cast<uint8_t>(LogChannel::StdErr), file, line, message);
	std::println(stderr, "Stack Trace:\n{}", GetPoorMansStacktrace());
	std::abort();
}

void LogManual(std::string_view file, int line, std::string_view message, const char* color) {
	if (s_LogLevel.load(std::memory_order_acquire) == LogLevel::Quiet) {
		return;
	}

	uint64_t fid = GetCurrentFiberID();
	std::string fiberTag = (fid == 0) ? "Thread" : (fid == 1) ? "Main" : std::format("{:#x}", fid);

	if (color[0] != '\0') {
		std::println(stderr, "{}[{}:{}] [Fiber:{}] [LUA] {}{}", color, file, line, fiberTag,
					 message, Color::Reset);
	} else {
		std::println(stderr, "[{}:{}] [Fiber:{}] [LUA] {}", file, line, fiberTag, message);
	}
}

int TraceStructCallback(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);

	// Keep trace rendering signal-safe via BufferPrint [1]
	char buf[1024];
	int ret = ZHLN::BufferPrint(buf, sizeof(buf), fmt, args);
	if (ret > 0) {
		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), std::string_view(buf, ret));
	}

	va_end(args);
	return ret;
}

void TraceStructHeader(std::string_view name, std::string_view label, const char* file,
					   uint32_t line) {
	std::string_view file_name = file;
	if (auto pos = file_name.find_last_of("/\\"); pos != std::string_view::npos) {
		file_name.remove_prefix(pos + 1);
	}
	auto line1 = ZHLN::Format("{}┌─── STRUCT TRACE: {} ({}) ───{}\n", Color::Cyan, name, label,
							  Color::Reset);
	auto line2 = ZHLN::Format("│ Source:  {}:{}\n", file_name, line);
	auto line3 =
		"├──────────────────────────────────────────────────────────────────────────────\n";

	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), line1.string_view());
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), line2.string_view());
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), line3);
}

void TraceStructFooter() {
	auto line = ZHLN::Format(
		"{}└──────────────────────────────────────────────────────────────────────────────{}\n",
		Color::Cyan, Color::Reset);
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), line.string_view());
}

void MemoryDump(const void* ptr, size_t size, std::string_view label, LogContext ctx,
				DumpOptions opts) {
	const auto* byte_ptr = static_cast<const uint8_t*>(ptr);
	std::string_view file_name = ctx.loc.file_name();
	if (auto pos = file_name.find_last_of("/\\"); pos != std::string_view::npos) {
		file_name.remove_prefix(pos + 1);
	}

	auto header1 =
		ZHLN::Format("{}┌─── DUMP: {} ({}) ───{}\n", Color::Cyan, label, ctx.fmt, Color::Reset);
	auto header2 = ZHLN::Format("│ Source:  {}:{}\n", file_name, ctx.loc.line());
	auto header3 =
		ZHLN::Format("│ Address: {}{}{} ({} bytes)\n", Color::Yellow, ptr, Color::Reset, size);
	auto header4 = "├────────────┬────────────────────────────────────────────────┬──────────"
				   "────────┬─────────────────────────┐\n";
	auto header5 = "│  Address   │ Hex Data                                       │ ASCII    "
				   "        │ Interpretation          │\n";
	auto header6 = "├────────────┼────────────────────────────────────────────────┼──────────"
				   "────────┼─────────────────────────┤\n";

	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), header1.string_view());
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), header2.string_view());
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), header3.string_view());
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), header4);
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), header5);
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), header6);

	for (size_t i = 0; i < size; i += opts.bytes_per_line) {
		auto addr_str = ZHLN::Format("│ {}{:#010X}{} │ ", Color::Cyan,
									 std::bit_cast<uintptr_t>(byte_ptr + i), Color::Reset);
		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), addr_str.string_view());

		for (size_t j = 0; j < opts.bytes_per_line; ++j) {
			if (i + j < size) {
				uint8_t b = byte_ptr[i + j];
				if (b == 0) {
					auto hex_zero = ZHLN::Format("{}00{} ", Color::Gray, Color::Reset);
					WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr),
								   hex_zero.string_view());
				} else {
					if (b < 16) {
						auto hex_val = ZHLN::Format("0{:X} ", b);
						WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr),
									   hex_val.string_view());
					} else {
						auto hex_val = ZHLN::Format("{:X} ", b);
						WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr),
									   hex_val.string_view());
					}
				}
			} else {
				WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), "   ");
			}
			if ((j + 1) % 4 == 0 && j + 1 < opts.bytes_per_line) {
				WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), " ");
			}
		}

		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), "│ ");
		for (size_t j = 0; j < opts.bytes_per_line; ++j) {
			if (i + j < size) {
				uint8_t c = byte_ptr[i + j];
				if (std::isprint(c)) {
					char c_str[2] = {static_cast<char>(c), '\0'};
					WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), c_str);
				} else {
					auto dot = ZHLN::Format("{}·{}", Color::Gray, Color::Reset);
					WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), dot.string_view());
				}
			} else {
				WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), " ");
			}
		}

		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), " │ ");
		if (i + 8 <= size) {
			uint64_t val64 = 0;
			std::memcpy(&val64, byte_ptr + i, 8);

			if (val64 != 0) {
				if (val64 > 0x100000000 && val64 < 0x00007FFFFFFFFFFF) {
					auto ptr_str =
						ZHLN::Format("{}ptr: {:#014X}{}", Color::Green, val64, Color::Reset);
					WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), ptr_str.string_view());
				} else {
					float f32 = NAN;
					int32_t i32 = 0;
					std::memcpy(&f32, byte_ptr + i, 4);
					std::memcpy(&i32, byte_ptr + i, 4);

					if (!std::isnan(f32) && std::abs(f32) > 0.0001f && std::abs(f32) < 1000000.0f) {
						auto flt_str = ZHLN::Format("flt: {}", f32);
						WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr),
									   flt_str.string_view());
					} else {
						auto int_str = ZHLN::Format("int: {}", i32);
						WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr),
									   int_str.string_view());
					}
				}
			} else {
				auto dash_str = ZHLN::Format("{}---{}", Color::Gray, Color::Reset);
				WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), dash_str.string_view());
			}
		}

		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), " │\n");
	}

	auto footer = ZHLN::Format(
		"{}└────────────┴────────────────────────────────────────────────┴──────────────────"
		"┴─────────────────────────┘{}\n",
		Color::Cyan, Color::Reset);
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), footer.string_view());
}

// --- Original Signal and Crash Diagnostic Logic ---

static void PerformDiagnosticDump(int sig, void* addr, Engine* engine) {
	const char* sigName = "UNKNOWN";
	switch (sig) {
		case SIGSEGV:
			sigName = "SIGSEGV (Access Violation)";
			break;
		case SIGILL:
			sigName = "SIGILL (Illegal Instruction)";
			break;
		case SIGFPE:
			sigName = "SIGFPE (Math Error)";
			break;
		case SIGABRT:
			sigName = "SIGABRT (Abort)";
			break;
#ifndef _WIN32
		case SIGBUS:
			sigName = "SIGBUS (Bus Error)";
			break;
#endif
	}

	auto sig_header =
		ZHLN::Format("\n{}DIAGNOSTIC REPORT FOR SIGNAL: {}{}\n", Color::Red, sigName, Color::Reset);
	auto addr_header =
		ZHLN::Format("Faulting Address: {}{}{}\n", Color::Yellow, addr, Color::Reset);
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), sig_header.string_view());
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), addr_header.string_view());

	if (engine != nullptr) {
		ZHLN_TRACE(*engine);

		auto& cam = engine->GetCamera();
		auto cam_hdr = ZHLN::Format("\n{}--- CAMERA DEEP STATE ---{}\n", Color::Cyan, Color::Reset);
		auto cam_pos = ZHLN::Format("  Position:  ({}, {}, {})\n", cam.position.GetX(),
									cam.position.GetY(), cam.position.GetZ());
		auto cam_dir = ZHLN::Format("  Direction: Yaw: {}, Pitch: {}\n", cam.yaw, cam.pitch);

		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), cam_hdr.string_view());
		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), cam_pos.string_view());
		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), cam_dir.string_view());

		auto& f = cam.frustum;
		auto frust_hdr = ZHLN::Format("\n{}--- FRUSTUM PLANE EQUATIONS (SIMD DECODED) ---{}\n",
									  Color::Cyan, Color::Reset);
		WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), frust_hdr.string_view());
		const char* names[] = {"Left  ", "Right ", "Top   ", "Bottom", "Near  ", "Far   "};

		for (int i = 0; i < 6; ++i) {
			int block = i / 4;
			int lane = i % 4;
			auto plane_str = ZHLN::Format("  Plane {}: [{}x {}y {}z] offset: {}\n", names[i],
										  f.mX[block].mF32[lane], f.mY[block].mF32[lane],
										  f.mZ[block].mF32[lane], f.mW[block].mF32[lane]);
			WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), plane_str.string_view());
		}

		ZHLN_DUMP(cam.frustum);

		if (engine->GetPhysicsContext().GetImpl() != nullptr) {
			ZHLN_TRACE(engine->GetPhysicsContext().GetWorld());
		}
	}

	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), "\nStack Trace:\n");
	auto stack = GetPoorMansStacktrace();
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), stack);
	WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), "\n");
}

static void ProcessCrash(int sig, void* addr) {

	// If the signal is SIGABRT, it was raised by std::abort() inside InternalPanic.
	// We already printed the panic and stacktrace, so exit immediately.
	if (sig == SIGABRT) {
		_exit(sig);
	}

	// DOUBLE-FAULT GUARD:
	int expected = 0;
	if (!s_PendingSignal.compare_exchange_strong(expected, sig)) {
		if (expected == -1) {
			ZHLN::Println("!! SECONDARY CRASH DURING DIAGNOSTICS. ABORTING !!");
		}
		_exit(sig);
	}

	s_FaultAddr.store(addr);

	if (ZHLN::GetCurrentFiberID() == 1) {
		ZHLN::Print("\n[ZHLN] Terminal signal on Main Thread. Attempting emergency dump...\n");

		// Mark that we are now in the "Emergency Dump" phase
		s_PendingSignal.store(-1);
		// If TTY was active, it recovers. If GLFW was active, this is a silent no-op.
		TTYBackend::EmergencyRestore();

		PerformDiagnosticDump(sig, addr, ZHLN::GetEngineContext());

		_exit(sig);
	} else {
		ZHLN::Print("\n[ZHLN] Signal intercepted in Worker. Main Thread will dump soon...\n");
		while (true) {
			HALT_THREAD();
		}
	}
}

#ifdef _WIN32
static LONG WINAPI VectoredCrashHandler(PEXCEPTION_POINTERS pExceptionInfo) {
	int sig = 0;
	switch (pExceptionInfo->ExceptionRecord->ExceptionCode) {
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_STACK_OVERFLOW:
			sig = SIGSEGV;
			break;
		case EXCEPTION_ILLEGAL_INSTRUCTION:
			sig = SIGILL;
			break;
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
			sig = SIGFPE;
			break;
		default:
			return EXCEPTION_CONTINUE_SEARCH;
	}
	ProcessCrash(sig, pExceptionInfo->ExceptionRecord->ExceptionAddress);
	return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void PosixCrashHandler(int sig, siginfo_t* info, void* context) {
	// Intercept standard user terminations (Ctrl+C / Kill)
	if (sig == SIGINT || sig == SIGTERM) {
		ZHLN::TTYBackend::EmergencyRestore();
		_exit(0); // Exit cleanly immediately
	}

	// Forward standard fatal errors to the crash handler
	ProcessCrash(sig, info->si_addr);
}
#endif

void CheckForCrashes(Engine* engine) {
	int sig = s_PendingSignal.load();
	if (sig <= 0) {
		return;
	}

	s_PendingSignal.store(-1);
	PerformDiagnosticDump(sig, s_FaultAddr.load(), engine);
	std::abort();
}

void SetupSignalHandler() {
#ifdef _WIN32
	AddVectoredExceptionHandler(1, VectoredCrashHandler);
#else
	struct sigaction sa{};
	sa.sa_sigaction = PosixCrashHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;

	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
	sigaction(SIGFPE, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
#endif
}

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void {
	va_list list;
	va_start(list, inFMT);

	char buffer[1024]{};
	int result = ZHLN::BufferPrint(buffer, sizeof(buffer), inFMT, list);

	va_end(list);

	if (result > 0) {
		ZHLN::Panic("{}", buffer);
	}
}

auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile,
					  uint32_t inLine) noexcept -> bool {
	ZHLN::Log("--- JOLT ASSERT FAILED ---\n"
			  "Expr: {}\n"
			  "Msg:  {}\n"
			  "File: {}:{}\n"
			  "--------------------------\n",
			  inExpression, ((inMessage != nullptr) ? inMessage : "None"), inFile, inLine);
	return true;
}

} // namespace ZHLN
