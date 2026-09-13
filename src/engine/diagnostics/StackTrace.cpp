// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/diagnostics/StackTrace.cpp
//
// Platform backtracing and symbol demangling, and nothing else. Split out of
// AssertHandler.cpp because it is the one part of the diagnostics layer with
// per-platform state that has to be initialised exactly once, and because both
// the crash path and the ordinary panic path want frames.
//
// Two entry points, deliberately:
//
//   CaptureStackTrace     frames into a caller-provided buffer. The crash path
//                         uses this so the frames live on its own stack.
//   GetPoorMansStacktrace the public API from Zahlen/Log.hpp, unchanged in
//                         signature; now a thin wrapper over the above.
//
// What this fixes on Windows: the old GetPoorMansStacktrace called
// SymInitialize(GetCurrentProcess(), nullptr, true) on every single invocation
// and never called SymCleanup. DbgHelp keeps one symbol table per process, so
// each call tore down and rebuilt the table underneath whatever was walking it,
// and the symbol handles from the previous walk were never released. A crash
// dump that prints a trace per subsystem therefore reinitialized DbgHelp once
// per subsystem. Initialisation now happens once, from
// InitializeSymbolResolver(), which SetupSignalHandler calls while the process
// is still healthy. SymFromAddr's return value is also checked now -- it was
// ignored before, so a failed lookup printed whatever was left in the buffer
// from the previous frame and read like a real symbol name.

#include "diagnostics/DiagnosticsInternal.hpp"
#include <Zahlen/Core/Platform.hpp> // windows.h on Windows
#include <Zahlen/Log.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // std::free
#include <cstring>
#include <string>
#include <string_view>

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

namespace ZHLN::Diagnostics {

// The hard ceiling on frames per trace. Sized as an array rather than a vector
// because this runs from a signal handler; 128 pointers is 1 KB of stack, which
// is what the trace this replaced already reserved. Named from GetPoorMansStacktrace
// below, so it sits outside the unnamed namespace.
constexpr int kMaxFrames = 128;

namespace {

// A demangled C++ name can run long, but a fixed buffer keeps demangling off the
// heap. __cxa_demangle only allocates when the buffer it is handed is too small,
// and the caller frees that case below.
constexpr size_t kDemangleCapacity = 1024;

// Longest mangled name worth attempting. Anything longer is left mangled rather
// than truncated, since a half-demangled name is worse than an honest one.
constexpr size_t kMaxMangledLength = 512;

// Appends to a caller's buffer without ever writing past it, and without
// involving the heap. Returns how many bytes were placed.
class BufferAppender {
public:
    BufferAppender(char* buf, size_t capacity) noexcept: _buf(buf), _capacity(capacity) {}

    void Append(std::string_view text) noexcept {
        const size_t room = (_len < _capacity) ? (_capacity - _len) : 0;
        const size_t take = (text.size() < room) ? text.size() : room;
        if (take > 0) {
            std::memcpy(_buf + _len, text.data(), take);
        }
        _len += take;
    }

    void AppendUInt(uint64_t value) noexcept {
        char digits[24] {};
        int  pos = static_cast<int>(sizeof(digits));
        if (value == 0) {
            digits[--pos] = '0';
        }
        while (value > 0 && pos > 0) {
            digits[--pos] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        Append(std::string_view(digits + pos, sizeof(digits) - static_cast<size_t>(pos)));
    }

    void AppendHex(uint64_t value) noexcept {
        static constexpr char kDigits[] = "0123456789abcdef";

        // Digits come out least-significant first, so build them into a scratch
        // buffer and copy across in reverse. 16 nibbles covers a uint64_t.
        char   nibbles[16] {};
        size_t count = 0;
        do {
            nibbles[count++] = kDigits[value & 0xF];
            value >>= 4;
        } while (value != 0 && count < sizeof(nibbles));

        char   buf[2 + sizeof(nibbles)] {'0', 'x'};
        size_t len = 2;
        while (count > 0 && len < sizeof(buf)) {
            buf[len++] = nibbles[--count];
        }
        Append(std::string_view(buf, len));
    }

    [[nodiscard]] auto size() const noexcept -> size_t {
        return _len;
    }

private:
    char*  _buf;
    size_t _capacity;
    size_t _len = 0;
};

} // namespace

void InitializeSymbolResolver() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    // backtrace()/backtrace_symbols() need no setup.
#else
    // DbgHelp is per-process, not per-call. exchange() rather than a plain bool
    // because SetupSignalHandler can be reached from more than one entry point
    // (app/main.cpp, app/UIEditor.cpp) and only the first one should initialise.
    static std::atomic<bool> s_initialized {false};
    if (s_initialized.exchange(true, std::memory_order::acq_rel)) {
        return;
    }

    // UNDNAME turns the decorated names DbgHelp returns into readable C++;
    // DEFERRED_LOADS keeps symbol loading off the initialisation path, which
    // matters because this can be called while the process is already unstable.
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, true);
#endif
}

auto CaptureStackTrace(char* buf, size_t capacity, int maxFrames) noexcept -> size_t {
    if (buf == nullptr || capacity == 0 || maxFrames <= 0) {
        return 0;
    }
    if (maxFrames > kMaxFrames) {
        maxFrames = kMaxFrames;
    }

    BufferAppender out(buf, capacity);

#if defined(__APPLE__) || defined(__linux__)
    void* frames[kMaxFrames] {};
    const int count = backtrace(frames, maxFrames);
    if (count <= 0) {
        return 0;
    }

    // backtrace_symbols allocates one array for the whole trace and the caller
    // frees it. It is the one allocation left on this path: removing it means
    // resolving symbols with dladdr and linking libdl, which is a separate
    // change from splitting this file. Everything after this point is
    // buffer-only.
    char** symbols = backtrace_symbols(frames, count);
    if (symbols == nullptr) {
        return 0;
    }

    for (int i = 0; i < count; ++i) {
        const std::string_view line = (symbols[i] != nullptr) ? std::string_view(symbols[i]) : std::string_view();

        // A GNU symbol line reads "<prefix> _Z<...> + <offset>"; demangle the
        // mangled name in place and reassemble around it.
        const size_t nameStart = line.find("_Z");
        const size_t nameEnd   = (nameStart == std::string_view::npos) ? std::string_view::npos : line.find(" + ", nameStart);

        if (nameStart != std::string_view::npos && nameEnd != std::string_view::npos) {
            const std::string_view mangled = line.substr(nameStart, nameEnd - nameStart);

            // __cxa_demangle reads a C string, and `mangled` is a window into the
            // middle of `line`, so its data() is not NUL-terminated. Copy it out
            // first -- pointing demangle at the slice would let it run past the
            // end of the symbol and into the offset text.
            char   mangledBuf[kMaxMangledLength] {};
            char   demangled[kDemangleCapacity] {};
            size_t demangledLen = sizeof(demangled);
            int    status       = -1;
            char*  result       = nullptr;

            if (mangled.size() < sizeof(mangledBuf)) {
                std::memcpy(mangledBuf, mangled.data(), mangled.size());
                result = abi::__cxa_demangle(mangledBuf, demangled, &demangledLen, &status);
            }

            if (status == 0 && result != nullptr) {
                out.Append(line.substr(0, nameStart));
                out.Append(std::string_view(result));
                out.Append(line.substr(nameEnd));
                out.Append("\n");
                // __cxa_demangle reallocates when the buffer it was handed is
                // too small, and hands back a pointer that is then ours to free.
                if (result != demangled) {
                    std::free(result);
                }
                continue;
            }
            if (result != nullptr && result != demangled) {
                std::free(result);
            }
        }

        out.Append(line);
        out.Append("\n");
    }

    std::free(static_cast<void*>(symbols));
#else
    void*          frames[kMaxFrames] {};
    const HANDLE   process  = GetCurrentProcess();
    const USHORT   captured = CaptureStackBackTrace(0, static_cast<ULONG>(maxFrames), frames, nullptr);

    char         symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] {};
    auto*        symbol               = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);
    symbol->SizeOfStruct              = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen                = MAX_SYM_NAME;

    for (USHORT i = 0; i < captured; ++i) {
        out.AppendUInt(i);
        out.Append(": ");

        const auto address = reinterpret_cast<DWORD64>(frames[i]);

        // SymFromAddr writes into symbol->Name only on success. Checking the
        // return is what stops a failed lookup from printing the previous
        // frame's name, which is indistinguishable from a real symbol in the
        // log and sends whoever reads it to the wrong function.
        if (SymFromAddr(process, address, nullptr, symbol) != FALSE) {
            out.Append(std::string_view(symbol->Name, strnlen(symbol->Name, MAX_SYM_NAME)));
        } else {
            out.Append("<unresolved>");
        }
        out.Append(" - ");
        out.AppendHex(address);
        out.Append("\n");
    }
#endif

    return out.size();
}

} // namespace ZHLN::Diagnostics

namespace ZHLN {

auto GetPoorMansStacktrace() -> std::string {
    // Generous because this is the ordinary panic path with a healthy stack.
    // The crash path calls Diagnostics::CaptureStackTrace directly with a much
    // smaller budget rather than going through here.
    constexpr size_t kCapacity = 16384;

    char   buf[kCapacity] {};
    size_t len = Diagnostics::CaptureStackTrace(buf, sizeof(buf), Diagnostics::kMaxFrames);

    if (len == 0) {
        return "Not implemented";
    }
    return std::string(buf, len);
}

} // namespace ZHLN
