// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "diagnostics/DiagnosticsInternal.hpp"
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Log.hpp>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#if defined(__linux__)
#include <sys/uio.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#endif

namespace ZHLN::Diagnostics {

constexpr std::string_view kSpaces = "                                                                ";

static auto CountVisibleChars(std::string_view str) noexcept -> size_t {
    size_t count = 0;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\x1b') {
            while (i < str.size() && str[i] != 'm') {
                ++i;
            }
        } else {
            count++;
        }
    }
    return count;
}

static void WritePadding(size_t width) noexcept {
    while (width > 0) {
        const size_t take = (width < kSpaces.size()) ? width : kSpaces.size();
        WriteErr(kSpaces.substr(0, take));
        width -= take;
    }
}

static auto HexDigit(uint8_t nibble) noexcept -> char {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    return kDigits[nibble & 0x0F];
}

auto SafeRead(std::span<const std::byte> src, std::span<std::byte> dest) noexcept -> bool {
    if ((src.data() == nullptr) || (dest.data() == nullptr) || dest.empty()) {
        return false;
    }
    if (src.size() < dest.size()) {
        return false;
    }

    const size_t size = dest.size();
#if defined(_WIN32)
    SIZE_T bytesRead = 0;
    BOOL   ok        = ReadProcessMemory(GetCurrentProcess(), src.data(), dest.data(), static_cast<SIZE_T>(size), &bytesRead);
    return ok && (bytesRead == size);
#elif defined(__linux__)
    struct iovec local {
        .iov_base = dest.data(), .iov_len = size
    };
    struct iovec remote {
        .iov_base = const_cast<void*>(static_cast<const void*>(src.data())), .iov_len = size
    };

    const ssize_t copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return copied == static_cast<ssize_t>(size);
#else
    int fd[2];
    if (pipe(fd) < 0) {
        return false;
    }
    fcntl(fd[1], F_SETFL, O_NONBLOCK);
    ssize_t written = write(fd[1], src.data(), size);
    if (written > 0) {
        ssize_t read_bytes = read(fd[0], dest.data(), written);
        close(fd[0]);
        close(fd[1]);
        return read_bytes == static_cast<ssize_t>(size);
    }
    close(fd[0]);
    close(fd[1]);
    return false;
#endif
}

void DumpFaultRegion(const void* faultAddress) noexcept {
    if (faultAddress == nullptr) {
        return;
    }

    constexpr size_t bytesPerLine = 16;
    constexpr size_t totalLines   = 8;
    constexpr size_t totalSize    = bytesPerLine * totalLines;

    auto dump_hdr = ZHLN::Format("\n{}--- FAULTING MEMORY SURROUNDING REGION ---{}\n", Color::Cyan, Color::Reset);
    WriteErr(dump_hdr.string_view());

    const char* byte_ptr = static_cast<const char*>(faultAddress);

    const char* start_ptr = byte_ptr - (bytesPerLine * (totalLines / 2));

    uintptr_t alignedStart = std::bit_cast<uintptr_t>(start_ptr) & ~15ULL;
    start_ptr              = std::bit_cast<const char*>(alignedStart);

    for (size_t i = 0; i < totalSize; i += bytesPerLine) {
        const char* current_ptr = start_ptr + i;

        std::byte raw_bytes[bytesPerLine] {};
        const auto probe = std::span<const std::byte>(reinterpret_cast<const std::byte*>(current_ptr), bytesPerLine);
        const bool readable = SafeRead(probe, raw_bytes);

        auto addr_str = ZHLN::Format("  {}{:016X}{} | ", Color::Cyan, std::bit_cast<uintptr_t>(current_ptr), Color::Reset);
        WriteErr(addr_str.string_view());

        if (!readable) {
            const auto* line = "?? ?? ?? ?? ?? ?? ?? ??  ?? ?? ?? ?? ?? ?? ?? ?? | ????????????????\n";
            WriteErr(line);
            continue;
        }

        char lineBuf[512] {};
        int  offset = 0;
        for (size_t j = 0; j < bytesPerLine; ++j) {
            const char* cur      = current_ptr + j;
            bool        isTarget = (cur == static_cast<const char*>(faultAddress));

            if (isTarget) {
                offset += ZHLN::BufferPrint(
                    lineBuf + offset, sizeof(lineBuf) - offset, "%s%02X%s ", Color::Red, std::to_integer<uint8_t>(raw_bytes[j]), Color::Reset
                );
            } else {
                offset += ZHLN::BufferPrint(lineBuf + offset, sizeof(lineBuf) - offset, "%02X ", std::to_integer<uint8_t>(raw_bytes[j]));
            }

            if ((j + 1) % 4 == 0 && j + 1 < bytesPerLine) {
                offset += ZHLN::BufferPrint(lineBuf + offset, sizeof(lineBuf) - offset, " ");
            }
        }
        while (offset < 54) {
            lineBuf[offset++] = ' ';
        }
        lineBuf[offset] = '\0';
        WriteErr(std::string_view(lineBuf, offset));
        WriteErr(" | ");

        char ascii_buf[bytesPerLine + 1] {};
        for (size_t j = 0; j < bytesPerLine; ++j) {
            auto c       = std::to_integer<uint8_t>(raw_bytes[j]);
            ascii_buf[j] = std::isprint(c) ? static_cast<char>(c) : '.';
        }
        WriteErr(std::string_view(ascii_buf, bytesPerLine));
        WriteErr(" |\n");
    }
    WriteErr("\n");
}

}

namespace ZHLN {

using Diagnostics::CountVisibleChars;
using Diagnostics::WriteErr;
using Diagnostics::WritePadding;

auto TraceStructCallback(const char* fmt, ...) -> int {
    va_list args;
    va_start(args, fmt);

    char buf[1024];
    int  ret = ZHLN::BufferPrint(buf, sizeof(buf), fmt, args);
    if (ret > 0) {
        WriteErr(std::string_view(buf, ret));
    }

    va_end(args);
    return ret;
}

void TraceStructHeader(std::string_view name, std::string_view label, const char* file, uint32_t line) {
    std::string_view file_name = file;
    if (auto pos = file_name.find_last_of("/\\"); pos != std::string_view::npos) {
        file_name.remove_prefix(pos + 1);
    }
    auto        line1 = ZHLN::Format("{}┌─── STRUCT TRACE: {} ({}) ───{}\n", Color::Cyan, name, label, Color::Reset);
    auto        line2 = ZHLN::Format("│ Source:  {}:{}\n", file_name, line);
    const auto* line3 = "├──────────────────────────────────────────────────────────────────────────────\n";

    WriteErr(line1.string_view());
    WriteErr(line2.string_view());
    WriteErr(line3);
}

void TraceStructFooter() {
    auto line = ZHLN::Format("{}└──────────────────────────────────────────────────────────────────────────────{}\n", Color::Cyan, Color::Reset);
    WriteErr(line.string_view());
}

void MemoryDump(const void* ptr, size_t size, std::string_view label, LogContext ctx, DumpOptions opts) {
    const auto*      byte_ptr  = static_cast<const uint8_t*>(ptr);
    std::string_view file_name = ctx.loc.file_name();
    if (auto pos = file_name.find_last_of("/\\"); pos != std::string_view::npos) {
        file_name.remove_prefix(pos + 1);
    }

    if (opts.bytes_per_line == 0) {
        return;
    }

    auto        header1 = ZHLN::Format("{}┌─── DUMP: {} ({}) ───{}\n", Color::Cyan, label, ctx.fmt, Color::Reset);
    auto        header2 = ZHLN::Format("│ Source:  {}:{}\n", file_name, ctx.loc.line());
    auto        header3 = ZHLN::Format("│ Address: {}{}{} ({} bytes)\n", Color::Yellow, ptr, Color::Reset, size);
    const auto* header4 = "├──────────────────┬───────────────────────────────────────────────────────┬───"
                          "───────────────┬─────────────────────────┤\n";
    const auto* header5 = "│     Address      │ Hex Data                                              │ "
                          "ASCII            │ Interpretation          │\n";
    const auto* header6 = "├──────────────────┼───────────────────────────────────────────────────────┼───"
                          "───────────────┼─────────────────────────┤\n";

    WriteErr(header1.string_view());
    WriteErr(header2.string_view());
    WriteErr(header3.string_view());
    WriteErr(header4);
    WriteErr(header5);
    WriteErr(header6);

    for (size_t i = 0; i < size; i += opts.bytes_per_line) {
        auto addr_str = ZHLN::Format("│ {}{:016X}{} │ ", Color::Cyan, std::bit_cast<uintptr_t>(byte_ptr + i), Color::Reset);
        WriteErr(addr_str.string_view());

        char   hex_row[512] {};
        size_t hex_len = 0;
        for (size_t j = 0; j < opts.bytes_per_line && hex_len + 4 < sizeof(hex_row); ++j) {
            if (i + j < size) {
                const uint8_t b = byte_ptr[i + j];
                hex_row[hex_len++] = Diagnostics::HexDigit(static_cast<uint8_t>(b >> 4));
                hex_row[hex_len++] = Diagnostics::HexDigit(b);
                hex_row[hex_len++] = ' ';
            } else {
                hex_row[hex_len++] = ' ';
                hex_row[hex_len++] = ' ';
                hex_row[hex_len++] = ' ';
            }
            if ((j + 1) % 4 == 0 && j + 1 < opts.bytes_per_line) {
                hex_row[hex_len++] = ' ';
            }
        }
        constexpr size_t kHexWidth = 54;
        if (hex_len < kHexWidth) {
            std::memset(hex_row + hex_len, ' ', kHexWidth - hex_len);
        }
        hex_len = kHexWidth;
        WriteErr(std::string_view(hex_row, hex_len));
        WriteErr("│ ");

        constexpr size_t kAsciiWidth = 16;
        char             ascii_row[kAsciiWidth] {};
        for (size_t j = 0; j < opts.bytes_per_line && j < kAsciiWidth; ++j) {
            if (i + j < size) {
                const uint8_t c = byte_ptr[i + j];
                ascii_row[j]    = std::isprint(c) ? static_cast<char>(c) : '.';
            } else {
                ascii_row[j] = ' ';
            }
        }
        WriteErr(std::string_view(ascii_row, kAsciiWidth));
        WriteErr(" │ ");

        constexpr size_t kInterpretWidth = 23;
        size_t           visible         = 0;

        if (i + 8 <= size) {
            uint64_t val64 = 0;
            std::memcpy(&val64, byte_ptr + i, 8);

            if (val64 != 0) {
                if (val64 > 0x100000000 && val64 < 0x00007FFFFFFFFFFF) {
                    auto info = ZHLN::Format("{}ptr: {:#014X}{}", Color::Green, val64, Color::Reset);
                    std::string_view text = info.string_view();
                    WriteErr(text);
                    visible = CountVisibleChars(text);
                } else {
                    float   f32 = NAN;
                    int32_t i32 = 0;
                    std::memcpy(&f32, byte_ptr + i, 4);
                    std::memcpy(&i32, byte_ptr + i, 4);

                    if (!std::isnan(f32) && std::abs(f32) > 0.0001f && std::abs(f32) < 1000000.0f) {
                        auto               info = ZHLN::Format("flt: {}", f32);
                        std::string_view   text = info.string_view();
                        WriteErr(text);
                        visible = CountVisibleChars(text);
                    } else {
                        auto             info = ZHLN::Format("int: {}", i32);
                        std::string_view text = info.string_view();
                        WriteErr(text);
                        visible = CountVisibleChars(text);
                    }
                }
            } else {
                auto             info = ZHLN::Format("{}---{}", Color::Gray, Color::Reset);
                std::string_view text = info.string_view();
                WriteErr(text);
                visible = CountVisibleChars(text);
            }
        } else {
            auto             info = ZHLN::Format("{}N/A{}", Color::Gray, Color::Reset);
            std::string_view text = info.string_view();
            WriteErr(text);
            visible = CountVisibleChars(text);
        }

        if (visible < kInterpretWidth) {
            WritePadding(kInterpretWidth - visible);
        }

        WriteErr(" │\n");
    }

    auto footer = ZHLN::Format(
        "{}"
        "└──────────────────┴───────────────────────────────────────────────"
        "────────┴──────────────────┴─────────────────────────┘{}\n",
        Color::Cyan, Color::Reset
    );
    WriteErr(footer.string_view());
}

}
