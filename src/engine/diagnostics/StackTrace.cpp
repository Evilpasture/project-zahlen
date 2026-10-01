// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "diagnostics/DiagnosticsInternal.hpp"
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Log.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <execinfo.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // i'm tired of missing macros
#include <dbghelp.h>
#if defined(_MSC_VER) // MSVC autolink; MinGW links dbghelp from CMakeLists.txt
#pragma comment(lib, "dbghelp.lib")
#endif
#endif

namespace ZHLN::Diagnostics {

constexpr int kMaxFrames = 128;

namespace {

constexpr size_t kDemangleCapacity = 1024;

constexpr size_t kMaxMangledLength = 512;

class BufferAppender {
  public:
    explicit BufferAppender(std::span<char> out) noexcept: _out(out) {
    }

    void Append(std::string_view text) noexcept {
        const size_t room = (_len < _out.size()) ? (_out.size() - _len) : 0;
        const size_t take = (text.size() < room) ? text.size() : room;
        if (take > 0) {
            std::memcpy(_out.data() + _len, text.data(), take);
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
    std::span<char> _out;
    size_t          _len = 0;
};

} // namespace

void InitializeSymbolResolver() noexcept {
#if defined(__APPLE__) || defined(__linux__)
#else
    static std::atomic<bool> s_initialized {false};
    if (s_initialized.exchange(true, std::memory_order::acq_rel)) {
        return;
    }

    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, true);
#endif
}

auto CaptureStackTrace(std::span<char> out, int maxFrames) noexcept -> size_t {
    if (out.empty() || maxFrames <= 0) {
        return 0;
    }
    if (maxFrames > kMaxFrames) {
        maxFrames = kMaxFrames;
    }

    BufferAppender sink(out);

#if defined(__APPLE__) || defined(__linux__)
    void*     frames[kMaxFrames] {};
    const int count = backtrace(frames, maxFrames);
    if (count <= 0) {
        return 0;
    }

    char** symbols = backtrace_symbols(frames, count);
    if (symbols == nullptr) {
        return 0;
    }

    for (int i = 0; i < count; ++i) {
        const std::string_view line = (symbols[i] != nullptr) ? std::string_view(symbols[i]) : std::string_view();

        const size_t nameStart = line.find("_Z");
        const size_t nameEnd   = (nameStart == std::string_view::npos) ? std::string_view::npos : line.find(" + ", nameStart);

        if (nameStart != std::string_view::npos && nameEnd != std::string_view::npos) {
            const std::string_view mangled = line.substr(nameStart, nameEnd - nameStart);

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
                sink.Append(line.substr(0, nameStart));
                sink.Append(std::string_view(result));
                sink.Append(line.substr(nameEnd));
                sink.Append("\n");
                if (result != demangled) {
                    std::free(result);
                }
                continue;
            }
            if (result != nullptr && result != demangled) {
                std::free(result);
            }
        }

        sink.Append(line);
        sink.Append("\n");
    }

    std::free(static_cast<void*>(symbols));
#else
    void*        frames[kMaxFrames] {};
    const HANDLE process  = GetCurrentProcess();
    const USHORT captured = CaptureStackBackTrace(0, static_cast<ULONG>(maxFrames), frames, nullptr);

    char  symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] {};
    auto* symbol         = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen   = MAX_SYM_NAME;

    for (USHORT i = 0; i < captured; ++i) {
        sink.AppendUInt(i);
        sink.Append(": ");

        const auto address = reinterpret_cast<DWORD64>(frames[i]);

        if (SymFromAddr(process, address, nullptr, symbol) != FALSE) {
            sink.Append(std::string_view(symbol->Name, strnlen(symbol->Name, MAX_SYM_NAME)));
        } else {
            sink.Append("<unresolved>");
        }
        sink.Append(" - ");
        sink.AppendHex(address);
        sink.Append("\n");
    }
#endif

    return sink.size();
}

} // namespace ZHLN::Diagnostics

namespace ZHLN {

auto GetPoorMansStacktrace() -> std::string {
    constexpr size_t kCapacity = 16384;

    char   buf[kCapacity] {};
    size_t len = Diagnostics::CaptureStackTrace(buf, Diagnostics::kMaxFrames);

    if (len == 0) {
        return "Not implemented";
    }
    return std::string(buf, len);
}

} // namespace ZHLN
