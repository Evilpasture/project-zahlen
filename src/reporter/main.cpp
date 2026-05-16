// src/reporter/main.cpp
#include <cstdlib>
#include <print>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <dbghelp.h>
#include <windows.h>
#pragma comment(lib, "dbghelp.lib")
#endif

// ANSI Colors
namespace Color {
constexpr char Reset[] = "\033[0m";
constexpr char Red[] = "\033[31m";
constexpr char Yellow[] = "\033[33m";
constexpr char Cyan[] = "\033[36m";
} // namespace Color

int main(int argc, char** argv) {
	int sig = 0;
	std::string addr = "UNKNOWN";
	std::string pidStr = "0";

	// Parse simple args: --sig <sig> --addr <addr> --pid <pid>
	for (int i = 1; i < argc - 1; ++i) {
		std::string arg = argv[i];
		if (arg == "--sig")
			sig = std::stoi(argv[++i]);
		else if (arg == "--addr")
			addr = argv[++i];
		else if (arg == "--pid")
			pidStr = argv[++i];
	}

	const char* sigName = "UNKNOWN";
	switch (sig) {
		case 11:
			sigName = "SIGSEGV (Access Violation / Segfault)";
			break; // 11 on POSIX
		case 4:
			sigName = "SIGILL (Illegal Instruction)";
			break;
		case 8:
			sigName = "SIGFPE (Math/Divide by Zero Error)";
			break;
		case 6:
			sigName = "SIGABRT (Abort/Assertion Failed)";
			break;
		case 10:
			sigName = "SIGBUS (Bus Error)";
			break;
	}

	std::println(stderr, "\n{}=================================================={}", Color::Red,
				 Color::Reset);
	std::println(stderr, "{}  ZAHLEN ENGINE CRASH REPORTER{}", Color::Red, Color::Reset);
	std::println(stderr, "{}=================================================={}\n", Color::Red,
				 Color::Reset);

	std::println(stderr, "{}Signal:{}   {} ({})", Color::Cyan, Color::Reset, sigName, sig);
	std::println(stderr, "{}Address:{}  {}", Color::Cyan, Color::Reset, addr);
	std::println(stderr, "{}Engine PID:{} {}\n", Color::Cyan, Color::Reset, pidStr);

	std::println(stderr, "{}Extracting Stack Trace...{}\n", Color::Yellow, Color::Reset);

#ifdef _WIN32
	// Windows: Generate a MiniDump file that can be opened in Visual Studio
	DWORD pid = std::stoul(pidStr);
	HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);

	if (hProcess) {
		HANDLE hFile = CreateFileA("ZahlenCrashDump.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
								   FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile != INVALID_HANDLE_VALUE) {
			if (MiniDumpWriteDump(hProcess, pid, hFile, MiniDumpNormal, nullptr, nullptr,
								  nullptr)) {
				std::println(stderr, "Success! Crash dump saved to 'ZahlenCrashDump.dmp'.\nOpen "
									 "this file in Visual Studio to debug the crash.");
			} else {
				std::println(stderr, "Failed to write MiniDump. Error: {}", GetLastError());
			}
			CloseHandle(hFile);
		}
		CloseHandle(hProcess);
	} else {
		std::println(stderr, "Could not attach to Engine process (PID: {}). Error: {}", pid,
					 GetLastError());
	}

	// Keep console open so the user can read the error
	std::println(stderr, "\nPress Enter to exit...");
	std::getchar();
#else
	// POSIX (Linux/macOS): We can invoke the system debugger (gdb or lldb) to attach
	// to the frozen PID, print the stack trace, and detach.

#if defined(__APPLE__)
	// macOS uses lldb
	std::string cmd =
		std::format("lldb -p {} --batch -o \"thread backtrace all\" -o \"quit\"", pidStr);
#else
	// Linux uses gdb
	std::string cmd = std::format("gdb -p {} -batch -ex \"thread apply all bt\"", pidStr);
#endif

	// system() is safe here because this is a separate, healthy process.
	int result = system(cmd.c_str());

	if (result != 0) {
		std::println(stderr, "Failed to extract stack trace. Ensure 'gdb' or 'lldb' is installed.");
	}
#endif

	return 0;
}