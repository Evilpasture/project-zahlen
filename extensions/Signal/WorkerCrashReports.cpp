// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Worker-side crash reporting. See <Signal/WorkerCrashReports.hpp> for the
// stream grammar and the reasoning behind the framing.
//
// Everything below exists so that the fatal-signal path can run without
// allocating: fixed buffers, lock-free atomics, and a loaded-module table that
// is captured once at install time (dl_iterate_phdr takes the loader lock, so it
// must never be called from a handler).

#include <Signal/WorkerCrashReports.hpp>
#include <Zahlen/Core/Format.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
    #include <errno.h>
    #include <signal.h>
    #include <sys/mman.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
    #if defined(__linux__)
        #include <execinfo.h>
        #include <link.h>
        #include <sys/syscall.h>
    #elif defined(__APPLE__)
        #include <execinfo.h>
        #include <mach-o/dyld.h>
        #include <mach-o/loader.h>
        #include <pthread.h>
    #endif
#else
    #include <io.h>
#endif

namespace ZHLN {
namespace {

constexpr std::size_t kFrameCapacity  = 64;
constexpr std::size_t kModuleCapacity = 96;
constexpr std::size_t kModuleNameMax  = 192;
constexpr std::size_t kReportCapacity = 32 * 1024;
constexpr std::size_t kAltStackBytes  = 64 * 1024;

constexpr int kStdinFd  = 0;
constexpr int kStdoutFd = 1;
constexpr int kStderrFd = 2;

void WriteAll(int fd, std::string_view text) noexcept {
    while (!text.empty()) {
#if defined(_WIN32)
        const int written = ::_write(fd, text.data(), static_cast<unsigned int>(text.size()));
#else
        const ssize_t written = ::write(fd, text.data(), text.size());
#endif
        if (written > 0) {
            text.remove_prefix(static_cast<std::size_t>(written));
            continue;
        }
#if !defined(_WIN32)
        if (written < 0 && errno == EINTR) {
            continue;
        }
#endif
        return; // Best effort: never fall back to a non-signal-safe writer.
    }
}

} // namespace

#if defined(__unix__) || defined(__APPLE__)

namespace {

struct CrashModuleRange {
    std::uintptr_t                   base  = 0;
    std::uintptr_t                   end   = 0;
    std::uintptr_t                   slide = 0; // non-zero on Mach-O only
    std::array<char, kModuleNameMax> name {};
};

// Attribution of the case that is currently running in this worker. The runner
// owns the pointed-to buffers: they are `std::string` members of the runner that
// outlive the case being executed, which is the same contract the per-case
// SIGALRM handler in tests/TestsFramework.hpp already relies on.
std::atomic<const char*> g_caseTag {nullptr};
std::atomic<std::size_t> g_caseTagLength {0};
std::atomic<const char*> g_caseNode {nullptr};
std::atomic<std::size_t> g_caseNodeLength {0};
std::atomic<bool>        g_caseOpen {false};

std::atomic<bool> g_reportsEnabled {false};
std::atomic<bool> g_forkedFilter {true};
std::atomic<bool> g_insideReport {false};

std::array<CrashModuleRange, kModuleCapacity> g_modules {};
std::atomic<std::size_t>                      g_moduleCount {0};

std::array<char, kReportCapacity> g_reportScratch {};
std::array<char, kAltStackBytes>  g_altStack {};

std::array<struct sigaction, 5> g_previousActions {};
std::array<bool, 5>             g_havePreviousActions {};
std::terminate_handler          g_previousTerminate {nullptr};
bool                            g_altStackInstalled {false};

static_assert(std::atomic<const char*>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);

void AppendBytes(char* dst, std::size_t& len, std::size_t cap, std::string_view text) noexcept {
    const std::size_t room = (len < cap) ? (cap - len) : 0;
    const std::size_t take = (text.size() < room) ? text.size() : room;
    for (std::size_t i = 0; i < take; ++i) {
        dst[len + i] = text[i];
    }
    len += take;
}

// Tab and newline terminate a field in the @@ZTEST grammar, so they are kept out
// of the text columns: an arbitrary module path must not reshape a record.
auto SanitizeField(std::string_view in, char* out, std::size_t cap) noexcept -> std::string_view {
    std::size_t len = 0;
    for (char c: in) {
        if (len + 1 >= cap) {
            break;
        }
        out[len++] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    return std::string_view {out, len};
}

#if defined(__linux__)

// dl_iterate_phdr takes the loader lock, so this only ever runs from
// RecordLoadedModules at install time -- never from a signal handler.
auto ModuleCallback(dl_phdr_info* info, std::size_t /*size*/, void* /*data*/) noexcept -> int {
    if (info == nullptr) {
        return 1;
    }
    const std::size_t index = g_moduleCount.load(std::memory_order::relaxed);
    if (index >= kModuleCapacity) {
        return 1; // Non-zero stops dl_iterate_phdr.
    }

    const auto     base = static_cast<std::uintptr_t>(info->dlpi_addr);
    std::uintptr_t end  = base;
    for (std::size_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        // p_vaddr carries the link-time address, which is non-zero on a
        // prelinked/PIE image; adding it to the load bias is what makes the
        // range comparable with a live return address.
        const auto segmentEnd = base + static_cast<std::uintptr_t>(phdr.p_vaddr) + static_cast<std::uintptr_t>(phdr.p_memsz);
        if (segmentEnd > end) {
            end = segmentEnd;
        }
    }
    if (end <= base) {
        return 0;
    }

    char                   sanitized[kModuleNameMax] {};
    const std::string_view rawName = (info->dlpi_name != nullptr) ? std::string_view {info->dlpi_name} : std::string_view {};
    const std::string_view name    = SanitizeField(rawName, sanitized, sizeof(sanitized));

    auto& range = g_modules[index];
    range.base  = base;
    range.end   = end;
    range.name.fill('\0');
    for (std::size_t i = 0; i < name.size() && i + 1 < range.name.size(); ++i) {
        range.name[i] = name[i];
    }
    g_moduleCount.store(index + 1, std::memory_order::release);
    return 0;
}

#elif defined(__APPLE__)

// `mach_header` holds no segment array: the header is followed by a run of load
// commands, and each segment's range lives in an LC_SEGMENT(_64) command inside
// that run. Every command carries its own `cmdsize`, so walking the array means
// stepping by `cmdsize`; the header magic says whether the commands are 32- or
// 64-bit. Commands are only 4-byte aligned, so they are copied out of the stream
// instead of being reinterpret_cast in place.
// MH_MAGIC_64 from <mach-o/loader.h>; spelled out so the 32-bit fallback
// below is readable next to it.
constexpr std::uint32_t kMachMagic64 = 0xfeedfacfU;

// segname is a NUL-padded 16-byte field, so it cannot be compared as a C string
// unless the image happens to have a terminator inside the field.
auto IsTextSegmentName(const char (&segname)[16]) noexcept -> bool {
    constexpr std::string_view kText = "__TEXT";
    for (std::size_t i = 0; i < kText.size(); ++i) {
        if (segname[i] != kText[i]) {
            return false;
        }
    }
    return segname[kText.size()] == '\0';
}

// The image's [base, end) in *runtime* addresses. A segment's vmaddr is a
// link-time address, so the dyld slide has to be added before it can be compared
// with a return address. `__TEXT` anchors `base` -- it is always the first
// segment of an image -- while `end` spans every segment, which is what makes a
// lookup hit code as well as data.
auto MeasureMachOImage(const mach_header* header, std::uintptr_t slide, std::uintptr_t& outBase, std::uintptr_t& outEnd) noexcept -> bool {
    if (header == nullptr) {
        return false;
    }

    // The 32- and 64-bit headers are different sizes (28 vs 32 bytes), so the
    // command array does not start at the same offset for both.
    const bool           is64     = (header->magic == kMachMagic64);
    const std::uintptr_t commands = reinterpret_cast<std::uintptr_t>(header) + (is64 ? sizeof(mach_header_64) : sizeof(mach_header));
    std::uintptr_t       end      = 0;
    std::size_t          offset   = 0;

    for (std::uint32_t i = 0; i < header->ncmds; ++i) {
        const auto* raw = reinterpret_cast<const char*>(commands + offset);

        load_command command {};
        std::memcpy(&command, raw, sizeof(command));
        if (command.cmdsize < sizeof(load_command)) {
            break; // Malformed image: stop instead of walking off the command run.
        }

        std::uintptr_t vmaddr  = 0;
        std::uintptr_t vmsize  = 0;
        std::int32_t   maxProt = 0;
        bool           isText  = false;
        bool           hit     = false;

        if (is64 && command.cmd == LC_SEGMENT_64) {
            segment_command_64 segment {};
            std::memcpy(&segment, raw, sizeof(segment));
            vmaddr  = static_cast<std::uintptr_t>(segment.vmaddr);
            vmsize  = static_cast<std::uintptr_t>(segment.vmsize);
            maxProt = segment.maxprot;
            isText  = IsTextSegmentName(segment.segname);
            hit     = true;
        } else if (!is64 && command.cmd == LC_SEGMENT) {
            segment_command segment {};
            std::memcpy(&segment, raw, sizeof(segment));
            vmaddr  = static_cast<std::uintptr_t>(segment.vmaddr);
            vmsize  = static_cast<std::uintptr_t>(segment.vmsize);
            maxProt = segment.maxprot;
            isText  = IsTextSegmentName(segment.segname);
            hit     = true;
        }

        // __PAGEZERO is a segment of zero permissions covering the low address
        // space: it is in the command list but it is not mapped, and letting its
        // (huge) vmsize extend `end` would swallow every library loaded below.
        if (hit && maxProt != 0) {
            const std::uintptr_t runtime = vmaddr + slide;
            if (runtime + vmsize > end) {
                end = runtime + vmsize;
            }
            if (isText) {
                outBase = runtime;
            }
        }

        offset += command.cmdsize;
    }

    if (outBase == 0 || end <= outBase) {
        return false;
    }
    outEnd = end;
    return true;
}

#endif

void RecordLoadedModules() noexcept {
    g_moduleCount.store(0, std::memory_order::relaxed);
#if defined(__linux__)
    ::dl_iterate_phdr(&ModuleCallback, nullptr);
#elif defined(__APPLE__)
    const std::uint32_t imageCount = _dyld_image_count();
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        const auto* header = _dyld_get_image_header(i);
        if (header == nullptr) {
            break;
        }
        const std::size_t index = g_moduleCount.load(std::memory_order::relaxed);
        if (index >= kModuleCapacity) {
            break;
        }

        std::uintptr_t base  = 0;
        std::uintptr_t end   = 0;
        const auto     slide = static_cast<std::uintptr_t>(_dyld_get_image_vmaddr_slide(i));
        if (!MeasureMachOImage(header, slide, base, end)) {
            continue;
        }

        const char* const imageName = _dyld_get_image_name(i);

        char                   sanitized[kModuleNameMax] {};
        const std::string_view name = SanitizeField((imageName != nullptr) ? std::string_view {imageName} : std::string_view {}, sanitized, sizeof(sanitized));

        auto& range = g_modules[index];
        range.base  = base;
        range.end   = end;
        range.slide = slide;
        range.name.fill('\0');
        for (std::size_t c = 0; c < name.size() && c + 1 < range.name.size(); ++c) {
            range.name[c] = name[c];
        }
        g_moduleCount.store(index + 1, std::memory_order::release);
    }
#endif

    // Order by base so a frame lookup is one pass; the loader reports load
    // order, which is not address order.
    const std::size_t count = g_moduleCount.load(std::memory_order::acquire);
    for (std::size_t i = 1; i < count; ++i) {
        CrashModuleRange key = g_modules[i];
        std::size_t      j   = i;
        while (j > 0 && g_modules[j - 1].base > key.base) {
            g_modules[j] = g_modules[j - 1];
            --j;
        }
        g_modules[j] = key;
    }
}

auto ModuleForAddress(std::uintptr_t address, std::uintptr_t& outOffset) noexcept -> const char* {
    const std::size_t count = g_moduleCount.load(std::memory_order::acquire);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& mod = g_modules[i];
        if (address >= mod.base && address < mod.end) {
#if defined(__APPLE__)
            // Mach-O symbolizers (atos, llvm-addr2line) take the *link-time*
            // vmaddr, not a distance from the image base: a dylib's __TEXT can
            // sit above 0, and the segment layouts of arm64 and x86_64 differ.
            // vmaddr + slide is the runtime address, so subtracting the slide
            // yields the vmaddr the symbol tables were built against.
            outOffset = address - mod.slide;
#else
            // ELF is the opposite: every image is linked at 0, so the distance
            // from the base *is* the link-time address addr2line expects.
            outOffset = address - mod.base;
#endif
            return mod.name.data();
        }
    }
    outOffset = 0;
    return ""; // The main executable: dladdr and /proc report it with no name.
}

struct ReportSink {
    char*       data     = nullptr;
    std::size_t capacity = 0;
    std::size_t length   = 0;

    void Append(std::string_view text) noexcept {
        AppendBytes(data, length, capacity, text);
    }

    template <typename... Args>
    void AppendFormatted(std::string_view fmt, Args&&... args) noexcept {
        if (length + 1 >= capacity) {
            return;
        }
        length += ZHLN::FormatTo(data + length, capacity - length, fmt, args...).size();
    }

    [[nodiscard]] auto view() const noexcept -> std::string_view {
        return std::string_view {data, length};
    }
};

// The worker shares its stdout/stderr pipe with ztest, and ztest stops reading
// that pipe as soon as it sees EOF on it -- which an unrelated `close(1)` on
// another thread of the test process is enough to cause, because only the last
// descriptor keeps the pipe open. A trace written straight to fd 2 can then be
// cut off half way, and a truncated report reads as "the worker froze" instead
// of "the worker crashed". Writing through a forked relay decouples the two: the
// child owns the pipe end until every byte is out, it never runs test code, and
// it therefore also survives a process whose heap or lock state is corrupt.
void WriteThroughForkedRelay(std::string_view report) noexcept {
    int pipeFds[2] {-1, -1};
    if (::pipe(pipeFds) != 0) {
        WriteAll(kStderrFd, report);
        return;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        WriteAll(kStderrFd, report);
        return;
    }

    if (pid == 0) {
        ::close(pipeFds[1]);

        // Only SIGPIPE is ignored: it is the one signal this child can plausibly
        // take (ztest going away mid-write), and EPIPE is a normal exit. Every
        // other fatal signal is put back to its default action so a fault in the
        // relay kills the relay instead of recursing into a crash reporter whose
        // state this child inherited half-written.
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ignore.sa_flags = 0;
        ::sigaction(SIGPIPE, &ignore, nullptr);

        struct sigaction restore {};
        restore.sa_handler = SIG_DFL;
        sigemptyset(&restore.sa_mask);
        restore.sa_flags = 0;
        ::sigaction(SIGSEGV, &restore, nullptr);
        ::sigaction(SIGBUS, &restore, nullptr);
        ::sigaction(SIGABRT, &restore, nullptr);
        ::sigaction(SIGILL, &restore, nullptr);
        ::sigaction(SIGFPE, &restore, nullptr);

        // The read end becomes stdin; stdout/stderr keep pointing at the pipe
        // `ztest` is reading, which is why they are duplicated before anything
        // else happens to the descriptor table.
        const int outFd = ::dup(kStdoutFd);
        const int errFd = ::dup(kStderrFd);

        if (pipeFds[0] != kStdinFd) {
            ::dup2(pipeFds[0], kStdinFd);
        }
        if (pipeFds[0] > kStderrFd) {
            ::close(pipeFds[0]);
        }

        char buffer[4096];
        while (true) {
            const ssize_t got = ::read(kStdinFd, buffer, sizeof(buffer));
            if (got > 0) {
                // Exactly one descriptor: a ztest worker has stdout and stderr
                // dup2'd onto the same pipe, so writing to both would print the
                // report twice. stderr wins because a report is a diagnostic;
                // stdout is only a fallback for a worker that closed fd 2.
                const std::string_view chunk {buffer, static_cast<std::size_t>(got)};
                if (errFd >= 0) {
                    WriteAll(errFd, chunk);
                } else if (outFd >= 0) {
                    WriteAll(outFd, chunk);
                }
                continue;
            }
            if (got < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        ::_exit(0);
    }

    ::close(pipeFds[0]);
    WriteAll(pipeFds[1], report);
    ::close(pipeFds[1]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

void ColdReportPath(std::string_view report) noexcept {
    if (g_forkedFilter.load(std::memory_order::relaxed)) {
        WriteThroughForkedRelay(report);
    } else {
        WriteAll(kStderrFd, report);
    }
}

auto CaptureFrames(std::array<void*, kFrameCapacity>& frames, int skip) noexcept -> int {
    int count = ::backtrace(frames.data(), static_cast<int>(frames.size()));
    if (count <= 0) {
        return 0;
    }
    const int dropped = (skip < count) ? skip : count;
    if (dropped > 0) {
        for (int i = dropped; i < count; ++i) {
            frames[static_cast<std::size_t>(i - dropped)] = frames[static_cast<std::size_t>(i)];
        }
        count -= dropped;
    }
    return count;
}

auto CurrentThreadId() noexcept -> std::uint64_t {
#if defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
    std::uint64_t tid = 0;
    ::pthread_threadid_np(nullptr, &tid);
    return tid;
#else
    return 0;
#endif
}

} // namespace

void EmitWorkerCrashReport(std::string_view reason, std::uintptr_t faultAddress, int skipFrames) noexcept {
    if (!g_reportsEnabled.load(std::memory_order::acquire)) {
        return;
    }
    if (g_insideReport.exchange(true, std::memory_order::acq_rel)) {
        return; // A second fatal signal while the first report is being written.
    }

    ReportSink sink {.data = g_reportScratch.data(), .capacity = g_reportScratch.size(), .length = 0};

    const char* const      caseTagPtr  = g_caseTag.load(std::memory_order::acquire);
    const std::size_t      caseTagLen  = g_caseTagLength.load(std::memory_order::relaxed);
    const char* const      caseNodePtr = g_caseNode.load(std::memory_order::acquire);
    const std::size_t      caseNodeLen = g_caseNodeLength.load(std::memory_order::relaxed);
    const std::string_view caseTag     = (caseTagPtr != nullptr) ? std::string_view {caseTagPtr, caseTagLen} : std::string_view {};
    const std::string_view caseNode    = (caseNodePtr != nullptr) ? std::string_view {caseNodePtr, caseNodeLen} : std::string_view {};

    // Close the in-flight case before the trace, so the orchestrator files the
    // report against this node ID instead of swallowing it as engine log noise.
    // The exchange makes the synthetic terminator happen at most once, even if
    // the runner was killed right after writing its own.
    const bool closedCase = g_caseOpen.exchange(false, std::memory_order::acq_rel);
    if (closedCase && !caseNode.empty()) {
        sink.Append(kWorkerCaseEndMarker);
        sink.Append(caseTag);
        sink.Append("\tCRASHED\t0\n");
    }

    char                   reasonField[96] {};
    const std::string_view reasonText = SanitizeField(reason, reasonField, sizeof(reasonField));

    sink.Append(kWorkerTraceBeginMarker);
    sink.Append(caseNode);
    sink.AppendFormatted("\t{}\t", CurrentThreadId());
    sink.Append(reasonText);
    sink.Append("\t");
    sink.AppendFormatted("{:16x}", static_cast<unsigned long long>(faultAddress));
    sink.Append("\n");

    if (closedCase && !caseNode.empty()) {
        sink.Append(kWorkerTraceNoteMarker);
        sink.Append("case was still in flight: it never reached @@ZTEST END\n");
    }

    std::array<void*, kFrameCapacity> frames {};
    const int                         frameCount = CaptureFrames(frames, skipFrames);
    if (frameCount == 0) {
        sink.Append(kWorkerTraceNoteMarker);
        sink.Append("no frames captured: backtrace() returned nothing (missing unwind tables?)\n");
    }

    for (int i = 0; i < frameCount; ++i) {
        const auto     address = reinterpret_cast<std::uintptr_t>(frames[static_cast<std::size_t>(i)]);
        std::uintptr_t offset  = 0;
        const char*    module  = ModuleForAddress(address, offset);

        char                   moduleField[kModuleNameMax] {};
        const std::string_view moduleText = SanitizeField(module, moduleField, sizeof(moduleField));

        sink.Append(kWorkerTraceFrameMarker);
        sink.AppendFormatted("{}\t{:16x}\t", i, static_cast<unsigned long long>(address));
        sink.Append(moduleText);
        sink.AppendFormatted("\t{:16x}\n", static_cast<unsigned long long>(offset));
    }

    sink.Append(kWorkerTraceEndMarker);
    sink.Append(caseNode);
    sink.AppendFormatted("\t{}\n", frameCount);

    ColdReportPath(sink.view());
    g_insideReport.store(false, std::memory_order::release);
}

namespace {

void FatalSignalHandler(int sig, siginfo_t* info, void* /*ucontext*/) noexcept {
    std::string_view reason = "fatal signal";
    switch (sig) {
        case SIGSEGV:
            reason = "SIGSEGV";
            break;
        case SIGILL:
            reason = "SIGILL";
            break;
        case SIGFPE:
            reason = "SIGFPE";
            break;
        case SIGBUS:
            reason = "SIGBUS";
            break;
        case SIGABRT:
            reason = "SIGABRT";
            break;
        default:
            break;
    }

    // si_addr is the faulting address for SIGSEGV/SIGBUS/SIGILL/SIGFPE and is
    // meaningless for SIGABRT (glibc reports a pid there), so the reason string
    // and the address have to agree before the address is shown at all.
    const std::uintptr_t faultAddress = (sig != SIGABRT && info != nullptr) ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;

    // Two frames to skip: this handler is entered from the kernel-dispatched
    // signal trampoline, and EmitWorkerCrashReport is one frame below it.
    EmitWorkerCrashReport(reason, faultAddress, 2);
    ::_exit(kSignalExitCodeBase + sig);
}

void OnStdTerminate() noexcept {
    // Three frames: OnStdTerminate, __cxa_throw/std::terminate's call site, and
    // the terminate trampoline.
    EmitWorkerCrashReport("std::terminate", 0, 3);
    ::_exit(kSignalExitCodeBase + SIGABRT);
}

constexpr std::array<int, 5> kCrashSignals {SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGABRT};

} // namespace

void InstallWorkerCrashReports(bool forkedFilter) {
    if (g_reportsEnabled.exchange(true, std::memory_order::acq_rel)) {
        return;
    }
    g_forkedFilter.store(forkedFilter, std::memory_order::release);
    RecordLoadedModules();

    // A stack overflow leaves no room for a handler on the faulting stack.
    if (!g_altStackInstalled) {
        stack_t altStack {};
        altStack.ss_sp      = g_altStack.data();
        altStack.ss_size    = g_altStack.size();
        altStack.ss_flags   = 0;
        g_altStackInstalled = ::sigaltstack(&altStack, nullptr) == 0;
    }

    struct sigaction action {};
    action.sa_sigaction = &FatalSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;

    for (std::size_t i = 0; i < kCrashSignals.size(); ++i) {
        g_havePreviousActions[i] = ::sigaction(kCrashSignals[i], &action, &g_previousActions[i]) == 0;
    }

    g_previousTerminate = std::set_terminate(&OnStdTerminate);
}

void UninstallWorkerCrashReports() {
    if (!g_reportsEnabled.exchange(false, std::memory_order::acq_rel)) {
        return;
    }
    for (std::size_t i = 0; i < kCrashSignals.size(); ++i) {
        if (g_havePreviousActions[i]) {
            ::sigaction(kCrashSignals[i], &g_previousActions[i], nullptr);
            g_havePreviousActions[i] = false;
        }
    }
    if (g_previousTerminate != nullptr) {
        std::set_terminate(g_previousTerminate);
        g_previousTerminate = nullptr;
    }
    if (g_altStackInstalled) {
        stack_t disable {};
        disable.ss_flags = SS_DISABLE;
        ::sigaltstack(&disable, nullptr);
        g_altStackInstalled = false;
    }
    g_caseOpen.store(false, std::memory_order::release);
}

auto WorkerCrashReportsInstalled() noexcept -> bool {
    return g_reportsEnabled.load(std::memory_order::acquire);
}

namespace {

void ArmCaseAttribution(const char* tag, std::size_t tagLength, std::string_view nodeId) noexcept {
    g_caseTagLength.store(tagLength, std::memory_order::relaxed);
    g_caseNodeLength.store(nodeId.size(), std::memory_order::relaxed);
    g_caseNode.store(nodeId.data(), std::memory_order::release);
    g_caseTag.store(tag, std::memory_order::release);
    g_caseOpen.store(true, std::memory_order::release);
}

void DisarmCaseAttribution() noexcept {
    g_caseOpen.store(false, std::memory_order::release);
    g_caseNode.store(nullptr, std::memory_order::release);
    g_caseNodeLength.store(0, std::memory_order::relaxed);
    g_caseTag.store(nullptr, std::memory_order::release);
    g_caseTagLength.store(0, std::memory_order::relaxed);
}

} // namespace

#else // !(__unix__ || __APPLE__)

// Fatal-signal capture is POSIX-only. The case lifecycle below is not, so a host
// without it still emits well-formed BEGIN/END records -- only the frames go
// missing.

void InstallWorkerCrashReports(bool /*forkedFilter*/) {
}

void UninstallWorkerCrashReports() {
}

auto WorkerCrashReportsInstalled() noexcept -> bool {
    return false;
}

void EmitWorkerCrashReport(std::string_view /*reason*/, std::uintptr_t /*faultAddress*/, int /*skipFrames*/) noexcept {
}

namespace {

void ArmCaseAttribution(const char* /*tag*/, std::size_t /*tagLength*/, std::string_view /*nodeId*/) noexcept {
}

void DisarmCaseAttribution() noexcept {
}

} // namespace

#endif

namespace {

// `relFile \t nodeId`, rebuilt at every case start. The handler borrows it, so it
// lives at namespace scope rather than on the runner's stack.
char g_caseTagBuffer[kModuleNameMax * 2] {};

} // namespace

void BeginWorkerCase(std::string_view relFile, std::string_view nodeId) noexcept {
    const std::size_t cap = sizeof(g_caseTagBuffer) - 1;
    std::size_t       len = 0;

    AppendBytes(g_caseTagBuffer, len, cap, relFile);
    AppendBytes(g_caseTagBuffer, len, cap, "\t");
    AppendBytes(g_caseTagBuffer, len, cap, nodeId);
    g_caseTagBuffer[len] = '\0';

    // Publish only after the bytes are in place; the release store is what makes
    // a handler interrupting this thread see a complete buffer.
    ArmCaseAttribution(g_caseTagBuffer, len, nodeId);

    WriteAll(kStdoutFd, kWorkerCaseBeginMarker);
    WriteAll(kStdoutFd, std::string_view {g_caseTagBuffer, len});
    WriteAll(kStdoutFd, "\n");
}

void EndWorkerCase(std::string_view relFile, std::string_view nodeId, std::string_view status, long long durationUs) noexcept {
    DisarmCaseAttribution();

    char              durationField[32] {};
    const std::size_t durationLength = ZHLN::FormatTo(durationField, sizeof(durationField), "{}", durationUs).size();

    WriteAll(kStdoutFd, kWorkerCaseEndMarker);
    WriteAll(kStdoutFd, relFile);
    WriteAll(kStdoutFd, "\t");
    WriteAll(kStdoutFd, nodeId);
    WriteAll(kStdoutFd, "\t");
    WriteAll(kStdoutFd, status);
    WriteAll(kStdoutFd, "\t");
    WriteAll(kStdoutFd, std::string_view {durationField, durationLength});
    WriteAll(kStdoutFd, "\n");
}

} // namespace ZHLN
