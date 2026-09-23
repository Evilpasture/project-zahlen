// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/SpirvLayout.hpp
//
// What a module's bytes say its types are: the size a struct occupies, the members of a
// push-constant block, and the frame-address/heap-index words of the descriptor heap's
// push data. Everything here is a constant expression.
//
// The ABI constants themselves live in PushDataLayout.hpp, the half every translation
// unit can afford; this is a reader, included only by the checks that read a module
// (src/render/GpuAbi.hpp) and the offline tools that replay them. It exists so the
// verification half needs no runtime pass over bytecode: `#embed` puts a module's bytes in
// a translation unit and these are the same numbers SPIRV-Reflect answers at pipeline
// creation, read by a reader that never runs.
//
// Two sizes, and the difference is deliberate:
//
//   * `StructSize` is what a host ABI means by a struct's size -- the end of the last
//     member rounded up to its alignment, and so the only number a C++ `sizeof` can be
//     held to;
//   * `StructExtent` is how far the members reach, unpadded (28 where the size of
//     `{ float; float3 }` is 32). It is what SPIRV-Reflect reports as padded_size and as a
//     member's size, and so what tools/zshader writes into the catalog as `PushSize`.
//
// The layout rules are SPIR-V's, not Slang's: offsets from OpMemberDecorate Offset,
// strides from ArrayStride/MatrixStride, a PhysicalStorageBuffer pointer 8 bytes, and
// std140/std430 visible only as the `Name_std140`-style OpName suffix Slang emits. A
// lookup matches the plain name first, then the suffixed copies.
//
// Deliberately narrow, like SpirvBindings.hpp: instruction headers, type opcodes,
// decorations and names -- no function body, no control flow, no descriptor kind. Anything
// it cannot read comes back as `Complete() == false`, which a caller must treat as "this
// proves nothing" rather than "this declares nothing". Standalone on purpose (no Vulkan
// type, no render header, no allocation) so a host tool can run it over real modules.

#pragma once

#include "PushDataLayout.hpp" // the ABI constants this reader is held against

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ZHLN::Vk {

// One member of a struct as the module declares it: the name the shader knows
// it by -- a byte range, because a consteval function cannot build a
// string_view out of the `uint8_t[]` an #embed produces (see SpirvBindings.hpp)
// -- where it sits and how big it is.
struct SpirvLayoutField {
    uint32_t nameOffset = 0;
    uint32_t nameLength = 0;
    uint32_t offset     = 0;
    uint32_t size       = 0;

    // True when this member's name is `name`, byte for byte.
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

// What a module's push-constant block declares: how far its members reach (`extent`, the
// catalog's PushSize) and the members themselves, in declaration order. Bounded at twice
// the widest block the engine ships (skinning.slang's 12 members); a wider block comes
// back `complete == false` rather than quietly truncated.
struct SpirvPushBlock {
    static constexpr uint32_t kCapacity = 32;

    uint32_t                                nameOffset = 0; // the variable's OpName
    uint32_t                                nameLength = 0;
    uint32_t                                extent     = 0;
    uint32_t                                count      = 0;
    std::array<SpirvLayoutField, kCapacity> fields {};
    bool                                    complete   = true;

    [[nodiscard]] constexpr auto Count() const noexcept -> uint32_t {
        return count;
    }
};

// What a lookup found under a name, and how sure it is.
struct SpirvTypeLookup {
    uint32_t size      = 0; // the struct's size, as a host ABI means it
    uint32_t extent    = 0; // how far its members reach, unpadded
    bool     found     = false;
    // Two declarations the same plain name answers to, with different layouts: a reader
    // that guessed between them would guess about the one thing this header exists to be
    // sure of.
    bool     ambiguous = false;
};

// The reader

// The types, decorations and names of one module, read once: construction is the walk and
// every query after it is a lookup in the tables the walk filled, so a caller with many
// questions (the ABI check asks about every type in a list) pays for one pass.
class SpirvTypes {
  public:
    // Word 0 of an instruction is [wordCount:16][opcode:16], little-endian on
    // every target the engine builds for.
    static constexpr uint32_t kSpirvMagic                    = 0x07230203;
    static constexpr size_t   kSpirvHeaderWords              = 5; // magic, version, generator, bound, schema
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
    // StorageClass PushConstant: the one variable a module's push block is.
    static constexpr uint32_t kSpirvStoragePushConstant      = 9;

    // Sized from the widest module the engine ships, with room to spare: it
    // declares 81 types, 227 struct members and 78 names. A module with more
    // than these reports through Complete() rather than truncating.
    static constexpr uint32_t kTypeCapacity        = 256;
    static constexpr uint32_t kMemberCapacity      = 512;
    static constexpr uint32_t kNameCapacity        = 192;
    static constexpr uint32_t kMemberNameCapacity  = 384;
    static constexpr uint32_t kStrideCapacity      = 128;
    static constexpr uint32_t kDecorationCapacity  = 384;
    static constexpr uint32_t kConstantCapacity    = 512;
    static constexpr uint32_t kVariableCapacity    = 32;

    constexpr SpirvTypes() noexcept = default;

    // Walks `module` once: every OpName/OpMemberName, every type declaration, member
    // offsets and strides, the integer constants an array length can be, and the
    // module-scope variables. It stops at the first OpFunction, because SPIR-V's logical
    // layout puts every declaration above the function section.
    [[nodiscard]] static constexpr auto Parse(std::span<const uint8_t> module) noexcept -> SpirvTypes;

    [[nodiscard]] constexpr auto Complete() const noexcept -> bool {
        return !_truncated;
    }
    [[nodiscard]] constexpr auto Bytes() const noexcept -> std::span<const uint8_t> {
        return _bytes;
    }

    // The struct the module declares under `name`, matching a type's OpName exactly, as its
    // last dotted component, or as its prefix before a `_std140`/`_std430`/`_scalar`
    // suffix -- the spellings Slang emits for one type.
    [[nodiscard]] constexpr auto LookupStruct(std::string_view name) const noexcept -> SpirvTypeLookup;

    // The size a host `sizeof` has to equal for the struct `name`, or 0 when the module
    // declares no such struct (LookupStruct::found tells the two apart).
    [[nodiscard]] constexpr auto StructSize(std::string_view name) const noexcept -> uint32_t {
        return LookupStruct(name).size;
    }

    // The module's push-constant block, when it declares one.
    [[nodiscard]] constexpr auto PushBlock() const noexcept -> SpirvPushBlock;

    // The push-data layout the struct `typeName` declares, read with the heap writer's
    // rules: every 8-byte member on an 8-byte boundary, in declaration order, is a frame
    // address, and the first 4-byte word after them is the descriptor index. Nothing when
    // the module declares no such struct or its addresses do not follow each other.
    [[nodiscard]] constexpr auto HeapPushData(std::string_view typeName) const noexcept -> std::optional<HeapPushDataLayout>;

  private:
    enum class Kind : uint8_t { Scalar, Vector, Matrix, Array, RuntimeArray, Struct, Pointer, Opaque };
    // What a decoration says. A member decoration is about one member of one
    // struct; the rest are about a type.
    enum class Decoration : uint8_t { ArrayStride, MatrixStride, MemberOffset, MemberMatrixStride };

    // One declared type. `first`/`count` index the member table for a struct,
    // `first` is the element/pointee/column type for the others, `count` is a
    // component/column count or the id of an array's length constant, and
    // `extra` is a scalar's bit width or a pointer's storage class -- whichever
    // reading the kind calls for.
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

    // --- tables
    // The index of the type `id` names, or an empty optional when the module
    // declares no such type. Deliberately an index and not a pointer: a pointer
    // *into this table* can only say "absent" by comparing itself to nullptr, and
    // that comparison is exactly what GCC refuses to fold when a sanitizer build
    // constant-evaluates it (GCC bug 71962, -fsanitize=null: "(&kTypes._types[1])
    // == 0 is not a constant expression" -- see cmake/Sanitizers.cmake). An index
    // answers "absent" as a bool, so the reader stays evaluable in every build --
    // including the sanitizer CI build, which is where the GpuAbi walk has to
    // hold the ABI to the module. Index access into the table is unaffected.
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

    // --- sizes
    // The alignment of a type, by the rules the runtime reader used: an array,
    // a matrix and a three-or-four-component vector are 16, a two-component
    // vector 8, a struct the widest of its members, a scalar its own width, and
    // a device address 8.
    [[nodiscard]] constexpr auto AlignOf(uint32_t id) const noexcept -> uint32_t;
    // How far a type's members reach, unpadded -- what SPIRV-Reflect reports as
    // a block member's size and a block's padded_size.
    [[nodiscard]] constexpr auto ExtentOf(uint32_t id) const noexcept -> uint32_t;
    // The size a host ABI means by the type: the extent, rounded up to the
    // type's alignment for a struct.
    [[nodiscard]] constexpr auto SizeOf(uint32_t id) const noexcept -> uint32_t {
        const std::optional<uint32_t> typeIndex = TypeIndexAt(id);
        if (!typeIndex || _types[*typeIndex].kind != Kind::Struct) {
            return ExtentOf(id);
        }
        return Vk::AlignUp(ExtentOf(id), AlignOf(id));
    }
    // True when `type`'s OpName is one `name` answers to.
    [[nodiscard]] constexpr auto NameMatches(const Type& type, std::string_view name) const noexcept -> bool;
    // The index of the first struct the module declares under `name` (the
    // declaration order the walk read them in), or an empty optional. An index
    // for the same reason TypeIndexAt returns one.
    [[nodiscard]] constexpr auto FirstStructIndex(std::string_view name) const noexcept -> std::optional<uint32_t>;
};

// The walk

[[nodiscard]] constexpr auto SpirvTypes::Parse(std::span<const uint8_t> module) noexcept -> SpirvTypes {
    SpirvTypes out {};
    out._bytes = module;

    if (module.size() < kSpirvHeaderWords * 4 || (module.size() % 4) != 0) {
        out._truncated = true;
        return out;
    }
    const size_t words = module.size() / 4;

    // Word `index` of the instruction stream, assembled a byte at a time: a
    // consteval function cannot view the bytes as words, because that would be
    // a cast, and a cast is not a constant expression.
    const auto wordAt = [&](size_t index) noexcept -> uint32_t {
        return static_cast<uint32_t>(module[index * 4]) | (static_cast<uint32_t>(module[index * 4 + 1]) << 8) |
               (static_cast<uint32_t>(module[index * 4 + 2]) << 16) | (static_cast<uint32_t>(module[index * 4 + 3]) << 24);
    };
    // The word count in an instruction's first word: how the walk skips it.
    const auto countAt = [&](size_t word) noexcept -> uint32_t {
        return static_cast<uint32_t>(module[word * 4 + 2]) | (static_cast<uint32_t>(module[word * 4 + 3]) << 8);
    };
    // The NUL-terminated string an instruction carries from word `first`, as a
    // byte range. False when it reaches the instruction's end without a
    // terminator, which is not a module this can read.
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
                // A member's name belongs to the member, and the struct it
                // belongs to is read when the member table is folded below.
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
                // Above the first function, so module scope by construction: a
                // variable declared in a body is local, and the walk never sees
                // one.
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
                // Operands: the struct, the member's index, the decoration, its
                // value.
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
                // Module scope ends here: everything above the function section
                // has been read, and nothing below it is a declaration this
                // reader answers for.
                word = words;
                continue;
            default:
                break;
        }
        word += count;
    }

    // --- fold the names and the decorations into the tables
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
            // A matrix member can carry its stride where it is used instead of
            // on the type (OpMemberDecorate MatrixStride): folded onto the
            // member's type, which is how the size below reads a stride.
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
        // The type decoration wins over a member's: it is what the type is
        // declared as everywhere it is used, and the runtime reader read it that
        // way too.
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
            return type->extra / 8; // a width is at least 8 bits, so this is at least 1
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
            return 8; // a VkDeviceAddress
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
    // The exact name wins outright.
    if (size == name.size()) {
        return is(start, name);
    }
    // Slang's layout-specialized copy: `FrameUniforms_std140` for
    // `FrameUniforms`. The character after the prefix has to be the separator,
    // or `Culling` would answer for `CullingConstants`.
    if (is(start, name) && _bytes[start + name.size()] == static_cast<uint8_t>('_')) {
        return true;
    }
    // A type a namespace qualified: `something.Type` answers for `Type`.
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
            // Two declarations the same plain name answers to: two layouts, and
            // no honest way to pick between them from here.
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
            return std::nullopt; // An address run longer than the container.
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

    // The descriptor index is the first 4-byte word after the address run.
    // What occupies the blob in front of the addresses is the schema's
    // arrangement -- the reader takes the prefix as given, because telling a
    // pass payload from any other word is not this module's question.
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

} // namespace ZHLN::Vk
