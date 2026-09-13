// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/diagnostics/MemoryInspector.cpp
//
// Looking at memory that may not be there, and formatting what it finds:
// SafeRead, the fault-region window, MemoryDump, and the struct-trace frame.
//
// Everything here can be reached from a signal handler, so nothing here
// allocates. That is a change for MemoryDump, which used to build each row out
// of three std::strings -- hex, ASCII and interpretation -- and therefore grew
// the heap once per row while dumping. It is reachable from the crash path
// because ZHLN::Dump(cam.frustum) runs there, so a corrupt heap turned the dump
// of the frustum into a second crash. The rows are fixed stack buffers now and
// the padding comes from a static string of spaces.
//
// SafeRead is the interesting one. It existed because a crash handler is handed
// a faulting address and has to inspect the surrounding bytes without taking a
// second fault, and on POSIX the way it did that was pipe(), write() the probe
// region in, read() it back out, close() both ends -- two file descriptors per
// probe. A fault-region dump probes eight lines, and a dump that walks a
// structure probes hundreds, so a process already near its descriptor limit
// started failing probes that would have succeeded, and the report filled with
// "unreadable" rows for memory that was perfectly readable. On Linux this is
// process_vm_readv, which asks the kernel to copy from the process to itself
// and allocates no descriptor at all.

#include "diagnostics/DiagnosticsInternal.hpp"
#include <Zahlen/Core/Platform.hpp> // windows.h on Windows, unistd.h on Unix
#include <Zahlen/Core/Print.hpp>    // BufferPrint, Format
#include <Zahlen/Log.hpp>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(__linux__)
#include <sys/uio.h> // process_vm_readv
#elif defined(__APPLE__)
#include <fcntl.h>
#endif

namespace ZHLN::Diagnostics {

// Enough spaces to pad any column without formatting a run of them on the fly.
constexpr std::string_view kSpaces = "                                                                ";

// Counts characters a terminal will actually advance for, skipping ANSI colour
// sequences. Padding to a byte length instead of a visible length is what makes
// the interpretation column wobble whenever a value is coloured.
static auto CountVisibleChars(std::string_view str) noexcept -> size_t {
    size_t count = 0;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\x1b') { // Start of ANSI escape
            while (i < str.size() && str[i] != 'm') {
                ++i;
            }
        } else {
            count++;
        }
    }
    return count;
}

// Writes exactly `width` spaces.
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

auto SafeRead(const void* src, void* dest, size_t size) noexcept -> bool {
    if ((src == nullptr) || (dest == nullptr) || size == 0) {
        return false;
    }
#if defined(_WIN32)
    SIZE_T bytesRead = 0;
    BOOL   ok        = ReadProcessMemory(GetCurrentProcess(), src, dest, (SIZE_T) size, &bytesRead);
    return ok && (bytesRead == size);
#elif defined(__linux__)
    // One syscall, no descriptor, and the kernel reports a short copy when the
    // range runs off the end of a mapping -- which is exactly the "is this
    // readable" answer the caller wants. Reading the process's own memory needs
    // no ptrace privilege.
    struct iovec local {
        .iov_base = dest, .iov_len = size
    };
    struct iovec remote {
        .iov_base = const_cast<void*>(src), .iov_len = size
    };

    const ssize_t copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return copied == static_cast<ssize_t>(size);
#else
    // macOS has no process_vm_readv. This is the descriptor-per-probe path the
    // Linux branch replaces, kept because it is the only portable fallback; it
    // is the reason a deep dump on Apple should stay shallow.
    int fd[2];
    if (pipe(fd) < 0) {
        return false;
    }
    // Make the write non-blocking so we never hang if the kernel buffer gets full
    fcntl(fd[1], F_SETFL, O_NONBLOCK);
    ssize_t written = write(fd[1], src, size);
    if (written > 0) {
        ssize_t read_bytes = read(fd[0], dest, written);
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
    constexpr size_t totalLines   = 8; // Dumps 128 bytes total
    constexpr size_t totalSize    = bytesPerLine * totalLines;

    auto dump_hdr = ZHLN::Format("\n{}--- FAULTING MEMORY SURROUNDING REGION ---{}\n", Color::Cyan, Color::Reset);
    WriteErr(dump_hdr.string_view());

    const char* byte_ptr = static_cast<const char*>(faultAddress);

    // Offset starting point back by half our dump window so the faulting address is centered
    const char* start_ptr = byte_ptr - (bytesPerLine * (totalLines / 2));

    // Align startAddr to 16-byte boundary for clean formatting
    uintptr_t alignedStart = std::bit_cast<uintptr_t>(start_ptr) & ~15ULL;
    start_ptr              = std::bit_cast<const char*>(alignedStart);

    for (size_t i = 0; i < totalSize; i += bytesPerLine) {
        const char* current_ptr = start_ptr + i;

        // Safely probe if the current line's memory is readable
        char raw_bytes[bytesPerLine] {};
        bool readable = SafeRead(current_ptr, raw_bytes, bytesPerLine);

        // Address
        auto addr_str = ZHLN::Format("  {}{:016X}{} | ", Color::Cyan, std::bit_cast<uintptr_t>(current_ptr), Color::Reset);
        WriteErr(addr_str.string_view());

        if (!readable) {
            const auto* line = "?? ?? ?? ?? ?? ?? ?? ??  ?? ?? ?? ?? ?? ?? ?? ?? | ????????????????\n";
            WriteErr(line);
            continue;
        }

        // Format Hex bytes
        char lineBuf[512] {};
        int  offset = 0;
        for (size_t j = 0; j < bytesPerLine; ++j) {
            const char* cur      = current_ptr + j;
            bool        isTarget = (cur == static_cast<const char*>(faultAddress));

            if (isTarget) {
                // Highlight the exact faulting byte/address in Red
                offset += ZHLN::BufferPrint(
                    lineBuf + offset, sizeof(lineBuf) - offset, "%s%02X%s ", Color::Red, static_cast<uint8_t>(raw_bytes[j]), Color::Reset
                );
            } else {
                offset += ZHLN::BufferPrint(lineBuf + offset, sizeof(lineBuf) - offset, "%02X ", static_cast<uint8_t>(raw_bytes[j]));
            }

            if ((j + 1) % 4 == 0 && j + 1 < bytesPerLine) {
                offset += ZHLN::BufferPrint(lineBuf + offset, sizeof(lineBuf) - offset, " ");
            }
        }
        // Pad the hex column to maintain alignment
        while (offset < 54) {
            lineBuf[offset++] = ' ';
        }
        lineBuf[offset] = '\0';
        WriteErr(std::string_view(lineBuf, offset));
        WriteErr(" | ");

        // Format ASCII
        char ascii_buf[bytesPerLine + 1] {};
        for (size_t j = 0; j < bytesPerLine; ++j) {
            auto c       = static_cast<uint8_t>(raw_bytes[j]);
            ascii_buf[j] = std::isprint(c) ? static_cast<char>(c) : '.';
        }
        WriteErr(std::string_view(ascii_buf, bytesPerLine));
        WriteErr(" |\n");
    }
    WriteErr("\n");
}

} // namespace ZHLN::Diagnostics

namespace ZHLN {

using Diagnostics::CountVisibleChars;
using Diagnostics::WriteErr;
using Diagnostics::WritePadding;

auto TraceStructCallback(const char* fmt, ...) -> int {
    va_list args;
    va_start(args, fmt);

    // Keep trace rendering signal-safe via BufferPrint [1]
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
        // Address column: 16 hex digits (64-bit), uppercase, consistent width
        auto addr_str = ZHLN::Format("│ {}{:016X}{} │ ", Color::Cyan, std::bit_cast<uintptr_t>(byte_ptr + i), Color::Reset);
        WriteErr(addr_str.string_view());

        // Hex data column: a fixed buffer, written into directly. The row this
        // replaces was a std::string that grew one Format() at a time.
        // 4 characters per byte ("XX ") plus a separator every four bytes, with
        // room for a wider bytes_per_line than the default 16.
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
                hex_row[hex_len++] = ' '; // Pad empty slots (3 chars per byte)
            }
            // Add extra space after every 4 bytes for readability
            if ((j + 1) % 4 == 0 && j + 1 < opts.bytes_per_line) {
                hex_row[hex_len++] = ' ';
            }
        }
        // Ensure the hex column is exactly 54 chars. Both directions: the row
        // this replaces ended in resize(54, ' '), which truncates a wider
        // bytes_per_line as readily as it pads the default one.
        constexpr size_t kHexWidth = 54;
        if (hex_len < kHexWidth) {
            std::memset(hex_row + hex_len, ' ', kHexWidth - hex_len);
        }
        hex_len = kHexWidth;
        WriteErr(std::string_view(hex_row, hex_len));
        WriteErr("│ ");

        // ASCII column: Exactly 16 chars, use '.' instead of '·' to avoid UTF-8 multibyte issues
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

        // Interpretation column. The FormatResult has to outlive the write --
        // it owns the pool slot its string_view points into, so binding it to a
        // name before use is what keeps this from reading released memory.
        constexpr size_t kInterpretWidth = 23;
        size_t           visible         = 0;

        if (i + 8 <= size) {
            uint64_t val64 = 0;
            std::memcpy(&val64, byte_ptr + i, 8);

            if (val64 != 0) {
                if (val64 > 0x100000000 && val64 < 0x00007FFFFFFFFFFF) {
                    // Looks like a pointer
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

        // Pad to 23 visible chars (excluding ANSI escape sequences)
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

} // namespace ZHLN
