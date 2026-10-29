// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Signal/StackSymbolizer.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Platform.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <dbghelp.h>
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace ZHLN {
namespace {

struct ResolvedLocation {
    std::string   function;
    std::string   file;
    std::uint32_t line = 0;
};

auto TrimAscii(std::string_view in) noexcept -> std::string_view {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!in.empty() && isSpace(in.front())) {
        in.remove_prefix(1);
    }
    while (!in.empty() && isSpace(in.back())) {
        in.remove_suffix(1);
    }
    return in;
}

auto ParseUnsigned(std::string_view text, unsigned long long& out) noexcept -> bool {
    const std::string_view trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), out);
    return ec == std::errc {} && ptr == trimmed.data() + trimmed.size();
}

// `0x0000000000001180` -- the per-address header that `addr2line -a` prints.
// The value is not used: addresses are requested in one order and expanded in
// that same order, so the header only has to delimit the inline chains.
auto LooksLikeAddressHeader(std::string_view line) noexcept -> bool {
    const std::string_view trimmed = TrimAscii(line);
    if (trimmed.size() < 3 || trimmed[0] != '0' || (trimmed[1] != 'x' && trimmed[1] != 'X')) {
        return false;
    }
    return std::all_of(trimmed.begin() + 2, trimmed.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

// `file:line` as printed by addr2line, possibly `file:line (discriminator N)`,
// `??:0` or `??:?`. The trailing number is what tells it apart from a symbol
// signature, so no column bookkeeping is needed to split the pairs.
auto LooksLikeLocation(std::string_view line) noexcept -> bool {
    const std::string_view trimmed = TrimAscii(line);
    const std::size_t      colon   = trimmed.rfind(':');
    if (colon == std::string_view::npos || colon + 1 >= trimmed.size()) {
        return false;
    }
    std::string_view tail = trimmed.substr(colon + 1);
    if (const std::size_t space = tail.find(' '); space != std::string_view::npos) {
        tail = tail.substr(0, space); // drop " (discriminator N)"
    }
    if (tail == "?") {
        return true;
    }
    return !tail.empty() && std::all_of(tail.begin(), tail.end(), [](char c) { return c >= '0' && c <= '9'; });
}

auto ParseLocation(std::string_view line) -> ResolvedLocation {
    ResolvedLocation       out;
    const std::string_view trimmed = TrimAscii(line);

    const std::size_t colon = trimmed.rfind(':');
    if (colon == std::string_view::npos) {
        std::string file {trimmed};
        if (file != "??") {
            out.file = std::move(file);
        }
        return out;
    }

    std::string_view tail = trimmed.substr(colon + 1);
    if (const std::size_t space = tail.find(' '); space != std::string_view::npos) {
        tail = tail.substr(0, space); // drop " (discriminator N)"
    }
    unsigned long long lineNumber = 0;
    if (ParseUnsigned(tail, lineNumber)) {
        out.line = static_cast<std::uint32_t>(lineNumber);
    }

    std::string_view file = trimmed.substr(0, colon);
    if (!file.empty() && file != "??") {
        out.file.assign(file);
    }
    return out;
}

auto FormatOffset(std::uintptr_t offset) -> std::string {
    char       buf[48] {};
    const auto written = ZHLN::FormatTo(buf, sizeof(buf), "+0x{:x}", static_cast<unsigned long long>(offset));
    return std::string {written.data(), written.size()};
}

auto FormatLocation(const ResolvedLocation& loc) -> std::string {
    std::string out;
    if (!loc.function.empty()) {
        out.append(loc.function);
    }
    if (!loc.file.empty()) {
        // libc and other stripped images often resolve to a symbol with no line
        // information; "foo() at " with a dangling " at " reads like a bug.
        if (!out.empty()) {
            out.append(" at ");
        }
        out.append(loc.file);
        if (loc.line != 0) {
            char       buf[24] {};
            const auto written = ZHLN::FormatTo(buf, sizeof(buf), ":{}", loc.line);
            out.append(written.data(), written.size());
        }
    }
    if (out.empty()) {
        out.append("??");
    }
    return out;
}

auto ModuleLabel(const WorkerStackFrame& frame) -> std::string {
    if (!frame.module.empty()) {
        return frame.module;
    }
    return std::string {"<main executable>"};
}

#if !defined(_WIN32)

auto IsExecutableFile(std::string_view path) noexcept -> bool {
    if (path.empty()) {
        return false;
    }
    std::string owned {path};
    return ::access(owned.c_str(), X_OK) == 0;
}

// Runs `exe args...` and returns everything it wrote to stdout. Deliberately a
// plain fork/exec with no shell: the paths come from the crashed process.
auto RunProcessCapture(std::string_view exe, const std::vector<std::string>& args) -> std::optional<std::string> {
    int pipeFds[2] {-1, -1};
    if (::pipe(pipeFds) != 0) {
        return std::nullopt;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        ::close(pipeFds[0]);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        if (pipeFds[1] > STDERR_FILENO) {
            ::close(pipeFds[1]);
        }
        const int devNull = ::open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDERR_FILENO);
        }

        std::string        exeStorage {exe};
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(exeStorage.data());
        for (const auto& a: args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        ::execvp(exeStorage.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(pipeFds[1]);
    std::string            output;
    std::array<char, 4096> buffer {};
    while (true) {
        const ssize_t got = ::read(pipeFds[0], buffer.data(), buffer.size());
        if (got > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(pipeFds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    return output;
}

auto SearchPath(std::string_view name) -> std::optional<std::string> {
    const char* pathVar = std::getenv("PATH");
    if (pathVar == nullptr || name.empty()) {
        return std::nullopt;
    }
    std::string_view paths {pathVar};
    while (!paths.empty()) {
        const std::size_t      sep  = paths.find(':');
        const std::string_view part = (sep == std::string_view::npos) ? paths : paths.substr(0, sep);
        if (!part.empty()) {
            std::string full {part};
            full.push_back('/');
            full.append(name);
            if (IsExecutableFile(full)) {
                return full;
            }
        }
        if (sep == std::string_view::npos) {
            break;
        }
        paths.remove_prefix(sep + 1);
    }
    return std::nullopt;
}

auto ResolveAddr2LineBinary() -> std::optional<std::string> {
    if (const char* overridePath = std::getenv("ADDR2LINE"); overridePath != nullptr && *overridePath != '\0') {
        if (IsExecutableFile(overridePath)) {
            return std::string {overridePath};
        }
    }
    for (const char* candidate: {"addr2line", "llvm-addr2line", "llvm-symbolizer", "eu-addr2line"}) {
        if (auto found = SearchPath(candidate); found.has_value()) {
            return found;
        }
    }
    return std::nullopt;
}

auto HexArgument(std::uintptr_t value) -> std::string {
    char       buf[32] {};
    const auto written = ZHLN::FormatTo(buf, sizeof(buf), "0x{:x}", static_cast<unsigned long long>(value));
    return std::string {written.data(), written.size()};
}

#endif // !_WIN32

// Splits `addr2line -a -f -C -i` output into one inline chain per requested
// address. The `-a` header is what makes this unambiguous: with `-i` an address
// expands into a variable number of (function, location) pairs, and nothing in
// the pairs themselves says where the next address begins.
auto ParseAddr2LineOutput(std::string_view output) -> std::vector<std::vector<ResolvedLocation>> {
    std::vector<std::vector<ResolvedLocation>> result;
    std::vector<std::string>                   lines;
    std::size_t                                pos = 0;
    while (pos <= output.size()) {
        const std::size_t nl = output.find('\n', pos);
        const auto        ln = (nl == std::string_view::npos) ? output.substr(pos) : output.substr(pos, nl - pos);
        lines.emplace_back(ln);
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }

    std::size_t i = 0;
    while (i < lines.size()) {
        if (LooksLikeAddressHeader(lines[i])) {
            ++i;
            result.emplace_back();
            continue;
        }
        if (result.empty()) {
            result.emplace_back(); // No -a header: be forgiving about it.
        }
        if (i + 1 >= lines.size()) {
            break;
        }
        std::string_view fn  = lines[i];
        std::string_view loc = lines[i + 1];
        if (LooksLikeLocation(fn) && !LooksLikeLocation(loc)) {
            std::swap(fn, loc);
        }

        ResolvedLocation       resolved = ParseLocation(loc);
        const std::string_view function = TrimAscii(fn);
        if (function != "??") {
            resolved.function.assign(function);
        }
        result.back().push_back(std::move(resolved));
        i += 2;
    }
    return result;
}

} // namespace

auto SymbolizeWorkerFrames(std::span<const WorkerStackFrame> frames, std::string_view mainExecutable) -> std::vector<SymbolizedFrame> {
    std::vector<SymbolizedFrame> rendered(frames.size());
    if (frames.empty()) {
        return rendered;
    }

    // Frames are grouped per module: one addr2line invocation per module rather
    // than one per frame keeps a 30-frame report in the low milliseconds.
    std::map<std::string, std::vector<std::size_t>> byModule;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const std::string module = frames[i].module.empty() ? std::string {mainExecutable} : frames[i].module;
        if (module.empty()) {
            continue;
        }
        byModule[module].push_back(i);
    }

    for (const auto& [module, indices]: byModule) {
#if defined(_WIN32)
        // Windows needs dbghelp against the matching .pdb; the module + offset
        // form is still actionable (`llvm-symbolizer` resolves it by hand).
        static_cast<void>(module);
        static_cast<void>(indices);
#else
        if (!IsExecutableFile(module)) {
            continue;
        }
        const auto addr2line = ResolveAddr2LineBinary();
        if (!addr2line.has_value()) {
            continue;
        }

        std::vector<std::string> args;
        args.emplace_back("-a"); // per-address header
        args.emplace_back("-f"); // function names
        args.emplace_back("-C"); // demangle
        args.emplace_back("-i"); // inline frames
        args.emplace_back("-e");
        args.push_back(module);
        for (std::size_t idx: indices) {
            args.push_back(HexArgument(frames[idx].moduleOffset));
        }

        const auto output = RunProcessCapture(*addr2line, args);
        if (!output.has_value()) {
            continue;
        }

        const auto resolved = ParseAddr2LineOutput(*output);
        for (std::size_t k = 0; k < indices.size() && k < resolved.size(); ++k) {
            const std::size_t target = indices[k];
            if (resolved[k].empty()) {
                continue;
            }
            const ResolvedLocation& head = resolved[k].front();
            if (head.function.empty() && head.file.empty()) {
                continue;
            }
            rendered[target].primary = "in " + FormatLocation(head);
            for (std::size_t j = 1; j < resolved[k].size(); ++j) {
                const ResolvedLocation& inlined = resolved[k][j];
                if (inlined.function.empty() && inlined.file.empty()) {
                    continue;
                }
                rendered[target].inlined.push_back(FormatLocation(inlined));
            }
        }
#endif
    }

    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!rendered[i].primary.empty()) {
            continue;
        }
        rendered[i].primary = "in " + ModuleLabel(frames[i]) + FormatOffset(frames[i].moduleOffset);
    }
    return rendered;
}

auto DescribeWorkerAddress(std::uintptr_t address, std::string_view module, std::uintptr_t moduleOffset) -> std::string {
    std::array<WorkerStackFrame, 1> single {WorkerStackFrame {
        .index        = 0,
        .address      = address,
        .module       = std::string {module},
        .moduleOffset = moduleOffset,
    }};
    const auto                      rendered = SymbolizeWorkerFrames(single, std::string_view {});
    if (rendered.empty()) {
        return {};
    }
    return rendered.front().primary;
}

} // namespace ZHLN
