// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::Test {

// Owns a growing byte buffer while fixtures are assembled, then transfers the
// completed dynamic array to the parser under test.
class BinaryWriter {
  public:
    void reserve(size_t capacity) {
        _bytes.reserve(capacity);
    }

    void resize(size_t size, std::byte value = std::byte {}) {
        _bytes.resize(size, value);
    }

    void push_back(std::byte value) {
        _bytes.push_back(value);
    }

    [[nodiscard]] auto size() const noexcept -> size_t {
        return _bytes.size();
    }

    [[nodiscard]] auto data() noexcept -> std::byte* {
        return _bytes.data();
    }

    [[nodiscard]] auto data() const noexcept -> const std::byte* {
        return _bytes.data();
    }

    [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte> {
        return _bytes;
    }

    [[nodiscard]] auto writable_bytes() noexcept -> std::span<std::byte> {
        return _bytes;
    }

    template <typename T>
    void append_object(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        append(std::as_bytes(std::span<const T> {&value, 1}));
    }

    template <typename T>
    void append(std::span<const T> values) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (values.empty()) {
            return;
        }
        const auto raw = std::as_bytes(values);
        _bytes.insert(_bytes.end(), raw.begin(), raw.end());
    }

    void append_text(std::string_view text) {
        append(std::as_bytes(std::span<const char> {text.data(), text.size()}));
    }

    void append_byte(uint8_t value) {
        _bytes.push_back(static_cast<std::byte>(value));
    }

    [[nodiscard]] auto take() && -> std::vector<std::byte> {
        return std::move(_bytes);
    }

  private:
    std::vector<std::byte> _bytes;
};

} // namespace ZHLN::Test
