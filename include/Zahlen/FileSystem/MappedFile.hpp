// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN::FS {

enum class MapAccess : uint8_t {
    ReadOnly = 0,
    ReadWrite,
    CopyOnWrite,
};

// Move-only RAII memory-mapped file view.
//
// Owns the OS file/mapping handles for the lifetime of the view and unmaps
// automatically on destruction or `Reset()`. Zero-length or missing files map
// to an invalid (`!IsValid()`) handle without throwing or leaking descriptors.
class ZHLN_API MappedFile {
  public:
    MappedFile() noexcept = default;
    ~MappedFile() noexcept;

    MappedFile(const MappedFile&)                    = delete;
    auto operator=(const MappedFile&) -> MappedFile& = delete;

    MappedFile(MappedFile&& other) noexcept;
    auto operator=(MappedFile&& other) noexcept -> MappedFile&;

    [[nodiscard]] static auto Open(std::string_view filepath, MapAccess access = MapAccess::ReadOnly) noexcept -> MappedFile;

    void Reset() noexcept;

    [[nodiscard]] auto IsValid() const noexcept -> bool {
        return _data != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return IsValid();
    }

    [[nodiscard]] auto Size() const noexcept -> size_t {
        return _size;
    }

    [[nodiscard]] auto Access() const noexcept -> MapAccess {
        return _access;
    }

    [[nodiscard]] auto Data() const noexcept -> const std::byte* {
        return static_cast<const std::byte*>(_data);
    }

    [[nodiscard]] auto MutableData() noexcept -> std::byte* {
        return _access == MapAccess::ReadOnly ? nullptr : static_cast<std::byte*>(_data);
    }

    [[nodiscard]] auto Bytes() const noexcept -> std::span<const std::byte> {
        return _data != nullptr ? std::span<const std::byte>(static_cast<const std::byte*>(_data), _size) : std::span<const std::byte> {};
    }

    [[nodiscard]] auto MutableBytes() noexcept -> std::span<std::byte> {
        return (_data != nullptr && _access != MapAccess::ReadOnly)
                   ? std::span<std::byte>(static_cast<std::byte*>(_data), _size)
                   : std::span<std::byte> {};
    }

    [[nodiscard]] auto Chars() const noexcept -> std::span<const char> {
        return _data != nullptr ? std::span<const char>(static_cast<const char*>(_data), _size) : std::span<const char> {};
    }

  private:
    void*     _data   = nullptr;
    size_t    _size   = 0;
    MapAccess _access = MapAccess::ReadOnly;
#if defined(_WIN32)
    void* _fileHandle    = nullptr;
    void* _mappingHandle = nullptr;
#else
    int _fd = -1;
#endif
};

} // namespace ZHLN::FS
