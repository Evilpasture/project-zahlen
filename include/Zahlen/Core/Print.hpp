// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include "Zahlen/Core/Format.hpp"
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ZHLN {


namespace Detail {

inline void RawWrite(int fd, const char* buf, size_t len) noexcept {
    if (len == 0) {
        return;
    }
#if defined(_WIN32)
    ::_write(fd, buf, static_cast<unsigned int>(len));
#else
    ::write(fd, buf, len);
#endif
}

}


template <typename... Args>
inline void Print(int fd, std::string_view fmt, Args&&... args) noexcept {
    if constexpr (sizeof...(Args) == 0) {
        Detail::RawWrite(fd, fmt.data(), fmt.size());
    } else {
        auto result = Format(fmt, std::forward<Args>(args)...);
        if (!result.empty()) {
            Detail::RawWrite(fd, result.string_view().data(), result.string_view().size());
        } else {
            Detail::RawWrite(fd, fmt.data(), fmt.size());
        }
    }
}

template <typename... Args>
inline void Print(FILE* stream, std::string_view fmt, Args&&... args) noexcept {
    int fd = (stream == stderr) ? 2 : 1;
    Print(fd, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Print(std::string_view fmt, Args&&... args) noexcept {
    Print(1, fmt, std::forward<Args>(args)...);
}


template <typename... Args>
inline void Println(int fd, std::string_view fmt, Args&&... args) noexcept {
    Print(fd, fmt, std::forward<Args>(args)...);
    Detail::RawWrite(fd, "\n", 1);
}

template <typename... Args>
inline void Println(FILE* stream, std::string_view fmt, Args&&... args) noexcept {
    int fd = (stream == stderr) ? 2 : 1;
    Println(fd, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Println(std::string_view fmt, Args&&... args) noexcept {
    Println(1, fmt, std::forward<Args>(args)...);
}

inline void Println() noexcept {
    Detail::RawWrite(1, "\n", 1);
}

inline void Println(int fd) noexcept {
    Detail::RawWrite(fd, "\n", 1);
}

inline void Println(FILE* stream) noexcept {
    int fd = (stream == stderr) ? 2 : 1;
    Detail::RawWrite(fd, "\n", 1);
}

namespace Color {
// These were `char[N]` arrays, and that is what GCC 16.2 miscompiles: passing several
// of them to one std::println/std::print next to string_view arguments, in a PIC
// translation unit at -O1 or above, makes the call land on the array's address in
// .rodata -- the process dies with SIGSEGV before a character is printed. As
// `std::string_view` the same call sites format identically and the miscompile is out
// of reach. Everything that needs a `const char*` now says `.data()`: the
// array-to-pointer decay is gone from the places that were getting it silently.
inline constexpr std::string_view Reset   = "\033[0m";
inline constexpr std::string_view Bold    = "\033[1m";
inline constexpr std::string_view Dim     = "\033[2m";
inline constexpr std::string_view Gray    = "\033[90m";
inline constexpr std::string_view Cyan    = "\033[36m";
inline constexpr std::string_view Yellow  = "\033[33m";
inline constexpr std::string_view Green   = "\033[32m";
inline constexpr std::string_view Red     = "\033[31m";
inline constexpr std::string_view BCyan   = "\033[1;36m";
inline constexpr std::string_view BGreen  = "\033[1;32m";
inline constexpr std::string_view BYellow = "\033[1;33m";
inline constexpr std::string_view BRed    = "\033[1;31m";
} // namespace Color

}
