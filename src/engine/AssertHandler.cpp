#include "Zahlen/Engine.hpp"

#include <Zahlen/Log.hpp>
#include <atomic>
#include <csignal>
#include <detail/Platform.hpp> // This handles windows.h and includes unistd.h on Unix

#ifdef _WIN32
#include <io.h>		 // For _write
#include <process.h> // For _exit
#define WRITE_STDOUT(msg, len) _write(2, msg, (unsigned int)len)
#define HALT_THREAD() Sleep(INFINITE)
#else
#include <unistd.h> // Included via Platform.hpp, but here for clarity
#define WRITE_STDOUT(msg, len) write(STDERR_FILENO, msg, len)
#define HALT_THREAD() pause()
#endif

namespace ZHLN {

static std::atomic<int> s_PendingSignal{0};
static std::atomic<void*> s_FaultAddr{nullptr};

static void WriteSafe(const char* msg) {
	size_t len = 0;
	while (msg[len]) {
		len++;
	}
	[[maybe_unused]] auto _ = WRITE_STDOUT(msg, len);
}

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

	std::println(stderr, "\n{}DIAGNOSTIC REPORT FOR SIGNAL: {}{}", Color::Red, sigName,
				 Color::Reset);
	std::println(stderr, "Faulting Address: {}{}{}", Color::Yellow, addr, Color::Reset);

	if (engine) {
		// 1. High-level structure trace
		ZHLN_TRACE(*engine);

		// 2. Camera Deep State
		auto& cam = engine->GetCamera();
		std::println(stderr, "\n{}--- CAMERA DEEP STATE ---{}", Color::Cyan, Color::Reset);
		std::println(stderr, "  Position:  ({:.4f}, {:.4f}, {:.4f})", cam.position.GetX(),
					 cam.position.GetY(), cam.position.GetZ());
		std::println(stderr, "  Direction: Yaw: {:.2f}, Pitch: {:.2f}", cam.yaw, cam.pitch);

		// 3. Frustum "Crawl" (SIMD Decode)
		auto& f = cam.frustum;
		std::println(stderr, "\n{}--- FRUSTUM PLANE EQUATIONS (SIMD DECODED) ---{}", Color::Cyan,
					 Color::Reset);
		const char* names[] = {"Left  ", "Right ", "Top   ", "Bottom", "Near  ", "Far   "};

		for (int i = 0; i < 6; ++i) {
			int block = i / 4;
			int lane = i % 4;
			// Crawling into the mF32 union of the Vec4 block
			std::println(stderr, "  Plane {}: [{:>10.4f}x {:>10.4f}y {:>10.4f}z] offset: {:>10.4f}",
						 names[i], f.mX[block].mF32[lane], f.mY[block].mF32[lane],
						 f.mZ[block].mF32[lane], f.mW[block].mF32[lane]);
		}

		// 4. Raw Memory Dump of the Frustum structure
		// This will show the raw hex and float interpretations side-by-side
		ZHLN_DUMP(cam.frustum);

		// 5. Physics Trace
		if (engine->GetPhysicsContext().GetImpl()) {
			ZHLN_TRACE(engine->GetPhysicsContext().GetWorld());
		}
	}

	std::println(stderr, "\nStack Trace:\n{}", GetPoorMansStacktrace());
}

// --- Shared Internal Logic ---
static void ProcessCrash(int sig, void* addr) {
	if (s_PendingSignal.load() == -1) {
		_exit(sig);
	}

	s_PendingSignal.store(sig);
	s_FaultAddr.store(addr);

	if (ZHLN::GetCurrentFiberID() == 1) {
		WriteSafe("\n[ZHLN] Terminal signal on Main Thread. Attempting emergency dump...\n");
		s_PendingSignal.store(-1);
		PerformDiagnosticDump(sig, addr, ZHLN::GetEngineContext());
		_exit(sig);
	} else {
		WriteSafe("\n[ZHLN] Signal intercepted in Worker. Main Thread will dump soon...\n");
		while (true) {
			HALT_THREAD();
		}
	}
}

// --- Platform Specific Entry Points ---

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
	return EXCEPTION_CONTINUE_SEARCH; // Technically unreachable due to HALT/exit
}
#else
static void PosixCrashHandler(int sig, siginfo_t* info, [[maybe_unused]] void* context) {
	ProcessCrash(sig, info->si_addr);
}
#endif

void CheckForCrashes(Engine* engine) {
	int sig = s_PendingSignal.load();
	if (sig <= 0)
		return;

	s_PendingSignal.store(-1);
	PerformDiagnosticDump(sig, s_FaultAddr.load(), engine);
	std::abort();
}

void SetupSignalHandler() {
#ifdef _WIN32
	AddVectoredExceptionHandler(1, VectoredCrashHandler);
#else
	struct sigaction sa;
	sa.sa_sigaction = PosixCrashHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;

	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
	sigaction(SIGFPE, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
#endif
}

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void {
	va_list list;
	va_start(list, inFMT);

	// Use a local buffer for the C-style string formatting
	char buffer[1024]{};
	int result = std::vsnprintf(buffer, sizeof(buffer), inFMT, list);

	va_end(list);

	if (result > 0) {
		// Hand off the formatted string to our modern Log system
		// We use "{}" to ensure we don't re-parse the Jolt output for format specifiers
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
			  inExpression, (inMessage ? inMessage : "None"), inFile, inLine);
	return true; // Trigger debug break
}

} // namespace ZHLN