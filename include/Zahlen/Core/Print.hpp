// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Print.hpp
//
// Console/file-descriptor output built on top of the formatting engine in
// Zahlen/Core/Format.hpp: Detail::RawWrite plus the Print/Println overloads.

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

// ============================================================================
// Signal-Safe Raw File-Descriptor Write
// ============================================================================

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

} // namespace Detail

// ============================================================================
// ZHLN::Print Overloads (Accepts FILE*, raw fd, or defaults to stdout)
// ============================================================================

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

// ============================================================================
// ZHLN::Println Overloads (Accepts FILE*, raw fd, or defaults to stdout)
// ============================================================================

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

} // namespace ZHLN
