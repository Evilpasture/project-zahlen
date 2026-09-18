// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/SpirvBindings.hpp
//
// What one compiled module declares in a descriptor set, read at compile time.
//
// A descriptor write names the binding it feeds -- `Vk::Slot<"texInput">(image)`
// (DescriptorWrites.hpp) -- and that name is matched against the module at
// runtime, because a name is the only thing that survives Slang's
// dead-parameter elimination: a binding a configuration drops (`#ifndef
// DISABLE_RTR`) must not move its neighbours' descriptors. The matching is a
// lookup in a vector, so a misspelled name and a binding this module happens not
// to declare are indistinguishable, and both fail as quietly as a descriptor
// slot nothing writes.
//
// `#embed` puts a compiled module's bytes in a translation unit, so the question
// can be answered before anything runs. This header is the reader: it walks the
// instruction stream and collects, per descriptor set, each binding's name
// (OpName), its binding number (OpDecorate Binding / DescriptorSet) and whether
// it is a sampler (its variable's pointee is OpTypeSampler), plus the entry point
// and execution model the module declares (OpEntryPoint). Everything here is a
// constant expression; ShaderProgram.hpp is where it becomes a check, by reading
// the very modules a pass hands to the pipeline.
//
// Deliberately narrow: instruction headers and five opcodes, never a type graph,
// a function body or a control-flow construct. That is everything a binding
// *name* needs. What it gives up is the descriptor kind beyond sampler-or-not --
// a sampled image is not told from a storage image here -- so handing a buffer
// to a binding the shader samples as an image stays the runtime assertion in
// HeapManager::WriteHeapBinding.
//
// This header stands alone: no Vulkan type, no render header. A scratch
// translation unit can include it directly and compare the reader against
// SPIRV-Reflect over real modules -- Parse is constexpr rather than consteval
// for exactly that, while every use in the engine is a constant expression.

#pragma once

#include <Zahlen/Core/Description.hpp> // StringLiteral: a binding name is a template argument

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace ZHLN::Vk {

// ============================================================================
// The five instructions a binding declaration is made of
// ============================================================================
// Word 0 of an instruction is [wordCount:16][opcode:16], little-endian on every
// target the engine builds for.

inline constexpr uint32_t kSpirvMagic             = 0x07230203;
inline constexpr size_t   kSpirvHeaderWords       = 5; // magic, version, generator, bound, schema
inline constexpr uint16_t kSpirvOpEntryPoint    = 15;
inline constexpr uint16_t kSpirvOpName            = 5;
inline constexpr uint16_t kSpirvOpTypeSampler     = 26;
inline constexpr uint16_t kSpirvOpTypePointer     = 32;
inline constexpr uint16_t kSpirvOpFunction        = 54;
inline constexpr uint16_t kSpirvOpVariable        = 59;
inline constexpr uint16_t kSpirvOpDecorate        = 71;
inline constexpr uint16_t kSpirvDecorationBinding = 33;
inline constexpr uint16_t kSpirvDecorationSet     = 34;

// ============================================================================
// One declared binding
// ============================================================================

/// One descriptor binding a module declares.
///
/// The name is a byte range into the module rather than a `std::string_view`
/// because a consteval function cannot form a `const char*` from the `uint8_t[]`
/// `#embed` produces -- that is a reinterpret_cast, and a cast is not a constant
/// expression. So `IsNamed` compares the range against a name the caller knows
/// as a literal, byte for byte.
struct SpirvBinding {
    /// The binding number the module assigned. The heap block orders descriptors
    /// by it, not by declaration order, so it is part of the binding's identity
    /// rather than a detail.
    uint32_t binding    = 0;
    uint32_t nameOffset = 0; ///< byte offset of the name within the module
    uint32_t nameLength = 0;
    /// A sampler binding: its variable's pointee is OpTypeSampler, so its
    /// descriptor lives in the static sampler heap and is written by
    /// InitHeapPassSamplers rather than by WriteHeapParameters.
    bool sampler = false;

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

/// The descriptor bindings one module declares in one descriptor set.
class SpirvBindings {
  public:
    /// Room for the deepest set the engine ships with an order of magnitude to
    /// spare: the lighting pair tops out at 21 bindings in set 0. Bounded
    /// because a constant expression cannot allocate, and reported through
    /// Complete() rather than truncated quietly -- see Parse.
    static constexpr uint32_t kCapacity = 64;

    constexpr SpirvBindings() noexcept = default;

    /// Walks `module` and collects the descriptor bindings it declares in `set`,
    /// in binding order.
    ///
    /// Anything that is not a module this reader can read -- bad magic, a size
    /// that is not a whole number of words, an instruction that runs past the
    /// end, a name that is not terminated, more bindings than a table holds --
    /// comes back with `Complete() == false`. A caller must treat that as "this
    /// proves nothing" and not as "this declares nothing": a short parse passing
    /// a check is the one failure mode that would make the check worse than no
    /// check at all.
    [[nodiscard]] static constexpr auto Parse(std::span<const uint8_t> module, uint32_t set) noexcept -> SpirvBindings;

    [[nodiscard]] constexpr auto Count() const noexcept -> uint32_t {
        return _count;
    }
    [[nodiscard]] constexpr auto operator[](uint32_t index) const noexcept -> const SpirvBinding& {
        return _bindings[index];
    }
    /// The bytes the parse was handed: what a binding's name is a range of.
    [[nodiscard]] constexpr auto Bytes() const noexcept -> std::span<const uint8_t> {
        return _bytes;
    }
    [[nodiscard]] constexpr auto Complete() const noexcept -> bool {
        return !_truncated;
    }

    /// How many entry points the module declares (OpEntryPoint). A module cooked
    /// for one stage of this engine declares exactly one.
    [[nodiscard]] constexpr auto EntryPointCount() const noexcept -> uint32_t {
        return _entryCount;
    }
    /// The execution model of the module's first entry point: which stage it was
    /// compiled for. A raw SPIR-V number rather than a Vulkan enum -- this header
    /// carries no Vulkan type; ShaderProgram.hpp maps it.
    [[nodiscard]] constexpr auto ExecutionModel() const noexcept -> uint32_t {
        return _executionModel;
    }
    /// The entry point's name as a byte range into the module. No `std::string_view`
    /// for the reason names have none: forming one would be a cast, and a cast is
    /// not a constant expression.
    [[nodiscard]] constexpr auto EntryPointOffset() const noexcept -> uint32_t {
        return _entryOffset;
    }
    [[nodiscard]] constexpr auto EntryPointLength() const noexcept -> uint32_t {
        return _entryLength;
    }
    /// True when the module declares exactly one entry point and it is `name`,
    /// byte for byte -- how a declared entry point is held to what was compiled.
    [[nodiscard]] constexpr auto IsEntryPoint(std::string_view name) const noexcept -> bool {
        if (_entryCount != 1 || name.size() != _entryLength || static_cast<size_t>(_entryOffset) + _entryLength > _bytes.size()) {
            return false;
        }
        for (uint32_t i = 0; i < _entryLength; ++i) {
            if (_bytes[_entryOffset + i] != static_cast<uint8_t>(name[i])) {
                return false;
            }
        }
        return true;
    }

    /// True when this set declares a non-sampler binding named `name`.
    [[nodiscard]] constexpr auto DeclaresResource(std::string_view name) const noexcept -> bool {
        for (uint32_t i = 0; i < _count; ++i) {
            if (!_bindings[i].sampler && _bindings[i].IsNamed(_bytes, name)) {
                return true;
            }
        }
        return false;
    }
    /// True when this set declares a sampler binding named `name`.
    [[nodiscard]] constexpr auto DeclaresSampler(std::string_view name) const noexcept -> bool {
        for (uint32_t i = 0; i < _count; ++i) {
            if (_bindings[i].sampler && _bindings[i].IsNamed(_bytes, name)) {
                return true;
            }
        }
        return false;
    }

  private:
    std::array<SpirvBinding, kCapacity> _bindings {};
    std::span<const uint8_t>            _bytes {};
    uint32_t                            _count     = 0;
    bool                                _truncated = false;

    // OpEntryPoint: what the module says it was compiled for.
    uint32_t _entryCount     = 0;
    uint32_t _executionModel = 0;
    uint32_t _entryOffset    = 0;
    uint32_t _entryLength    = 0;
};

// ============================================================================
// The parse
// ============================================================================
// Two walks, each collecting only what it needs. The first jumps instruction to
// instruction and records the ids that decorate with Binding / DescriptorSet: on
// Slang output that is a few dozen ids out of tens of thousands of instructions,
// and everything expensive afterwards is proportional to *that*, not to the
// module. The second walk then looks at only the instructions a bound id can
// appear in -- its OpName (the name), its OpVariable (the type it was declared
// with), the OpTypePointer that type names, and OpTypeSampler (which is what
// makes the pointee a sampler). A module's function bodies are skipped entirely:
// not only for speed, but because a walk that never reads them cannot misread
// them.

[[nodiscard]] constexpr auto SpirvBindings::Parse(std::span<const uint8_t> module, uint32_t set) noexcept -> SpirvBindings {
    /// An id that decorates as a binding, and what the decorations said.
    struct Candidate {
        uint32_t id         = 0;
        uint32_t set        = 0;
        uint32_t binding    = 0;
        bool     hasSet     = false;
        bool     hasBinding = false;
    };
    /// An id -> range pair: a name's bytes in the module.
    struct Range {
        uint32_t id     = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
    };
    /// An id -> id pair: a variable's declared type, a pointer's pointee.
    struct Pair {
        uint32_t id      = 0;
        uint32_t related = 0;
    };

    // Sized from the deepest module the engine ships, with room to spare: one
    // set declares 103 names, 21 bound ids and 46 pointer types. Overflow is
    // reported through Complete(), never truncated quietly.
    constexpr uint32_t kNameCapacity     = 128;
    constexpr uint32_t kTypeCapacity     = 128;
    constexpr uint32_t kVariableCapacity = 64;
    constexpr uint32_t kSamplerCapacity  = 16;

    SpirvBindings out {};
    out._bytes = module;

    if (module.size() < kSpirvHeaderWords * 4 || (module.size() % 4) != 0) {
        out._truncated = true;
        return out;
    }
    const size_t words = module.size() / 4;

    /// Word `index` of the instruction stream, assembled a byte at a time: a
    /// consteval function cannot view the bytes as words, because that would be
    /// a cast, and a cast is not a constant expression.
    const auto wordAt = [&](size_t index) noexcept -> uint32_t {
        return static_cast<uint32_t>(module[index * 4]) | (static_cast<uint32_t>(module[index * 4 + 1]) << 8) |
               (static_cast<uint32_t>(module[index * 4 + 2]) << 16) | (static_cast<uint32_t>(module[index * 4 + 3]) << 24);
    };
    /// The word count in an instruction's first word: how the walk skips it.
    const auto countAt = [&](size_t word) noexcept -> uint32_t {
        return static_cast<uint32_t>(module[word * 4 + 2]) | (static_cast<uint32_t>(module[word * 4 + 3]) << 8);
    };

    if (wordAt(0) != kSpirvMagic) {
        out._truncated = true;
        return out;
    }

    // --- Walk 1: the ids that carry a binding ------------------------------
    std::array<Candidate, kCapacity> candidates {};
    uint32_t                         candidateCount = 0;

    for (size_t word = kSpirvHeaderWords; word < words;) {
        const uint32_t count = countAt(word);
        if (count == 0 || word + count > words) {
            out._truncated = true;
            return out;
        }
        // The declarations are all above the first function definition (SPIR-V's
        // logical layout: capabilities, entry points, debug, annotations, types,
        // globals, then functions), so the walk stops there rather than reading
        // code it has no use for -- a module with a large table in a function
        // body would otherwise cost steps proportional to that body.
        if (module[word * 4 + 1] == 0 && module[word * 4] == static_cast<uint8_t>(kSpirvOpFunction)) {
            break;
        }
        // OpEntryPoint (15) is above OpDecorate in the logical layout, and it is
        // the only place a module states what stage it was compiled for and what
        // the pipeline may call it as. Recorded here because ShaderProgram.hpp
        // hands the stage and the entry point to the pipeline from the module
        // itself rather than from fields declared beside it.
        if (module[word * 4 + 1] == 0 && module[word * 4] == static_cast<uint8_t>(kSpirvOpEntryPoint) && count >= 4) {
            ++out._entryCount;
            if (out._entryCount == 1) {
                out._executionModel = wordAt(word + 1);
                // The name is a null-terminated literal from word 3 on.
                const size_t nameStart = (word + 3) * 4;
                const size_t limit     = (word + count) * 4;
                size_t       nameEnd   = nameStart;
                while (nameEnd < limit && module[nameEnd] != 0) {
                    ++nameEnd;
                }
                if (nameEnd == limit) {
                    out._truncated = true;
                    return out;
                }
                out._entryOffset = static_cast<uint32_t>(nameStart);
                out._entryLength = static_cast<uint32_t>(nameEnd - nameStart);
            }
        }
        // OpDecorate is the only other instruction this walk cares about, and 71
        // fits in one byte, so the low byte decides and the high byte only has to
        // be zero: assembling the whole opcode of every instruction in a
        // 45,000-word module is a measurable share of the parse, and the case
        // falls through for free.
        if (module[word * 4 + 1] == 0 && module[word * 4] == static_cast<uint8_t>(kSpirvOpDecorate) && count >= 4) {
            const uint32_t decoration = wordAt(word + 2);
            if (decoration == kSpirvDecorationBinding || decoration == kSpirvDecorationSet) {
                const uint32_t target = wordAt(word + 1);
                int64_t        known  = -1;
                for (uint32_t i = 0; i < candidateCount; ++i) {
                    if (candidates[i].id == target) {
                        known = static_cast<int64_t>(i);
                    }
                }
                // A decoration for an id this walk has not seen yet: worth a row
                // only because the row may still turn out to carry a Binding --
                // DescriptorSet arrives as an instruction of its own.
                if (known < 0) {
                    if (candidateCount == kCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    known                 = static_cast<int64_t>(candidateCount);
                    candidates[candidateCount].id = target;
                    ++candidateCount;
                }
                Candidate& entry = candidates[static_cast<size_t>(known)];
                if (decoration == kSpirvDecorationBinding) {
                    entry.binding    = wordAt(word + 3);
                    entry.hasBinding = true;
                } else {
                    entry.set    = wordAt(word + 3);
                    entry.hasSet = true;
                }
            }
        }
        word += count;
    }

    // --- Walk 2: names, variables, pointer types, samplers -----------------
    std::array<Range, kNameCapacity>       names {};
    std::array<Pair, kVariableCapacity>    variables {};
    std::array<Pair, kTypeCapacity>        pointers {};
    std::array<uint32_t, kSamplerCapacity> samplerTypes {};

    uint32_t nameCount     = 0;
    uint32_t variableCount = 0;
    uint32_t pointerCount  = 0;
    uint32_t samplerCount  = 0;

    const auto isCandidate = [&](uint32_t id) noexcept -> bool {
        for (uint32_t i = 0; i < candidateCount; ++i) {
            if (candidates[i].id == id) {
                return true;
            }
        }
        return false;
    };

    for (size_t word = kSpirvHeaderWords; word < words;) {
        const uint32_t count  = countAt(word);
        const uint32_t opcode = static_cast<uint32_t>(module[word * 4]) | (static_cast<uint32_t>(module[word * 4 + 1]) << 8);
        if (count == 0 || word + count > words) {
            out._truncated = true;
            return out;
        }
        switch (opcode) {
            case kSpirvOpName:
                if (count >= 3 && isCandidate(wordAt(word + 1))) {
                    if (nameCount == kNameCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    // The name is the instruction's remaining words, NUL
                    // terminated. Its length comes from the bytes rather than
                    // from the word count, so padding after the terminator is
                    // not part of it.
                    const uint32_t offset = static_cast<uint32_t>((word + 2) * 4);
                    const uint32_t span   = (count - 2) * 4;
                    uint32_t       length = 0;
                    while (length < span && module[offset + length] != 0) {
                        ++length;
                    }
                    if (length == span) {
                        out._truncated = true; // No terminator: not a module this can read.
                        return out;
                    }
                    names[nameCount++] = Range {.id = wordAt(word + 1), .offset = offset, .length = length};
                }
                break;
            case kSpirvOpVariable:
                // Only a module-scope variable can carry a binding, and this is
                // the section above the first function: a variable declared in a
                // body is local by construction (the deepest module declares
                // 1,496 of them, and skipping them is most of the walk).
                if (count >= 4 && isCandidate(wordAt(word + 2))) {
                    if (variableCount == kVariableCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    variables[variableCount++] = Pair {.id = wordAt(word + 2), .related = wordAt(word + 1)};
                }
                break;
            case kSpirvOpTypePointer:
                if (count >= 4) {
                    if (pointerCount == kTypeCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    pointers[pointerCount++] = Pair {.id = wordAt(word + 1), .related = wordAt(word + 3)};
                }
                break;
            case kSpirvOpFunction:
                // Module scope ends here: see the layout note in the first walk.
                word = words;
                break;
            case kSpirvOpTypeSampler:
                if (count >= 2) {
                    if (samplerCount == kSamplerCapacity) {
                        out._truncated = true;
                        return out;
                    }
                    samplerTypes[samplerCount++] = wordAt(word + 1);
                }
                break;
            default:
                break;
        }
        word += count;
    }

    // --- Resolve ----------------------------------------------------------
    for (uint32_t i = 0; i < candidateCount; ++i) {
        const Candidate& candidate = candidates[i];
        // No DescriptorSet decoration means set 0, which is what the spec says
        // and what the runtime reflection reports.
        const uint32_t candidateSet = candidate.hasSet ? candidate.set : 0;
        if (!candidate.hasBinding || candidateSet != set) {
            continue;
        }

        SpirvBinding binding {.binding = candidate.binding};
        for (uint32_t n = 0; n < nameCount; ++n) {
            if (names[n].id == candidate.id) {
                binding.nameOffset = names[n].offset;
                binding.nameLength = names[n].length;
            }
        }
        if (binding.nameLength == 0) {
            out._truncated = true; // A bound variable with no OpName cannot be matched by name.
            return out;
        }

        uint32_t pointee = 0;
        for (uint32_t v = 0; v < variableCount; ++v) {
            if (variables[v].id != candidate.id) {
                continue;
            }
            for (uint32_t p = 0; p < pointerCount; ++p) {
                if (pointers[p].id == variables[v].related) {
                    pointee = pointers[p].related;
                }
            }
        }
        for (uint32_t s = 0; s < samplerCount; ++s) {
            binding.sampler = binding.sampler || samplerTypes[s] == pointee;
        }

        if (out._count == kCapacity) {
            out._truncated = true;
            return out;
        }
        out._bindings[out._count++] = binding;
    }

    // Binding order, which is the order the heap block addresses them in.
    for (uint32_t i = 1; i < out._count; ++i) {
        const SpirvBinding current = out._bindings[i];
        uint32_t          j         = i;
        while (j > 0 && out._bindings[j - 1].binding > current.binding) {
            out._bindings[j] = out._bindings[j - 1];
            --j;
        }
        out._bindings[j] = current;
    }

    return out;
}

} // namespace ZHLN::Vk
