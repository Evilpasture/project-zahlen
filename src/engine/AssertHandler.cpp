#include "Zahlen/Engine.hpp"

#include <Zahlen/Log.hpp>
#include <atomic>
#include <csignal>
#include <unistd.h>

namespace ZHLN {

static std::atomic<int> s_PendingSignal{0};
static std::atomic<void*> s_FaultAddr{nullptr};

static void WriteSafe(const char* msg) {
	size_t len = 0;
	while (msg[len])
		len++;
	[[maybe_unused]] auto _ = write(STDERR_FILENO, msg, len);
}

// --- NEW: Internal Emergency Logic ---
static void PerformDiagnosticDump(int sig, void* addr, Engine* engine) {
	const char* sigName = "UNKNOWN";
	switch (sig) {
		case SIGSEGV:
			sigName = "SIGSEGV";
			break;
		case SIGBUS:
			sigName = "SIGBUS";
			break;
		case SIGILL:
			sigName = "SIGILL";
			break;
		case SIGFPE:
			sigName = "SIGFPE";
			break;
		case SIGABRT:
			sigName = "SIGABRT";
			break;
	}

	std::println(stderr, "\n{}DIAGNOSTIC REPORT FOR SIGNAL: {}{}", Color::Red, sigName,
				 Color::Reset);
	std::println(stderr, "Faulting Address: {}{}{}", Color::Yellow, addr, Color::Reset);

	if (engine) {
		ZHLN_TRACE(*engine);
		if (engine->GetPhysicsContext().GetImpl()) {
			ZHLN_TRACE(engine->GetPhysicsContext().GetWorld());
		}
	}

	std::println(stderr, "\nStack Trace (Capture at moment of diagnostic):\n{}",
				 GetPoorMansStacktrace());
}

static void CrashHandler(int sig, siginfo_t* info, [[maybe_unused]] void* context) {
	// If we are already handling a crash and get another signal, just die hard.
	if (s_PendingSignal.load() == -1) {
		_exit(sig);
	}

	s_PendingSignal.store(sig);
	s_FaultAddr.store(info->si_addr);

	// Note: Signal type check (HW vs SW) is implicit in the dump logic.
	// Hardware faults on Main Thread are immediate; Workers block.

	if (ZHLN::GetCurrentFiberID() == 1) {
		// ====================================================================
		// MAIN THREAD BYPASS (Immediate emergency dump)
		// ====================================================================
		WriteSafe("\n[ZHLN] Terminal signal on Main Thread. Attempting emergency dump...\n");

		// Mark as "In Progress" to prevent recursive abort loops
		s_PendingSignal.store(-1);

		// Perform dump using current thread context
		PerformDiagnosticDump(sig, info->si_addr, ZHLN::GetEngineContext());

		_exit(sig);
	} else {
		// ====================================================================
		// WORKER THREAD QUEUE (Block worker, wait for Main Thread)
		// ====================================================================
		WriteSafe("\n[ZHLN] Signal intercepted in Worker. Main Thread will dump soon...\n");

		// Block this worker thread forever so it stops corrupting state.
		// The Main Thread will see s_PendingSignal in the next ProcessEvents() call.
		while (true) {
			pause();
		}
	}
}

void CheckForCrashes(Engine* engine) {
	int sig = s_PendingSignal.load();
	if (sig <= 0)
		return;

	// Reset flag
	s_PendingSignal.store(-1);

	// Perform the safe dump from the main loop
	PerformDiagnosticDump(sig, s_FaultAddr.load(), engine);

	std::abort();
}

void SetupSignalHandler() {
	struct sigaction sa;
	sa.sa_sigaction = CrashHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;

	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
	sigaction(SIGFPE, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
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