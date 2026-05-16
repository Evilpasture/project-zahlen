#include <Zahlen/Log.hpp>
#include <csignal>
#include <detail/Atomic.hpp>
#include <detail/Platform.hpp>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define WRITE_STDOUT(msg, len) _write(2, msg, (unsigned int)len)
#define HALT_THREAD() Sleep(INFINITE)
#else
#include <sys/wait.h>
#include <unistd.h>
#define WRITE_STDOUT(msg, len) write(STDERR_FILENO, msg, len)
#define HALT_THREAD() pause()
#endif

namespace ZHLN {

static ZHLN::Atomic<int> s_PendingSignal{0};

// --- Async-Signal-Safe Utilities ---
// We cannot use std::to_string or std::format in a signal handler!

static void WriteSafe(const char* msg) {
	size_t len = 0;
	while (msg[len])
		len++;
	[[maybe_unused]] auto _ = WRITE_STDOUT(msg, len);
}

// Converts a positive integer to a string in a pre-allocated buffer
static void UtoaSafe(uint64_t val, char* buf, int base = 10) {
	if (val == 0) {
		buf[0] = '0';
		buf[1] = '\0';
		return;
	}
	char temp[64];
	int i = 0;
	while (val != 0) {
		uint64_t rem = val % base;
		temp[i++] = (rem > 9) ? (char)(rem - 10 + 'A') : (char)(rem + '0');
		val /= base;
	}
	int j = 0;
	while (i > 0) {
		buf[j++] = temp[--i];
	}
	buf[j] = '\0';
}

static void SpawnReporterProcess(int sig, void* addr) {
	// Prepare buffers for the arguments
	char sigBuf[32];
	char addrBuf[64];
	char pidBuf[32];

	UtoaSafe(static_cast<uint64_t>(sig), sigBuf, 10);
	addrBuf[0] = '0';
	addrBuf[1] = 'x';
	UtoaSafe(reinterpret_cast<uint64_t>(addr), addrBuf + 2, 16);

#ifdef _WIN32
	UtoaSafe(GetCurrentProcessId(), pidBuf, 10);

	// Windows CreateProcess requires a mutable command line string
	char cmdLine[512];
	int i = 0;
	auto append = [&](const char* str) {
		while (*str && i < 510)
			cmdLine[i++] = *str++;
	};

	append("ZahlenReporter.exe --sig ");
	append(sigBuf);
	append(" --addr ");
	append(addrBuf);
	append(" --pid ");
	append(pidBuf);
	cmdLine[i] = '\0';

	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi = {0};

	// Launch the reporter as a detached process
	if (CreateProcessA("ZahlenReporter.exe", cmdLine, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
					   nullptr, nullptr, &si, &pi)) {
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	} else {
		WriteSafe("FATAL: Failed to launch ZahlenReporter.exe\n");
	}
#else
	UtoaSafe(getpid(), pidBuf, 10);

	pid_t pid = fork();
	if (pid == 0) {
		// Child Process: Exec the reporter
		const char* args[] = {
			"./ZahlenReporter", "--sig", sigBuf, "--addr", addrBuf, "--pid", pidBuf, nullptr};
		execvp(args[0], const_cast<char**>(args));

		// If execvp returns, it failed to find the reporter
		WriteSafe("FATAL: Failed to execute ./ZahlenReporter\n");
		_exit(1);
	}
#endif
}

static void ProcessCrash(int sig, void* addr) {
	int expected = 0;
	// Ensure only ONE thread processes the crash
	if (!s_PendingSignal.compare_exchange_strong(expected, sig)) {
		while (true)
			HALT_THREAD();
	}

	WriteSafe("\n[ZHLN] FATAL EXCEPTION INTERCEPTED. Launching Crash Reporter...\n");

	// Safely spawn the out-of-process reporter
	SpawnReporterProcess(sig, addr);

	// Give the OS a moment to schedule the new process, then kill the engine.
	// Do NOT return from this function, or the corrupted thread will resume.
#ifdef _WIN32
	Sleep(2000);
	TerminateProcess(GetCurrentProcess(), sig);
#else
	sleep(2);
	_exit(sig);
#endif
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