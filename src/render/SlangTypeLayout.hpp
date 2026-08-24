// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Error.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Vk {

enum class SpirvLayoutError : uint8_t {
    Success = 0,
    InvalidArguments[[= ZHLN::Reflect::Description("SPIR-V blob or type name is empty")]],
    ModuleParseFailed[[= ZHLN::Reflect::Description("Failed to parse SPIR-V module")]],
    TypeNotFound[[= ZHLN::Reflect::Description("Type was not found in compiled SPIR-V")]],
    EmptyLayout[[= ZHLN::Reflect::Description("Reflected type has zero size")]],
    TypeSizeMismatch[[= ZHLN::Reflect::Description("GPU type size does not match the C++ host type")]],
    HeapPushAddressCount[[= ZHLN::Reflect::Description("DescriptorHeapPushData device-address count does not match the host")]],
    HeapPushIndexMissing[[= ZHLN::Reflect::Description("DescriptorHeapPushData is missing a descriptor-index word after the frame addresses")]],
    HeapPushOverlapsPassData[[= ZHLN::Reflect::Description("DescriptorHeapPushData frame addresses overlap the per-pass push blob")]],
};

/// Empty tag whose reflected identifier is the Slang type name
/// `DescriptorHeapPushData`. Rename this with the Slang type.
struct DescriptorHeapPushData {};

inline constexpr uint32_t kHeapFrameAddressCount = 6;

/// Byte offsets in the vkCmdPushDataEXT blob, reflected from
/// DescriptorHeapPushData in compiled SPIR-V. Address slots are taken in
/// Slang declaration order (every 8-byte member); the first 4-byte word
/// after those addresses is the descriptor index.
struct HeapPushDataLayout {
    std::array<uint32_t, kHeapFrameAddressCount> frameAddressOffsets {};
    uint32_t                                     heapIndexOffset = 0;
    uint32_t                                     requiredSize    = 0;
};

[[nodiscard]] auto ReflectHeapPushDataLayout(const void* spirv, size_t sizeBytes) noexcept -> std::expected<HeapPushDataLayout, ZHLN::Error>;

struct SlangTypeField {
    std::string name;
    uint32_t    offset = 0;
    uint32_t    size   = 0;
};

struct SlangTypeLayout {
    uint32_t                    size      = 0;
    uint32_t                    alignment = 0;
    std::vector<SlangTypeField> fields;

    [[nodiscard]] auto FieldOffset(std::string_view name) const noexcept -> std::optional<uint32_t>;
    [[nodiscard]] auto FieldSize(std::string_view name) const noexcept -> std::optional<uint32_t>;
};

/// Reflects a named struct from compiled SPIR-V (UBO / SSBO / push-constant
/// blocks and their nested members). Layout authority is the slangc output
/// the engine already embeds — this file never sees `.slang` source.
[[nodiscard]] auto ReflectTypeLayout(const void* spirv, size_t sizeBytes, std::string_view typeName) noexcept
    -> std::expected<SlangTypeLayout, ZHLN::Error>;

/// Writes `value` at the reflected field offset inside a push-data blob.
template <typename T>
bool WriteReflectedField(std::span<std::byte> blob, const SlangTypeLayout& layout, std::string_view name, const T& value) noexcept {
    auto offset = layout.FieldOffset(name);
    auto size   = layout.FieldSize(name);
    if (!offset || !size || *size < sizeof(T) || *offset + sizeof(T) > blob.size()) {
        return false;
    }
    std::memcpy(blob.data() + *offset, &value, sizeof(T));
    return true;
}

} // namespace ZHLN::Vk
