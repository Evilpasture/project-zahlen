// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include "PushDataLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ZHLN::Vk {

struct SpirvLayoutField {
    uint32_t nameOffset = 0;
    uint32_t nameLength = 0;
    uint32_t offset     = 0;
    uint32_t size       = 0;

    [[nodiscard]] constexpr auto IsNamed(std::span<const uint8_t> module, std::string_view name) const noexcept -> bool {
        if (name.size() != nameLength || static_cast<size_t>(nameOffset) + nameLength > module.size()) {
            return false;
        }
        for (uint32_t i = 0; i < nameLength; ++i) {
            if (module[nameOffset + i] != static_cast<uint8_t>(name[i])) {
                return false;
            }
        }
        return true;
    }
};

struct SpirvPushBlock {
    static constexpr uint32_t kCapacity = 32;

    uint32_t                                nameOffset = 0;
    uint32_t                                nameLength = 0;
    uint32_t                                extent     = 0;
    uint32_t                                count      = 0;
    std::array<SpirvLayoutField, kCapacity> fields {};
    bool                                    complete   = true;

    [[nodiscard]] constexpr auto Count() const noexcept -> uint32_t {
        return count;
    }
};

struct SpirvTypeLookup {
    uint32_t size      = 0;
    uint32_t extent    = 0;
    bool     found     = false;
    bool     ambiguous = false;
};


class SpirvTypes {
  public:
    static constexpr uint32_t kSpirvMagic                    = 0x07230203;
    static constexpr size_t   kSpirvHeaderWords              = 5;
    static constexpr uint16_t kSpirvOpName                   = 5;
    static constexpr uint16_t kSpirvOpMemberName             = 6;
    static constexpr uint16_t kSpirvOpTypeInt                = 21;
    static constexpr uint16_t kSpirvOpTypeFloat              = 22;
    static constexpr uint16_t kSpirvOpTypeVector             = 23;
    static constexpr uint16_t kSpirvOpTypeMatrix             = 24;
    static constexpr uint16_t kSpirvOpTypeArray              = 28;
    static constexpr uint16_t kSpirvOpTypeRuntimeArray       = 29;
    static constexpr uint16_t kSpirvOpTypeStruct             = 30;
    static constexpr uint16_t kSpirvOpTypePointer            = 32;
    static constexpr uint16_t kSpirvOpConstant               = 43;
    static constexpr uint16_t kSpirvOpSpecConstant           = 50;
    static constexpr uint16_t kSpirvOpFunction               = 54;
    static constexpr uint16_t kSpirvOpVariable               = 59;
    static constexpr uint16_t kSpirvOpDecorate               = 71;
    static constexpr uint16_t kSpirvOpMemberDecorate         = 72;
    static constexpr uint16_t kSpirvDecorationArrayStride    = 6;
    static constexpr uint16_t kSpirvDecorationMatrixStride   = 7;
    static constexpr uint16_t kSpirvDecorationOffset         = 35;
    static constexpr uint32_t kSpirvStoragePushConstant      = 9;

    static constexpr uint32_t kTypeCapacity        = 256;
    static constexpr uint32_t kMemberCapacity      = 512;
    static constexpr uint32_t kNameCapacity        = 192;
    static constexpr uint32_t kMemberNameCapacity  = 384;
    static constexpr uint32_t kStrideCapacity      = 128;
    static constexpr uint32_t kDecorationCapacity  = 384;
    static constexpr uint32_t kConstantCapacity    = 512;
    static constexpr uint32_t kVariableCapacity    = 32;

    constexpr SpirvTypes() noexcept = default;

    [[nodiscard]] static constexpr auto Parse(std::span<const uint8_t> module) noexcept -> SpirvTypes;

    [[nodiscard]] constexpr auto Complete() const noexcept -> bool {
        return !_truncated;
    }
    [[nodiscard]] constexpr auto Bytes() const noexcept -> std::span<const uint8_t> {
        return _bytes;
    }

    [[nodiscard]] constexpr auto LookupStruct(std::string_view name) const noexcept -> SpirvTypeLookup;

    [[nodiscard]] constexpr auto StructSize(std::string_view name) const noexcept -> uint32_t {
        return LookupStruct(name).size;
    }

    [[nodiscard]] constexpr auto PushBlock() const noexcept -> SpirvPushBlock;

    [[nodiscard]] constexpr auto HeapPushData(std::string_view typeName) const noexcept -> std::optional<HeapPushDataLayout>;

  private:
    enum class Kind : uint8_t { Scalar, Vector, Matrix, Array, RuntimeArray, Struct, Pointer, Opaque };
    enum class Decoration : uint8_t { ArrayStride, MatrixStride, MemberOffset, MemberMatrixStride };

    struct Type {
        uint32_t id         = 0;
        uint32_t first      = 0;
        uint32_t count      = 0;
        uint32_t extra      = 0;
        uint32_t nameOffset = 0;
        uint32_t nameLength = 0;
        Kind     kind       = Kind::Opaque;
    };
    struct Member {
        uint32_t typeId     = 0;
        uint32_t offset     = 0;
        uint32_t nameOffset = 0;
        uint32_t nameLength = 0;
    };
    struct Name {
        uint32_t id     = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
    };
    struct MemberName {
        uint32_t structId = 0;
        uint32_t index    = 0;
        uint32_t offset   = 0;
        uint32_t length   = 0;
    };
    struct Stride {
        uint32_t typeId = 0;
        uint32_t stride = 0;
    };
    struct DecorationEntry {
        uint32_t          target = 0;
        uint32_t          member = 0;
        uint32_t          value  = 0;
        Vk::SpirvTypes::Decoration kind = Decoration::ArrayStride;
    };
    struct Constant {
        uint32_t id    = 0;
        uint32_t value = 0;
    };
    struct Variable {
        uint32_t id           = 0;
        uint32_t typeId       = 0;
        uint32_t storageClass = 0;
    };

    std::array<Type, kTypeCapacity>               _types {};
    std::array<Member, kMemberCapacity>           _members {};
    std::array<Name, kNameCapacity>               _names {};
    std::array<MemberName, kMemberNameCapacity>   _memberNames {};
    std::array<Stride, kStrideCapacity>           _strides {};
    std::array<DecorationEntry, kDecorationCapacity> _decorations {};
    std::array<Constant, kConstantCapacity>       _constants {};
    std::array<Variable, kVariableCapacity>       _variables {};
    std::span<const uint8_t>                      _bytes {};
    uint32_t                                      _typeCount        = 0;
    uint32_t                                      _memberCount      = 0;
    uint32_t                                      _nameCount        = 0;
    uint32_t                                      _memberNameCount  = 0;
    uint32_t                                      _strideCount      = 0;
    uint32_t                                      _decorationCount  = 0;
    uint32_t                                      _constantCount    = 0;
    uint32_t                                      _variableCount    = 0;
    bool                                          _truncated        = false;

    [[nodiscard]] constexpr auto TypeIndexAt(uint32_t id) const noexcept -> std::optional<uint32_t> {
        for (uint32_t i = 0; i < _typeCount; ++i) {
            if (_types[i].id == id) {
                return i;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] constexpr auto TypeNameAt(uint32_t id, uint32_t* offset, uint32_t* length) const noexcept -> bool {
        for (uint32_t i = 0; i < _nameCount; ++i) {
            if (_names[i].id == id) {
                *offset = _names[i].offset;
                *length = _names[i].length;
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] constexpr auto MemberNameAt(uint32_t structId, uint32_t index, uint32_t* offset, uint32_t* length) const noexcept -> bool {
        for (uint32_t i = 0; i < _memberNameCount; ++i) {
            if (_memberNames[i].structId == structId && _memberNames[i].index == index) {
                *offset = _memberNames[i].offset;
                *length = _memberNames[i].length;
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] constexpr auto StrideOf(uint32_t typeId) const noexcept -> std::optional<uint32_t> {
        for (uint32_t i = 0; i < _strideCount; ++i) {
            if (_strides[i].typeId == typeId) {
                return _strides[i].stride;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] constexpr auto DecorationFor(uint32_t target, uint32_t member, Decoration kind) const noexcept -> std::optional<uint32_t> {
        for (uint32_t i = 0; i < _decorationCount; ++i) {
            if (_decorations[i].target == target && _decorations[i].member == member && _decorations[i].kind == kind) {
                return _decorations[i].value;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] constexpr auto ConstantValue(uint32_t id) const noexcept -> std::optional<uint32_t> {
        for (uint32_t i = 0; i < _constantCount; ++i) {
            if (_constants[i].id == id) {
                return _constants[i].value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr auto AlignOf(uint32_t id) const noexcept -> uint32_t;
    [[nodiscard]] constexpr auto ExtentOf(uint32_t id) const noexcept -> uint32_t;
    [[nodiscard]] constexpr auto SizeOf(uint32_t id) const noexcept -> uint32_t {
        const std::optional<uint32_t> typeIndex = TypeIndexAt(id);
        if (!typeIndex || _types[*typeIndex].kind != Kind::Struct) {
            return ExtentOf(id);
        }
        return Vk::AlignUp(ExtentOf(id), AlignOf(id));
    }
    [[nodiscard]] constexpr auto NameMatches(const Type& type, std::string_view name) const noexcept -> bool;
    [[nodiscard]] constexpr auto FirstStructIndex(std::string_view name) const noexcept -> std::optional<uint32_t>;
};


[[nodiscard]] constexpr auto SpirvTypes::Parse(std::span<const uint8_t> module) noexcept -> SpirvTypes {
    SpirvTypes out {};
    out._bytes = module;

    if (module.size() < kSpirvHeaderWords * 4 || (module.size() % 4) != 0) {
        out._truncated = true;
        return out;
    }
    const size_t words = module.size() / 4;

    const auto wordAt = [&](size_t index) noexcept -> uint32_t {
        return static_cast<uint32_t>(module[index * 4]) | (static_cast<uint32_t>(module[index * 4 + 1]) << 8) |
               (static_cast<uint32_t>(module[index * 4 + 2]) << 16) | (static_cast<uint32_t>(module[index * 4 + 3]) << 24);
    };
    const auto countAt = [&](size_t word) noexcept -> uint32_t {
        return static_cast<uint32_t>(module[word * 4 + 2]) | (static_cast<uint32_t>(module[word * 4 + 3]) << 8);
    };
    const auto stringAt = [&](size_t first, uint32_t count, uint32_t* offset, uint32_t* length) noexcept -> bool {
        const uint32_t start = static_cast<uint32_t>(first * 4);
        const uint32_t span  = count * 4;
        uint32_t       size  = 0;
        while (size < span && start + size < module.size() && module[start + size] != 0) {
            ++size;
        }
        if (size == span || start + size >= module.size()) {
            return false;
        }
        *offset = start;
        *length = size;
        return true;
    };

    if (wordAt(0) != kSpirvMagic) {
        out._truncated = true;
        return out;
    }

    for (size_t word = kSpirvHeaderWords; word < words;) {
        const uint32_t count = countAt(word);
        if (count == 0 || word + count > words) {
            out._truncated = true;
            return out;
        }
        const uint32_t opcode = static_cast<uint32_t>(module[word * 4]) | (static_cast<uint32_t>(module[word * 4 + 1]) << 8);
        uint32_t       offset = 0;
        uint32_t       length = 0;

        switch (opcode) {
            case kSpirvOpName:
                if (count >= 3) {
                    if (out._nameCount == kNameCapacity || !stringAt(word + 2, count - 2, &offset, &length)) {
                        out._truncated = true;
                        return out;
                    }
                    out._names[out._nameCount++] = Name {.id = wordAt(word + 1), .offset = offset, .length = length};
                }
                break;
            case kSpirvOpMemberName:
                if (count >= 4) {
                    if (out._memberNameCount == kMemberNameCapacity || !stringAt(word + 3, count - 3, &offset, &length)) {
                        out._truncated = true;
                        return out;
                    }
                    out._memberNames[out._memberNameCount++] =
                        MemberName {.structId = wordAt(word + 1), .index = wordAt(word + 2), .offset = offset, .length = length};
                }
                break;
            case kSpirvOpTypeInt:
            case kSpirvOpTypeFloat:
                if (count >= 3) {
                    if (out._typeCount == kTypeCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    out._types[out._typeCount++] = Type {.id = wordAt(word + 1), .extra = wordAt(word + 2), .kind = Kind::Scalar};
                }
                break;
            case kSpirvOpTypeVector:
                if (count >= 4) {
                    if (out._typeCount == kTypeCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    out._types[out._typeCount++] =
                        Type {.id = wordAt(word + 1), .first = wordAt(word + 2), .count = wordAt(word + 3), .kind = Kind::Vector};
                }
                break;
            case kSpirvOpTypeMatrix:
                if (count >= 4) {
                    if (out._typeCount == kTypeCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    out._types[out._typeCount++] =
                        Type {.id = wordAt(word + 1), .first = wordAt(word + 2), .count = wordAt(word + 3), .kind = Kind::Matrix};
                }
                break;
            case kSpirvOpTypeArray:
                if (count >= 4) {
                    if (out._typeCount == kTypeCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    out._types[out._typeCount++] =
                        Type {.id = wordAt(word + 1), .first = wordAt(word + 2), .count = wordAt(word + 3), .kind = Kind::Array};
                }
                break;
            case kSpirvOpTypeRuntimeArray:
                if (count >= 3) {
                    if (out._typeCount == kTypeCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    out._types[out._typeCount++] = Type {.id = wordAt(word + 1), .first = wordAt(word + 2), .kind = Kind::RuntimeArray};
                }
                break;
            case kSpirvOpTypeStruct: {
                if (count >= 2) {
                    if (out._typeCount == kTypeCapacity || out._memberCount + (count - 2) > kMemberCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    const uint32_t first = out._memberCount;
                    for (uint32_t i = 0; i + 2 < count; ++i) {
                        out._members[out._memberCount++] = Member {.typeId = wordAt(word + 2 + i)};
                    }
                    out._types[out._typeCount++] = Type {.id = wordAt(word + 1), .first = first, .count = count - 2, .kind = Kind::Struct};
                }
                break;
            }
            case kSpirvOpTypePointer:
                if (count >= 4) {
                    if (out._typeCount == kTypeCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    out._types[out._typeCount++] =
                        Type {.id = wordAt(word + 1), .first = wordAt(word + 3), .extra = wordAt(word + 2), .kind = Kind::Pointer};
                }
                break;
            case kSpirvOpConstant:
            case kSpirvOpSpecConstant:
                if (count >= 4) {
                    if (out._constantCount == kConstantCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    out._constants[out._constantCount++] = Constant {.id = wordAt(word + 2), .value = wordAt(word + 3)};
                }
                break;
            case kSpirvOpVariable:
                if (count >= 4) {
                    if (out._variableCount == kVariableCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    out._variables[out._variableCount++] =
                        Variable {.id = wordAt(word + 2), .typeId = wordAt(word + 1), .storageClass = wordAt(word + 3)};
                }
                break;
            case kSpirvOpDecorate:
                if (count >= 4) {
                    const uint32_t decoration = wordAt(word + 2);
                    if (decoration == kSpirvDecorationArrayStride || decoration == kSpirvDecorationMatrixStride) {
                        if (out._decorationCount == kDecorationCapacity) {
                            out._truncated = true;
                            return out;
                        }
                        out._decorations[out._decorationCount++] = DecorationEntry {
                            .target = wordAt(word + 1),
                            .value  = wordAt(word + 3),
                            .kind   = decoration == kSpirvDecorationMatrixStride ? Decoration::MatrixStride : Decoration::ArrayStride,
                        };
                    }
                }
                break;
            case kSpirvOpMemberDecorate:
                if (count >= 5) {
                    const uint32_t decoration = wordAt(word + 3);
                    if (decoration == kSpirvDecorationOffset || decoration == kSpirvDecorationMatrixStride) {
                        if (out._decorationCount == kDecorationCapacity) {
                            out._truncated = true;
                            return out;
                        }
                        out._decorations[out._decorationCount++] = DecorationEntry {
                            .target = wordAt(word + 1),
                            .member = wordAt(word + 2),
                            .value  = wordAt(word + 4),
                            .kind   = decoration == kSpirvDecorationOffset ? Decoration::MemberOffset : Decoration::MemberMatrixStride,
                        };
                    }
                }
                break;
            case kSpirvOpFunction:
                word = words;
                continue;
            default:
                break;
        }
        word += count;
    }

    for (uint32_t i = 0; i < out._typeCount; ++i) {
        uint32_t offset = 0;
        uint32_t length = 0;
        if (out.TypeNameAt(out._types[i].id, &offset, &length)) {
            out._types[i].nameOffset = offset;
            out._types[i].nameLength = length;
        }
    }
    for (uint32_t t = 0; t < out._typeCount; ++t) {
        if (out._types[t].kind != Kind::Struct) {
            continue;
        }
        const uint32_t structId = out._types[t].id;
        for (uint32_t index = 0; index < out._types[t].count; ++index) {
            Member&      member   = out._members[out._types[t].first + index];
            uint32_t     offset   = 0;
            uint32_t     length   = 0;
            const auto   offsetAt = out.DecorationFor(structId, index, Decoration::MemberOffset);
            member.offset         = offsetAt.has_value() ? *offsetAt : 0;
            if (out.MemberNameAt(structId, index, &offset, &length)) {
                member.nameOffset = offset;
                member.nameLength = length;
            }
            const auto stride = out.DecorationFor(structId, index, Decoration::MemberMatrixStride);
            const std::optional<uint32_t> memberIndex = out.TypeIndexAt(member.typeId);
            if (stride.has_value() && memberIndex.has_value() && out._types[*memberIndex].kind == Kind::Matrix) {
                if (out._strideCount == kStrideCapacity) {
                    out._truncated = true;
                    return out;
                }
                out._strides[out._strideCount++] = Stride {.typeId = member.typeId, .stride = *stride};
            }
        }
    }
    for (uint32_t i = 0; i < out._decorationCount; ++i) {
        const DecorationEntry& entry = out._decorations[i];
        if (entry.kind != Decoration::ArrayStride && entry.kind != Decoration::MatrixStride) {
            continue;
        }
        if (out._strideCount == kStrideCapacity) {
            out._truncated = true;
            return out;
        }
        bool known = false;
        for (uint32_t s = 0; s < out._strideCount; ++s) {
            if (out._strides[s].typeId == entry.target) {
                known = true;
            }
        }
        if (!known) {
            out._strides[out._strideCount++] = Stride {.typeId = entry.target, .stride = entry.value};
        }
    }
    return out;
}

[[nodiscard]] constexpr auto SpirvTypes::AlignOf(uint32_t id) const noexcept -> uint32_t {
    const std::optional<uint32_t> typeIndex = TypeIndexAt(id);
    if (!typeIndex) {
        return 1;
    }
    const Type* type = &_types[*typeIndex];
    switch (type->kind) {
        case Kind::Scalar:
            return type->extra / 8;
        case Kind::Vector:
            if (type->count >= 3) {
                return 16;
            }
            if (type->count == 2) {
                return 8;
            }
            return AlignOf(type->first);
        case Kind::Matrix:
            return 16;
        case Kind::Array:
            return AlignOf(type->first) > 16 ? AlignOf(type->first) : 16;
        case Kind::Struct: {
            uint32_t alignment = 1;
            for (uint32_t index = 0; index < type->count; ++index) {
                const uint32_t member = AlignOf(_members[type->first + index].typeId);
                alignment              = member > alignment ? member : alignment;
            }
            return alignment;
        }
        case Kind::Pointer:
            return 8;
        default:
            return 4;
    }
}

[[nodiscard]] constexpr auto SpirvTypes::ExtentOf(uint32_t id) const noexcept -> uint32_t {
    const std::optional<uint32_t> typeIndex = TypeIndexAt(id);
    if (!typeIndex) {
        return 0;
    }
    const Type* type = &_types[*typeIndex];
    switch (type->kind) {
        case Kind::Scalar:
            return type->extra / 8;
        case Kind::Vector:
            return ExtentOf(type->first) * type->count;
        case Kind::Matrix: {
            const auto     stride = StrideOf(id);
            const uint32_t column = ExtentOf(type->first);
            return stride.has_value() ? *stride * type->count : column * type->count;
        }
        case Kind::Array: {
            const auto     length = ConstantValue(type->count);
            const uint32_t count  = length.has_value() ? *length : 0;
            const auto     stride = StrideOf(id);
            return stride.has_value() ? *stride * count : ExtentOf(type->first) * count;
        }
        case Kind::Pointer:
            return 8;
        case Kind::Struct: {
            uint32_t end = 0;
            for (uint32_t index = 0; index < type->count; ++index) {
                const Member&  member = _members[type->first + index];
                const uint32_t reach  = member.offset + ExtentOf(member.typeId);
                end                   = reach > end ? reach : end;
            }
            return end;
        }
        default:
            return 0;
    }
}

[[nodiscard]] constexpr auto SpirvTypes::NameMatches(const Type& type, std::string_view name) const noexcept -> bool {
    if (type.kind != Kind::Struct || type.nameLength == 0 || name.empty()) {
        return false;
    }
    const uint32_t start = type.nameOffset;
    const uint32_t size  = type.nameLength;
    if (static_cast<size_t>(start) + size > _bytes.size() || size < name.size()) {
        return false;
    }
    const auto is = [&](uint32_t at, std::string_view text) noexcept -> bool {
        for (size_t i = 0; i < text.size(); ++i) {
            if (at + i >= _bytes.size() || _bytes[at + i] != static_cast<uint8_t>(text[i])) {
                return false;
            }
        }
        return true;
    };
    if (size == name.size()) {
        return is(start, name);
    }
    if (is(start, name) && _bytes[start + name.size()] == static_cast<uint8_t>('_')) {
        return true;
    }
    for (uint32_t i = 0; i < size; ++i) {
        if (_bytes[start + i] == static_cast<uint8_t>('.') && size - (i + 1) == name.size() && is(start + i + 1, name)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr auto SpirvTypes::FirstStructIndex(std::string_view name) const noexcept -> std::optional<uint32_t> {
    for (uint32_t i = 0; i < _typeCount; ++i) {
        if (NameMatches(_types[i], name)) {
            return i;
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr auto SpirvTypes::LookupStruct(std::string_view name) const noexcept -> SpirvTypeLookup {
    SpirvTypeLookup found {};
    for (uint32_t i = 0; i < _typeCount; ++i) {
        const Type& type = _types[i];
        if (!NameMatches(type, name)) {
            continue;
        }
        const uint32_t size = SizeOf(type.id);
        if (!found.found) {
            found.found  = true;
            found.size   = size;
            found.extent = ExtentOf(type.id);
        } else if (size != found.size) {
            found.ambiguous = true;
        }
    }
    return found;
}

[[nodiscard]] constexpr auto SpirvTypes::PushBlock() const noexcept -> SpirvPushBlock {
    SpirvPushBlock out {};
    for (uint32_t i = 0; i < _variableCount; ++i) {
        const Variable& variable = _variables[i];
        if (variable.storageClass != kSpirvStoragePushConstant) {
            continue;
        }
        const std::optional<uint32_t> pointerIndex = TypeIndexAt(variable.typeId);
        if (!pointerIndex || _types[*pointerIndex].kind != Kind::Pointer) {
            continue;
        }
        const Type* pointer = &_types[*pointerIndex];
        const std::optional<uint32_t> blockIndex = TypeIndexAt(pointer->first);
        if (!blockIndex || _types[*blockIndex].kind != Kind::Struct) {
            continue;
        }
        const Type* block = &_types[*blockIndex];
        (void)TypeNameAt(variable.id, &out.nameOffset, &out.nameLength);
        out.complete = block->count <= SpirvPushBlock::kCapacity;
        out.count    = block->count < SpirvPushBlock::kCapacity ? block->count : SpirvPushBlock::kCapacity;
        out.extent   = ExtentOf(block->id);
        for (uint32_t index = 0; index < out.count; ++index) {
            const Member& member         = _members[block->first + index];
            out.fields[index].nameOffset = member.nameOffset;
            out.fields[index].nameLength = member.nameLength;
            out.fields[index].offset     = member.offset;
            out.fields[index].size       = ExtentOf(member.typeId);
        }
        return out;
    }
    return out;
}

[[nodiscard]] constexpr auto SpirvTypes::HeapPushData(std::string_view typeName) const noexcept -> std::optional<HeapPushDataLayout> {
    const SpirvTypeLookup         lookup    = LookupStruct(typeName);
    const std::optional<uint32_t> typeIndex = FirstStructIndex(typeName);
    if (!lookup.found || lookup.ambiguous || !typeIndex) {
        return std::nullopt;
    }
    const Type* type = &_types[*typeIndex];

    HeapPushDataLayout layout;
    uint32_t           addressCount = 0;
    for (uint32_t index = 0; index < type->count; ++index) {
        const Member&  member = _members[type->first + index];
        const uint32_t size   = ExtentOf(member.typeId);
        if (size != sizeof(uint64_t) || (member.offset % alignof(uint64_t)) != 0) {
            continue;
        }
        if (addressCount == HeapPushDataLayout::kMaxAddresses) {
            return std::nullopt;
        }
        if (addressCount > 0 && member.offset < layout.frameAddressOffsets[addressCount - 1] + sizeof(uint64_t)) {
            return std::nullopt;
        }
        layout.frameAddressOffsets[addressCount++] = member.offset;
    }
    if (addressCount == 0) {
        return std::nullopt;
    }
    layout.addressCount = addressCount;

    const uint32_t after = layout.frameAddressOffsets[addressCount - 1] + sizeof(uint64_t);
    uint32_t       index = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < type->count; ++i) {
        const Member& member = _members[type->first + i];
        if (member.offset < after || (member.offset % alignof(uint32_t)) != 0 || ExtentOf(member.typeId) < sizeof(uint32_t)) {
            continue;
        }
        index = member.offset < index ? member.offset : index;
    }
    if (index == 0xFFFFFFFFu) {
        return std::nullopt;
    }
    layout.heapIndexOffset = index;
    layout.requiredSize    = index + sizeof(uint32_t);
    return layout;
}

}
