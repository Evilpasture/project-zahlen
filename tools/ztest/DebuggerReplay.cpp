// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DebuggerReplay.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Ztest::Debugger {

namespace fs = std::filesystem;

static auto EqualAsciiNoCase(std::string_view lhs, std::string_view rhs) noexcept -> bool {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

static auto PathFromEnvironment(const Replay& replay) -> std::string_view {
    for (const auto& [key, value]: replay.environment) {
        if (EqualAsciiNoCase(key, "PATH")) {
            return value;
        }
    }
#if defined(_WIN32)
    return {};
#else
    return "/usr/bin:/bin"; // execvp's conventional fallback when PATH is absent
#endif
}

// Resolve against the *worker's* directory: an empty/relative PATH component
// means something different after the worker's chdir than it does in ztest.
static auto FindOnWorkerPath(const Replay& replay, std::string_view name) -> std::optional<std::string> {
    std::error_code ec;
    const fs::path parentCwd = fs::current_path(ec);
    if (ec) {
        return std::nullopt;
    }
    fs::path workerCwd = replay.workingDir.empty() ? parentCwd : fs::path {replay.workingDir};
    if (workerCwd.is_relative()) {
        workerCwd = parentCwd / workerCwd;
    }

    const std::string_view searchPath = PathFromEnvironment(replay);
#if defined(_WIN32)
    constexpr char separator = ';';
    const std::string executable = std::string {name}.append(".exe");
#else
    constexpr char separator = ':';
    const std::string executable {name};
#endif
    std::size_t start = 0;
    while (start <= searchPath.size()) {
        const std::size_t end = searchPath.find(separator, start);
        const std::string_view component = searchPath.substr(start, end == std::string_view::npos ? end : end - start);
        fs::path dir = component.empty() ? fs::path {"."} : fs::path {component};
#if defined(_WIN32)
        // Some Windows PATH entries are quoted even though quotes are not part
        // of their directory names.
        const std::string raw = dir.string();
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
            dir = raw.substr(1, raw.size() - 2);
        }
#endif
        if (dir.is_relative()) {
            dir = workerCwd / dir;
        }
        const fs::path candidate = dir / executable;
#if defined(_WIN32)
        const bool executableFile = fs::is_regular_file(candidate, ec);
#else
        const bool executableFile = ::access(candidate.c_str(), X_OK) == 0 && fs::is_regular_file(candidate, ec);
#endif
        if (executableFile) {
            return candidate.string();
        }
        ec.clear();
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

auto FindTool(const Replay& replay) -> std::optional<Tool> {
#if defined(__APPLE__) || defined(_WIN32)
    constexpr Kind preference[] {Kind::Lldb, Kind::Gdb};
#else
    constexpr Kind preference[] {Kind::Gdb, Kind::Lldb};
#endif
    for (const Kind kind: preference) {
        if (auto path = FindOnWorkerPath(replay, kind == Kind::Lldb ? "lldb" : "gdb")) {
            return Tool {.kind = kind, .path = std::move(*path)};
        }
    }
    return std::nullopt;
}

#if defined(__APPLE__)
// LLDB's -o argument is a debugger command, NOT a shell command. Quote each
// DYLD assignment as a single debugger argument even when a path has spaces.
static auto QuoteLldbArgument(std::string_view text) -> std::string {
    std::string result = "\"";
    for (const char c: text) {
        if (c == '\\' || c == '"') {
            result.push_back('\\');
        }
        result.push_back(c);
    }
    result.push_back('"');
    return result;
}
#endif

auto BuildDebuggerArgv(const Tool& tool, const Replay& replay) -> std::vector<std::string> {
    std::vector<std::string> args {tool.path};
    if (tool.kind == Kind::Gdb) {
        args.insert(args.end(), {"-ex", "run", "-ex", "bt", "--args"});
    } else {
        args.insert(args.end(), {"-o", "settings set target.inherit-env true"});
#if defined(__APPLE__)
        // SIP removes DYLD_* from an Apple-protected /usr/bin/lldb process.
        // Explicitly set those on the *inferior*; the debugger itself inherits
        // the entire worker environment through execve below.
        std::string dyldSettings = "settings set target.env-vars";
        for (const auto& [key, value]: replay.environment) {
            if (key.starts_with("DYLD_")) {
                dyldSettings.push_back(' ');
                dyldSettings.append(QuoteLldbArgument(key + "=" + value));
            }
        }
        if (dyldSettings != "settings set target.env-vars") {
            args.insert(args.end(), {"-o", std::move(dyldSettings)});
        }
#endif
        args.insert(args.end(), {"-o", "run", "-o", "bt", "--"});
    }
    args.insert(args.end(), replay.argv.begin(), replay.argv.end());
    return args;
}

auto HasInteractiveTerminal() noexcept -> bool {
#if defined(_WIN32)
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0
        && GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
#else
    const int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const bool interactive = ::isatty(fd) != 0;
    ::close(fd);
    return interactive;
#endif
}

auto IsYes(std::string_view answer) noexcept -> bool {
    return EqualAsciiNoCase(answer, "y") || EqualAsciiNoCase(answer, "yes");
}

auto IsNo(std::string_view answer) noexcept -> bool {
    return answer.empty() || EqualAsciiNoCase(answer, "n") || EqualAsciiNoCase(answer, "no");
}

// No redirected stdin (including a pipe with a stray "y") can authorize a
// debugger. On POSIX the controlling terminal is used for both the question
// and the debugger even if ztest's stdout/stderr have been redirected.
auto Prompt() -> bool {
    if (!HasInteractiveTerminal()) {
        return false;
    }
#if defined(_WIN32)
    for (;;) {
        std::fputs("  > Launch debugger for this case? [Y/N]: ", stderr);
        std::fflush(stderr);
        char buffer[128] {};
        if (std::fgets(buffer, sizeof(buffer), stdin) == nullptr) {
            return false;
        }
        std::string_view reply {buffer};
        if (reply.find('\n') == std::string_view::npos) {
            int c = 0;
            while ((c = std::fgetc(stdin)) != '\n' && c != EOF) {}
        }
        while (!reply.empty() && std::isspace(static_cast<unsigned char>(reply.front()))) {
            reply.remove_prefix(1);
        }
        while (!reply.empty() && std::isspace(static_cast<unsigned char>(reply.back()))) {
            reply.remove_suffix(1);
        }
        if (IsYes(reply)) {
            return true;
        }
        if (IsNo(reply)) {
            return false;
        }
        std::fputs("  Please enter Y or N.\n", stderr);
    }
#else
    const int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    for (;;) {
        constexpr std::string_view question = "  > Launch debugger for this case? [Y/N]: ";
        std::size_t offset = 0;
        while (offset < question.size()) {
            const ssize_t n = ::write(fd, question.data() + offset, question.size() - offset);
            if (n <= 0) {
                ::close(fd);
                return false;
            }
            offset += static_cast<std::size_t>(n);
        }
        std::string answer;
        bool overflow = false;
        for (;;) {
            char ch = '\0';
            const ssize_t n = ::read(fd, &ch, 1);
            if (n <= 0) { // EOF or Ctrl-C: decline; ztest's signal handler handles the latter
                ::close(fd);
                return false;
            }
            if (ch == '\n' || ch == '\r') {
                break;
            }
            if (answer.size() < 127) {
                answer.push_back(ch);
            } else {
                overflow = true;
            }
        }
        const std::size_t first = answer.find_first_not_of(" \t");
        const std::string_view trimmed = first == std::string::npos ? std::string_view {}
            : std::string_view {answer}.substr(first, answer.find_last_not_of(" \t") - first + 1);
        if (!overflow && IsYes(trimmed)) {
            ::close(fd);
            return true;
        }
        if (!overflow && IsNo(trimmed)) {
            ::close(fd);
            return false;
        }
        constexpr std::string_view retry = "  Please enter Y or N.\n";
        (void)::write(fd, retry.data(), retry.size());
    }
#endif
}

#if defined(_WIN32)
// CreateProcess accepts one command line, not an argv array. Escape quotes and
// trailing backslashes using the Windows command-line parsing convention.
static auto QuoteWindowsArg(std::string_view arg) -> std::string {
    if (!arg.empty() && arg.find_first_of(" \t\n\"") == std::string_view::npos) {
        return std::string {arg};
    }
    std::string out = "\"";
    std::size_t slashes = 0;
    for (const char c: arg) {
        if (c == '\\') {
            ++slashes;
        } else if (c == '"') {
            out.append(slashes * 2 + 1, '\\');
            out.push_back('"');
            slashes = 0;
        } else {
            out.append(slashes, '\\');
            out.push_back(c);
            slashes = 0;
        }
    }
    out.append(slashes * 2, '\\');
    out.push_back('"');
    return out;
}
#endif

// An empty result means the debugger was launched and has exited (whether the
// replayed test passed or crashed does not change the *original* ztest result).
auto Launch(const Replay& replay) -> std::optional<std::string> {
    if (replay.argv.empty()) {
        return "cannot replay a case without a test binary";
    }
    const auto tool = FindTool(replay);
    if (!tool) {
        return "neither LLDB nor GDB was found on the worker PATH";
    }
    const std::vector<std::string> args = BuildDebuggerArgv(*tool, replay);

#if defined(_WIN32)
    std::string commandLine;
    for (const auto& arg: args) {
        if (!commandLine.empty()) {
            commandLine.push_back(' ');
        }
        commandLine.append(QuoteWindowsArg(arg));
    }
    std::string environmentBlock;
    for (const auto& [key, value]: replay.environment) {
        environmentBlock.append(key).push_back('=');
        environmentBlock.append(value).push_back('\0');
    }
    environmentBlock.push_back('\0');
    if (replay.environment.empty()) {
        environmentBlock.push_back('\0');
    }

    // Duplicate inheritable console handles even if the caller redirected
    // stdout (the prompt uses stderr). No debugger output goes into a pipe.
    HANDLE childInput = nullptr;
    HANDLE childOutput = nullptr;
    const HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, GetStdHandle(STD_INPUT_HANDLE), self, &childInput, 0, TRUE, DUPLICATE_SAME_ACCESS)
        || !DuplicateHandle(self, GetStdHandle(STD_ERROR_HANDLE), self, &childOutput, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        const DWORD error = GetLastError();
        if (childInput != nullptr) {
            CloseHandle(childInput);
        }
        return "could not duplicate debugger console (Win32 error " + std::to_string(error) + ")";
    }
    STARTUPINFOA startup {};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = childInput;
    startup.hStdOutput = childOutput;
    startup.hStdError  = childOutput;
    PROCESS_INFORMATION process {};
    const BOOL ok = CreateProcessA(
        tool->path.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, environmentBlock.data(),
        replay.workingDir.empty() ? nullptr : replay.workingDir.c_str(), &startup, &process
    );
    const DWORD createError = ok ? 0 : GetLastError();
    CloseHandle(childInput);
    CloseHandle(childOutput);
    if (!ok) {
        return "failed to start debugger (CreateProcess error " + std::to_string(createError) + ")";
    }
    const DWORD waited    = WaitForSingleObject(process.hProcess, INFINITE);
    const DWORD waitError = waited == WAIT_OBJECT_0 ? 0 : GetLastError();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0) {
        return "failed while waiting for debugger (Win32 error " + std::to_string(waitError) + ")";
    }
    return std::nullopt;
#else
    const int tty = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty < 0) {
        return "interactive terminal went away before the debugger could start";
    }

    std::vector<char*> argvPtrs;
    argvPtrs.reserve(args.size() + 1);
    for (const auto& arg: args) {
        argvPtrs.push_back(const_cast<char*>(arg.c_str()));
    }
    argvPtrs.push_back(nullptr);
    std::vector<std::string> envStorage;
    envStorage.reserve(replay.environment.size());
    for (const auto& [key, value]: replay.environment) {
        envStorage.push_back(key + "=" + value);
    }
    std::vector<char*> envPtrs;
    envPtrs.reserve(envStorage.size() + 1);
    for (const auto& entry: envStorage) {
        envPtrs.push_back(const_cast<char*>(entry.c_str()));
    }
    envPtrs.push_back(nullptr);

    // LLDB/GDB and ztest share the terminal's foreground process group. Ignore
    // terminal Ctrl-C/Ctrl-\\ in the *parent* while the debugger owns the prompt;
    // reset both signals in the child so they still reach the debugger/inferior.
    struct sigaction ignored {}, priorInt {}, priorQuit {};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    const bool savedInt  = ::sigaction(SIGINT, &ignored, &priorInt) == 0;
    const bool savedQuit = ::sigaction(SIGQUIT, &ignored, &priorQuit) == 0;
    const pid_t pid = ::fork();
    const int forkError = errno;
    if (pid == 0) {
        struct sigaction defaultAction {};
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultAction.sa_mask);
        ::sigaction(SIGINT, &defaultAction, nullptr);
        ::sigaction(SIGQUIT, &defaultAction, nullptr);
        if (::dup2(tty, STDIN_FILENO) < 0 || ::dup2(tty, STDOUT_FILENO) < 0 || ::dup2(tty, STDERR_FILENO) < 0) {
            ::_exit(127);
        }
        if (tty > STDERR_FILENO) {
            ::close(tty);
        }
        if (!replay.workingDir.empty()) {
            (void)::chdir(replay.workingDir.c_str()); // match the regular worker's fallback on chdir failure
        }
        ::execve(tool->path.c_str(), argvPtrs.data(), envPtrs.data());
        constexpr char error[] = "ztest: could not exec debugger\n";
        (void)::write(STDERR_FILENO, error, sizeof(error) - 1);
        ::_exit(127);
    }
    ::close(tty);
    int status = 0;
    int waitError = 0;
    if (pid > 0) {
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                waitError = errno;
                break;
            }
        }
    }
    if (savedQuit) {
        ::sigaction(SIGQUIT, &priorQuit, nullptr);
    }
    if (savedInt) {
        ::sigaction(SIGINT, &priorInt, nullptr);
    }
    if (pid < 0) {
        return "could not fork debugger: " + std::string {std::strerror(forkError)};
    }
    if (waitError != 0) {
        return "failed while waiting for debugger: " + std::string {std::strerror(waitError)};
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return std::nullopt;
    }
    if (WIFEXITED(status)) {
        return "debugger exited with status " + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return "debugger terminated by signal " + std::to_string(WTERMSIG(status));
    }
    return "debugger ended unexpectedly";
#endif
}

} // namespace Ztest::Debugger
