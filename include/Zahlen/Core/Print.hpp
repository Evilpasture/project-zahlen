// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <type_traits>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ZHLN {

// ============================================================================
// Lock-Free Statically Allocated Signal-Safe Memory Pool
// ============================================================================

class SignalSafePool {
  public:
    static constexpr size_t BufferSize  = 2048;
    static constexpr size_t BufferCount = 16;

    static char* Acquire(size_t& out_idx) noexcept {
        uint32_t mask = s_allocatedMask.load(std::memory_order::relaxed);
        while (true) {
            uint32_t free_bit         = ~mask;
            uint32_t active_free_bits = free_bit & ((1u << BufferCount) - 1);
            if (active_free_bits == 0) {
                return nullptr;
            }
            uint32_t idx      = std::countr_zero(active_free_bits);
            uint32_t new_mask = mask | (1u << idx);
            if (s_allocatedMask.compare_exchange_weak(mask, new_mask, std::memory_order::acquire, std::memory_order::relaxed)) {
                out_idx = idx;
                return s_pool[idx];
            }
        }
    }

    static void Release(size_t idx) noexcept {
        s_allocatedMask.fetch_and(~(1u << idx), std::memory_order::release);
    }

    static const char* GetBuffer(size_t idx) noexcept {
        return s_pool[idx];
    }

    static char* GetBufferMutable(size_t idx) noexcept {
        return s_pool[idx];
    }

  private:
    inline static char                  s_pool[BufferCount][BufferSize];
    inline static std::atomic<uint32_t> s_allocatedMask {0};
};

// ============================================================================
// Async-Signal Safe RAII Resource Holder
// ============================================================================

class FormatResult {
  public:
    constexpr FormatResult() noexcept: _poolIdx(0), _len(0), _valid(false) {
    }
    constexpr FormatResult(size_t poolIdx, size_t len) noexcept: _poolIdx(poolIdx), _len(len), _valid(true) {
    }

    ~FormatResult() noexcept {
        if (_valid) {
            SignalSafePool::Release(_poolIdx);
        }
    }

    FormatResult(const FormatResult&)            = delete;
    FormatResult& operator=(const FormatResult&) = delete;

    FormatResult(FormatResult&& other) noexcept: _poolIdx(other._poolIdx), _len(other._len), _valid(other._valid) {
        other._valid = false;
    }

    FormatResult& operator=(FormatResult&& other) noexcept {
        if (this != &other) {
            if (_valid) {
                SignalSafePool::Release(_poolIdx);
            }
            _poolIdx     = other._poolIdx;
            _len         = other._len;
            _valid       = other._valid;
            other._valid = false;
        }
        return *this;
    }

    [[nodiscard]] const char* c_str() const noexcept {
        return _valid ? SignalSafePool::GetBuffer(_poolIdx) : "";
    }

    [[nodiscard]] std::string_view string_view() const noexcept {
        return _valid ? std::string_view(SignalSafePool::GetBuffer(_poolIdx), _len) : std::string_view();
    }

    operator std::string_view() const noexcept {
        return string_view();
    }

    [[nodiscard]] size_t size() const noexcept {
        return _len;
    }
    [[nodiscard]] bool empty() const noexcept {
        return _len == 0;
    }

  private:
    size_t _poolIdx;
    size_t _len;
    bool   _valid;
};

// ============================================================================
// Signal-Safe Low-Level Conversion Utilities
// ============================================================================

namespace Detail {

/**
 * @brief Performs a direct, unbuffered, lock-free system write to standard output.
 * This operation is async-signal safe on POSIX and Windows.
 */
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

inline size_t SafeStrLen(const char* s) noexcept {
    if (s == nullptr) {
        return 0;
    }
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/**
 * @brief Index-based string reversal.
 * Removes pointer subtraction to prevent out-of-bound underflow diagnostics.
 */
inline void ReverseStr(char* buf, size_t len) noexcept {
    if (len <= 1) {
        return;
    }
    size_t i = 0;
    size_t j = len - 1;
    while (i < j) {
        char t = buf[i];
        buf[i] = buf[j];
        buf[j] = t;
        i++;
        j--;
    }
}

/**
 * @brief Async-signal safe signed integer formatting.
 * Safely handles LLONG_MIN negation via unsigned conversion before arithmetic.
 */
inline size_t FormatInt(char* buf, size_t max_len, int64_t val) noexcept {
    if (max_len == 0) {
        return 0;
    }
    size_t   len  = 0;
    bool     neg  = false;
    uint64_t uval = 0;
    if (val < 0) {
        neg = true;
        // Cast before negating to prevent signed integer overflow UB on LLONG_MIN
        uval = -static_cast<uint64_t>(val);
    } else {
        uval = static_cast<uint64_t>(val);
    }

    if (uval == 0) {
        if (len < max_len - 1) {
            buf[len++] = '0';
        }
    } else {
        while (uval > 0 && len < max_len - 1) {
            buf[len++] = '0' + (uval % 10);
            uval /= 10;
        }
    }
    if (neg && len < max_len - 1) {
        buf[len++] = '-';
    }

    ReverseStr(buf, len);
    buf[len] = '\0';
    return len;
}

/**
 * @brief Async-signal safe unsigned integer formatting.
 */
inline size_t FormatUInt(char* buf, size_t max_len, uint64_t val, int base = 10, bool uppercase = false) noexcept {
    if (max_len == 0) {
        return 0;
    }
    size_t len = 0;
    if (val == 0) {
        if (len < max_len - 1) {
            buf[len++] = '0';
        }
    } else {
        const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
        while (val > 0 && len < max_len - 1) {
            buf[len++] = digits[val % base];
            val /= base;
        }
    }

    ReverseStr(buf, len);
    buf[len] = '\0';
    return len;
}

// Bit-casting for IEEE-754 properties without importing cmath
inline uint64_t DoubleToRaw(double d) noexcept {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

inline bool IsNaN(double d) noexcept {
    uint64_t bits = DoubleToRaw(d);
    return ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) && ((bits & 0x000FFFFFFFFFFFFFULL) != 0);
}

inline bool IsInf(double d) noexcept {
    uint64_t bits = DoubleToRaw(d);
    return ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) && ((bits & 0x000FFFFFFFFFFFFFULL) == 0);
}

inline double SafeModfPositive(double val, double* ipart) noexcept {
    if (val >= 9223372036854775807.0) {
        *ipart = val;
        return 0.0;
    }
    auto int_val = static_cast<uint64_t>(val);
    *ipart       = static_cast<double>(int_val);
    return val - *ipart;
}

inline size_t FormatDouble(char* buf, size_t max_len, double val, int precision = 6) noexcept {
    if (max_len == 0) {
        return 0;
    }
    size_t len = 0;
    if (IsNaN(val)) {
        const char* s    = "nan";
        size_t      slen = SafeStrLen(s);
        size_t      copy = slen < max_len - 1 ? slen : max_len - 1;
        std::memcpy(buf, s, copy);
        buf[copy] = '\0';
        return copy;
    }
    if (IsInf(val)) {
        const char* s    = val < 0 ? "-inf" : "inf";
        size_t      slen = SafeStrLen(s);
        size_t      copy = slen < max_len - 1 ? slen : max_len - 1;
        std::memcpy(buf, s, copy);
        buf[copy] = '\0';
        return copy;
    }

    bool neg = false;
    if (val < 0) {
        neg = true;
        val = -val;
    }

    double ipart = NAN;
    double fpart = SafeModfPositive(val, &ipart);

    auto   i_val = static_cast<uint64_t>(ipart);
    char   int_buf[32];
    size_t int_len = FormatUInt(int_buf, sizeof(int_buf), i_val);

    if (neg && len < max_len - 1) {
        buf[len++] = '-';
    }

    for (size_t i = 0; i < int_len && len < max_len - 1; ++i) {
        buf[len++] = int_buf[i];
    }

    if (precision > 0 && len < max_len - 1) {
        buf[len++] = '.';
        for (int p = 0; p < precision && len < max_len - 1; ++p) {
            fpart *= 10.0;
            int digit  = static_cast<int>(fpart) % 10;
            buf[len++] = '0' + digit;
            fpart -= digit; // Direct subtraction avoids floor loops
        }
    }
    buf[len] = '\0';
    return len;
}

template <typename T>
inline size_t AppendValue(char* buf, size_t max_len, const T& val) noexcept {
    if constexpr (
        std::is_same_v<std::decay_t<T>, int> || std::is_same_v<std::decay_t<T>, long> || std::is_same_v<std::decay_t<T>, long long> ||
        std::is_same_v<std::decay_t<T>, short> || std::is_same_v<std::decay_t<T>, signed char>
    ) {
        return FormatInt(buf, max_len, static_cast<int64_t>(val));
    } else if constexpr (
        std::is_same_v<std::decay_t<T>, unsigned int> || std::is_same_v<std::decay_t<T>, unsigned long> ||
        std::is_same_v<std::decay_t<T>, unsigned long long> || std::is_same_v<std::decay_t<T>, unsigned short> || std::is_same_v<std::decay_t<T>, unsigned char>
    ) {
        return FormatUInt(buf, max_len, static_cast<uint64_t>(val));
    } else if constexpr (std::is_same_v<std::decay_t<T>, double> || std::is_same_v<std::decay_t<T>, float>) {
        return FormatDouble(buf, max_len, static_cast<double>(val));
    } else if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        const char* s    = val ? "true" : "false";
        size_t      slen = SafeStrLen(s);
        size_t      copy = slen < max_len - 1 ? slen : max_len - 1;
        std::memcpy(buf, s, copy);
        buf[copy] = '\0';
        return copy;
    } else if constexpr (std::is_same_v<std::decay_t<T>, char>) {
        if (max_len > 1) {
            buf[0] = val;
            buf[1] = '\0';
            return 1;
        }
        return 0;
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        auto   sv   = static_cast<std::string_view>(val);
        size_t copy = sv.size() < max_len - 1 ? sv.size() : max_len - 1;
        std::memcpy(buf, sv.data(), copy);
        buf[copy] = '\0';
        return copy;
    } else if constexpr (std::is_pointer_v<std::decay_t<T>>) {
        if constexpr (std::is_convertible_v<T, const char*>) {
            const char* s = static_cast<const char*>(val);
            if (!s) {
                s = "(null)";
            }
            size_t slen = SafeStrLen(s);
            size_t copy = slen < max_len - 1 ? slen : max_len - 1;
            std::memcpy(buf, s, copy);
            buf[copy] = '\0';
            return copy;
        } else {
            auto uval = std::bit_cast<uintptr_t>(val);
            if (max_len < 3) {
                return 0;
            }
            buf[0]         = '0';
            buf[1]         = 'x';
            size_t hex_len = FormatUInt(buf + 2, max_len - 2, uval, 16, false);
            return hex_len + 2;
        }
    } else {
        const char* s    = "?";
        size_t      slen = SafeStrLen(s);
        size_t      copy = slen < max_len - 1 ? slen : max_len - 1;
        std::memcpy(buf, s, copy);
        buf[copy] = '\0';
        return copy;
    }
}

struct FormatOptions {
    bool   hex       = false;
    bool   uppercase = false;
    size_t width     = 0; // Minimum field width (0-padded if hex)
};

} // namespace Detail

// ============================================================================
// ZHLN::BufferPrint (vsnprintf / snprintf Async-Signal Safe Replacement)
// ============================================================================

inline int BufferPrint(char* buf, size_t max_len, const char* fmt, va_list args) noexcept {
    if ((buf == nullptr) || max_len == 0) {
        return 0;
    }

    va_list args_copy;
    va_copy(args_copy, args);

    size_t buf_idx = 0;
    size_t fmt_idx = 0;

    while (fmt[fmt_idx] && buf_idx < max_len - 1) {
        if (fmt[fmt_idx] == '%') {
            fmt_idx++;
            if (!fmt[fmt_idx]) {
                break;
            }

            bool is_long_long = false;
            bool is_long      = false;
            if (fmt[fmt_idx] == 'l') {
                is_long = true;
                fmt_idx++;
                if (fmt[fmt_idx] == 'l') {
                    is_long_long = true;
                    fmt_idx++;
                }
            }

            char spec = fmt[fmt_idx];
            if (spec == '%') {
                buf[buf_idx++] = '%';
                fmt_idx++;
            } else if (spec == 'd' || spec == 'i') {
                int64_t val = 0;
                if (is_long_long) {
                    val = va_arg(args_copy, long long);
                } else if (is_long) {
                    val = va_arg(args_copy, long);
                } else {
                    val = va_arg(args_copy, int);
                }
                char   tmp[32];
                size_t tmp_len = Detail::FormatInt(tmp, sizeof(tmp), val);
                for (size_t i = 0; i < tmp_len && buf_idx < max_len - 1; ++i) {
                    buf[buf_idx++] = tmp[i];
                }
                fmt_idx++;
            } else if (spec == 'u') {
                uint64_t val = 0;
                if (is_long_long) {
                    val = va_arg(args_copy, unsigned long long);
                } else if (is_long) {
                    val = va_arg(args_copy, unsigned long);
                } else {
                    val = va_arg(args_copy, unsigned int);
                }
                char   tmp[32];
                size_t tmp_len = Detail::FormatUInt(tmp, sizeof(tmp), val, 10);
                for (size_t i = 0; i < tmp_len && buf_idx < max_len - 1; ++i) {
                    buf[buf_idx++] = tmp[i];
                }
                fmt_idx++;
            } else if (spec == 'x' || spec == 'X') {
                uint64_t val = 0;
                if (is_long_long) {
                    val = va_arg(args_copy, unsigned long long);
                } else if (is_long) {
                    val = va_arg(args_copy, unsigned long);
                } else {
                    val = va_arg(args_copy, unsigned int);
                }
                char   tmp[32];
                size_t tmp_len = Detail::FormatUInt(tmp, sizeof(tmp), val, 16, spec == 'X');
                for (size_t i = 0; i < tmp_len && buf_idx < max_len - 1; ++i) {
                    buf[buf_idx++] = tmp[i];
                }
                fmt_idx++;
            } else if (spec == 'p') {
                void* val  = va_arg(args_copy, void*);
                auto  uval = std::bit_cast<uintptr_t>(val);
                if (buf_idx < max_len - 1) {
                    buf[buf_idx++] = '0';
                }
                if (buf_idx < max_len - 1) {
                    buf[buf_idx++] = 'x';
                }
                char   tmp[32];
                size_t tmp_len = Detail::FormatUInt(tmp, sizeof(tmp), uval, 16, false);
                for (size_t i = 0; i < tmp_len && buf_idx < max_len - 1; ++i) {
                    buf[buf_idx++] = tmp[i];
                }
                fmt_idx++;
            } else if (spec == 's') {
                const char* s = va_arg(args_copy, const char*);
                if (s == nullptr) {
                    s = "(null)";
                }
                size_t s_len = Detail::SafeStrLen(s);
                for (size_t i = 0; i < s_len && buf_idx < max_len - 1; ++i) {
                    buf[buf_idx++] = s[i];
                }
                fmt_idx++;
            } else if (spec == 'c') {
                char c         = static_cast<char>(va_arg(args_copy, int));
                buf[buf_idx++] = c;
                fmt_idx++;
            } else if (spec == 'f') {
                double val = va_arg(args_copy, double);
                char   tmp[64];
                size_t tmp_len = Detail::FormatDouble(tmp, sizeof(tmp), val, 6);
                for (size_t i = 0; i < tmp_len && buf_idx < max_len - 1; ++i) {
                    buf[buf_idx++] = tmp[i];
                }
                fmt_idx++;
            } else {
                buf[buf_idx++] = '%';
                if (is_long) {
                    buf[buf_idx++] = 'l';
                    if (is_long_long) {
                        buf[buf_idx++] = 'l';
                    }
                }
                if (buf_idx < max_len - 1) {
                    buf[buf_idx++] = spec;
                }
                fmt_idx++;
            }
        } else {
            buf[buf_idx++] = fmt[fmt_idx++];
        }
    }

    buf[buf_idx] = '\0';
    va_end(args_copy);
    return static_cast<int>(buf_idx);
}

inline int BufferPrint(char* buf, size_t max_len, const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    int result = BufferPrint(buf, max_len, fmt, args);
    va_end(args);
    return result;
}

// ============================================================================
// ZHLN::Format (std::format Async-Signal Safe Replacement)
// ============================================================================

template <typename... Args>
inline FormatResult Format(std::string_view fmt, Args&&... args) noexcept {
    size_t poolIdx = 0;
    char*  buf     = SignalSafePool::Acquire(poolIdx);
    if (!buf) {
        return {};
    }

    constexpr size_t argCount = sizeof...(Args);
    if constexpr (argCount == 0) {
        size_t max_len = SignalSafePool::BufferSize;
        size_t buf_idx = 0;
        size_t fmt_idx = 0;
        while (fmt_idx < fmt.size() && buf_idx < max_len - 1) {
            if (fmt[fmt_idx] == '{' && fmt_idx + 1 < fmt.size() && fmt[fmt_idx + 1] == '{') {
                buf[buf_idx++] = '{';
                fmt_idx += 2;
            } else if (fmt[fmt_idx] == '}' && fmt_idx + 1 < fmt.size() && fmt[fmt_idx + 1] == '}') {
                buf[buf_idx++] = '}';
                fmt_idx += 2;
            } else {
                buf[buf_idx++] = fmt[fmt_idx++];
            }
        }
        buf[buf_idx] = '\0';
        return {poolIdx, buf_idx};
    } else {
        using FormatFn = size_t (*)(const void*, char*, size_t, Detail::FormatOptions);

        struct ErasedArg {
            const void* ptr;
            FormatFn    func;
        };

        std::array<ErasedArg, argCount> erasedArgs   = {};
        size_t                          trackedCount = 0;

        auto EraseOne = [&]<typename T>(const T& val) {
            erasedArgs[trackedCount++] = {
                .ptr = std::addressof(val), .func = [](const void* ptr, char* b, size_t len, Detail::FormatOptions opts) -> size_t {
                    using DecayedT = std::decay_t<T>;
                    if constexpr (std::is_integral_v<DecayedT> && !std::is_same_v<DecayedT, bool> && !std::is_same_v<DecayedT, char>) {
                        if (opts.hex) {
                            return Detail::FormatUInt(b, len, static_cast<uint64_t>(*static_cast<const T*>(ptr)), 16, opts.uppercase);
                        }
                    }
                    return Detail::AppendValue(b, len, *static_cast<const T*>(ptr));
                }
            };
        };
        (EraseOne(args), ...);

        auto BoundAppenders = [&](size_t idx, char* b, size_t len, Detail::FormatOptions opts) -> size_t {
            if (idx >= argCount) {
                return 0;
            }
            return erasedArgs[idx].func(erasedArgs[idx].ptr, b, len, opts);
        };

        size_t buf_idx      = 0;
        size_t fmt_idx      = 0;
        size_t next_arg_idx = 0;
        size_t max_len      = SignalSafePool::BufferSize;

        while (fmt_idx < fmt.size() && buf_idx < max_len - 1) {
            if (fmt[fmt_idx] == '{') {
                if (fmt_idx + 1 < fmt.size() && fmt[fmt_idx + 1] == '}') {
                    if (next_arg_idx < argCount) {
                        size_t remaining = max_len - 1 - buf_idx;
                        size_t written   = BoundAppenders(next_arg_idx, buf + buf_idx, remaining, {});
                        buf_idx += written;
                        next_arg_idx++;
                    } else {
                        const char* err     = "{!OUT_OF_ARGS}";
                        size_t      err_len = Detail::SafeStrLen(err);
                        for (size_t i = 0; i < err_len && buf_idx < max_len - 1; ++i) {
                            buf[buf_idx++] = err[i];
                        }
                    }
                    fmt_idx += 2;
                } else if (fmt_idx + 1 < fmt.size() && fmt[fmt_idx + 1] == '{') {
                    buf[buf_idx++] = '{';
                    fmt_idx += 2;
                } else {
                    size_t close_idx = fmt.find('}', fmt_idx);
                    if (close_idx != std::string_view::npos) {
                        std::string_view spec = fmt.substr(fmt_idx + 1, close_idx - (fmt_idx + 1));
                        if (next_arg_idx < argCount) {
                            size_t remaining = max_len - 1 - buf_idx;

                            bool is_hex   = spec.starts_with(':') && (spec.ends_with('x') || spec.ends_with('X'));
                            bool is_upper = spec.ends_with('X');

                            // Parse width from spec (e.g., ":02X" -> width=2)
                            size_t width = 0;
                            if (spec.starts_with(':')) {
                                std::string_view width_str = spec.substr(1);
                                // Remove trailing x/X
                                if (width_str.ends_with('x') || width_str.ends_with('X')) {
                                    width_str.remove_suffix(1);
                                }
                                // Parse digits
                                for (char c: width_str) {
                                    if (c >= '0' && c <= '9') {
                                        width = width * 10 + (c - '0');
                                    } else {
                                        break; // Stop at non-digit
                                    }
                                }
                            }

                            // Format the value
                            size_t written = BoundAppenders(next_arg_idx, buf + buf_idx, remaining, {.hex = is_hex, .uppercase = is_upper, .width = width});

                            // Apply zero-padding for hex values if needed
                            if (is_hex && width > written && remaining > (width - written)) {
                                // Shift existing content to the right
                                for (int i = static_cast<int>(written) - 1; i >= 0; --i) {
                                    buf[buf_idx + width - written + i] = buf[buf_idx + i];
                                }
                                // Fill leading zeros
                                for (size_t i = 0; i < (width - written); ++i) {
                                    buf[buf_idx + i] = '0';
                                }
                                buf_idx += width;
                            } else {
                                buf_idx += written;
                            }
                            next_arg_idx++;
                        }
                        fmt_idx = close_idx + 1;
                    } else {
                        buf[buf_idx++] = fmt[fmt_idx++];
                    }
                }
            } else if (fmt[fmt_idx] == '}') {
                if (fmt_idx + 1 < fmt.size() && fmt[fmt_idx + 1] == '}') {
                    buf[buf_idx++] = '}';
                    fmt_idx += 2;
                } else {
                    buf[buf_idx++] = fmt[fmt_idx++];
                }
            } else {
                buf[buf_idx++] = fmt[fmt_idx++];
            }
        }

        buf[buf_idx] = '\0';
        return {poolIdx, buf_idx};
    }
}

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
    // Map standard streams safely to raw file descriptors without calling fileno()
    int fd = (stream == stderr) ? 2 : 1;
    Print(fd, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void Print(std::string_view fmt, Args&&... args) noexcept {
    Print(1, fmt, std::forward<Args>(args)...); // Default to stdout
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
