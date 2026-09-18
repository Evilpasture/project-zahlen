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
// `#embed` puts a compiled module's bytes in a translation unit (Resources.cpp,
// ShaderBindingChecks.cpp), so the question can be answered before anything
// runs. This header is the reader: it walks the instruction stream and collects,
// per descriptor set, each binding's name (OpName), its binding number
// (OpDecorate Binding / DescriptorSet) and whether it is a sampler (its
// variable's pointee is OpTypeSampler). Everything here is a constant
// expression, so a name no module of a pass declares -- a typo -- and a binding
// of a pass that nothing writes are both compile errors, per configuration, with
// no GPU involved.
//
// Deliberately narrow: instruction headers and five opcodes, never a type graph,
// a function body or a control-flow construct. That is everything a binding
// *name* needs. What it gives up is the descriptor kind beyond sampler-or-not --
// a sampled image is not told from a storage image here -- so handing a buffer
// to a binding the shader samples as an image stays the runtime assertion in
// HeapManager::WriteHeapBinding.
//
// This header stands alone: no Vulkan type, no render header. ShaderBindingChecks.cpp
// includes it directly, and so can a scratch translation unit comparing this
// reader against SPIRV-Reflect over real modules -- Parse is constexpr rather
// than consteval for exactly that, while every use in the engine is a constant
// expression.

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

// ============================================================================
// What a pass declares
// ============================================================================
// The reader above answers what a *module* declares. A pass is not a module: it
// is one or more configurations of one or more shaders (lighting.slang compiles
// RT and NoRT, SMAA.slang EDGE/WEIGHT/BLEND), and Slang drops the parameters a
// configuration does not reference -- the NoRT lighting module has no
// blueNoiseTex or tlas, yet keeps texEmissive at binding 18, because the module
// preserves the gaps. One call site therefore names the union of what its
// configurations declare, and that union is what a pass has to state.

/// One declared binding name, as a type.
///
/// Types and not values: every check below instantiates something per name, and
/// only a type reaches a diagnostic -- `BindingName<"texInpuut">` printed by the
/// compiler says which binding failed, where a `bool` says only that one did.
template <ZHLN::StringLiteral Name>
struct BindingName {
    static constexpr std::string_view value = Name;
    /// The name as the literal it was declared as: what a diagnostic takes as a
    /// template argument.
    static constexpr auto literal = Name;

    [[nodiscard]] static constexpr auto Contains(std::string_view want) noexcept -> bool {
        return want == value;
    }
};

/// Binding names, in the order the shader declares them.
template <ZHLN::StringLiteral... Names>
struct BindingNames {
    static constexpr uint32_t count = sizeof...(Names);

    static constexpr std::array<std::string_view, sizeof...(Names)> names {std::string_view(Names)...};
    /// The names as literal-backed types, so a check can instantiate one proof
    /// per name and have the compiler print the one that failed.
    using Entries = std::tuple<BindingName<Names>...>;

    [[nodiscard]] static constexpr auto Contains(std::string_view want) noexcept -> bool {
        for (const std::string_view name: names) {
            if (name == want) {
                return true;
            }
        }
        return false;
    }
};

/// A pass's descriptor interface: the names its writes have to spell.
///
///   * `Resources` -- non-sampler bindings (images, buffers, acceleration
///     structures), written by WriteHeapParameters into a transient heap block;
///   * `Samplers` -- sampler bindings, written once by InitHeapPassSamplers into
///     static sampler-heap slots;
///   * `DroppedResources` / `DroppedSamplers` -- names the shader *source*
///     declares and no compiled module does, because nothing active references
///     them (blit.slang's texDepth and frame are sampled nowhere;
///     hiz_generate.slang's pointSampler is sampled with nowhere). A write naming
///     one is correct and skipped at runtime, so listing it keeps that from
///     being an error. Kind-separated on purpose: a dropped sampler spelled as a
///     resource write would be skipped too, and then only the block-slot count
///     assertion -- at runtime -- would notice.
///
/// The four lists are the pass's whole descriptor interface, and
/// ShaderBindingChecks.cpp proves them against the compiled modules: every
/// declared name exists in some module with the matching kind, and every name
/// recorded as dropped exists in none. A dropped name's *spelling* is the one
/// thing nothing can confirm -- the module has no such binding, by definition --
/// and a misspelling there costs a skipped write, nothing more.
/// Which half of a descriptor block a name belongs to, and therefore which write
/// path fills it. The two are checked, dropped and written apart: a sampler is a
/// static sampler-heap slot filled once (InitHeapPassSamplers), everything else a
/// transient resource-heap slot filled per write (WriteHeapParameters).
enum class BindingKind : uint8_t {
    Resource,
    Sampler,
};

/// A descriptor block's declaration: the names its writes have to spell.
template <typename T>
concept DeclaredBindings = requires {
    T::Resources::count;
    T::Samplers::count;
    T::DroppedResources::count;
    T::DroppedSamplers::count;
};

// ============================================================================
// The checks
// ============================================================================
// Two directions, at two places of the build:
//
//   * At the write, against the declaration: every name the write spells is a
//     name the pass declares (the typo), every name the pass declares is spelled
//     (so a binding the shader gained cannot go unwritten), and no name is
//     spelled twice.
//
//   * In the translation unit holding the module bytes, against the compiled
//     modules: the declaration and the modules declare the same bindings, in
//     both directions and per configuration. That is what lets the first
//     direction trust a hand-written list.
//
// The gates below are what a write helper calls in a static_assert, so the check
// *is* the write: there is no case table to keep up to date and no way to write
// a descriptor block without one.

namespace TemplatedDetail {

/// The names `Declared` lists for `Kind`: what a write of that kind has to name.
template <typename Declared, BindingKind Kind>
using KindList = std::conditional_t<Kind == BindingKind::Sampler, typename Declared::Samplers, typename Declared::Resources>;

/// The names `Declared` drops for `Kind`: what a write of that kind may name
/// although no compiled module declares it (see DeclaredBindings).
template <typename Declared, BindingKind Kind>
using DroppedList = std::conditional_t<Kind == BindingKind::Sampler, typename Declared::DroppedSamplers, typename Declared::DroppedResources>;

/// The names `Declared` lists for `Kind` as literal-backed types.
template <typename Declared, BindingKind Kind>
using KindEntries = typename KindList<Declared, Kind>::Entries;

/// A name a write spells that the declaration does not have. Declared and never
/// defined on purpose: reaching into it is how the gate below reports, and the
/// instantiation carries the binding's name into the compiler's words --
///
///     error: implicit instantiation of undefined template
///       'ZHLN::Vk::TemplatedDetail::UndeclaredBinding<Bindings::Lighting,
///        ZHLN::StringLiteral<11>{"texInpuut"}>'
///
/// -- so a misspelling is named, not left to be found.
template <typename Declared, BindingKind Kind, ZHLN::StringLiteral Name>
struct UndeclaredBinding;

/// A name the declaration has that the write does not spell. Missing rather than
/// misspelled, and reported the same way -- the instantiation names it.
template <typename Declared, BindingKind Kind, ZHLN::StringLiteral Name>
struct UnspelledBinding;

/// One argument's worth of "is this name one of ours": complete exactly when it
/// is, which is also when nothing is diagnosed.
template <typename Declared, BindingKind Kind, typename Named>
consteval void RequireDeclared() {
    if constexpr (KindList<Declared, Kind>::Contains(Named::name) || DroppedList<Declared, Kind>::Contains(Named::name)) {
        return;
    } else {
        static_cast<void>(sizeof(UndeclaredBinding<Declared, Kind, Named::literal>));
    }
}

/// A listed name and the arguments that ought to spell it: complete exactly when
/// one of them does.
template <typename Declared, BindingKind Kind, ZHLN::StringLiteral Name, typename... Named>
    requires((false || ... || (std::string_view(Name) == Named::name)))
struct SpelledByOne {};

/// Every name `Declared` lists for `Kind` is spelled by `Named...`: one
/// instantiation per listed name, so the argument that is missing is the one the
/// compiler names.
template <typename Declared, BindingKind Kind, typename... Named, size_t... Index>
consteval void RequireSpelled(const std::index_sequence<Index...>&) {
    (static_cast<void>(sizeof(SpelledByOne<Declared, Kind, std::tuple_element_t<Index, KindEntries<Declared, Kind>>::literal, Named...>)), ...);
}

} // namespace TemplatedDetail

/// True when every name `Named...` spells is one `Declared` lists for `Kind`, or
/// records as dropped for it. A name that is neither is a typo -- or a binding
/// this configuration dropped and nobody wrote down, which is the same mistake
/// with a different cause -- and the compiler says which name it was.
template <typename Declared, BindingKind Kind, typename... Named>
[[nodiscard]] consteval auto NamesAreDeclared() noexcept -> bool {
    static_assert(DeclaredBindings<Declared>, "a descriptor block lists Resources, Samplers and the dropped names of both");
    (TemplatedDetail::RequireDeclared<Declared, Kind, Named>(), ...);
    return true;
}

/// True when every name `Declared` lists for `Kind` is spelled by `Named...`: the
/// direction that catches a forgotten argument. A transient block has no
/// previous frame's descriptor to fall back on, so a binding nothing writes is a
/// descriptor the shader reads from an older frame.
template <typename Declared, BindingKind Kind, typename... Named>
[[nodiscard]] consteval auto NamesCoverDeclarations() noexcept -> bool {
    static_assert(DeclaredBindings<Declared>, "a descriptor block lists Resources, Samplers and the dropped names of both");
    if constexpr (TemplatedDetail::KindList<Declared, Kind>::count == 0) {
        return true;
    } else {
        TemplatedDetail::RequireSpelled<Declared, Kind, Named...>(std::make_index_sequence<TemplatedDetail::KindList<Declared, Kind>::count> {});
        return true;
    }
}

/// True when no two arguments name the same binding: a repeated name is a second
/// write over the first, and only the last one survives.
template <typename... Named>
[[nodiscard]] consteval auto NamesAreDistinct() noexcept -> bool {
    constexpr std::array<std::string_view, sizeof...(Named)> spelled {Named::name...};
    for (size_t i = 0; i < spelled.size(); ++i) {
        for (size_t j = i + 1; j < spelled.size(); ++j) {
            if (spelled[i] == spelled[j]) {
                return false;
            }
        }
    }
    return true;
}

// ============================================================================
// The module gate
// ============================================================================
// The mirror of the write gates, and the reason a hand-written declaration can
// be trusted: a declaration is only worth checking a write against while it is
// still what the module says.

/// Every binding every module declares is one of the names `Declared` lists, of
/// the matching kind. Per module and not per union: a binding one configuration
/// declares is not excused by a name another configuration happens to have.
template <typename Declared>
[[nodiscard]] consteval auto EveryModuleBindingIsDeclared(std::span<const SpirvBindings> modules) noexcept -> bool {
    static_assert(DeclaredBindings<Declared>, "a pass declaration lists Resources, Samplers and the dropped names of both");

    for (const SpirvBindings& parsed: modules) {
        const std::span<const uint8_t> bytes = parsed.Bytes();
        for (uint32_t b = 0; b < parsed.Count(); ++b) {
            const SpirvBinding& binding = parsed[b];
            // The two lists have different lengths, so they cannot share one
            // variable: which applies is the binding's kind.
            bool found = false;
            if (binding.sampler) {
                for (const std::string_view name: Declared::Samplers::names) {
                    found = found || binding.IsNamed(bytes, name);
                }
            } else {
                for (const std::string_view name: Declared::Resources::names) {
                    found = found || binding.IsNamed(bytes, name);
                }
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}

/// Every name `Declared` lists is declared by at least one module. A name left
/// behind after the shader stopped declaring it would make the write gates
/// demand a descriptor for a binding that no longer exists.
template <typename Declared>
[[nodiscard]] consteval auto EveryDeclaredNameIsInSomeModule(std::span<const SpirvBindings> modules) noexcept -> bool {
    static_assert(DeclaredBindings<Declared>, "a pass declaration lists Resources, Samplers and the dropped names of both");

    const auto declaredBySomeModule = [&](std::string_view name, BindingKind kind) consteval {
        for (const SpirvBindings& parsed: modules) {
            const std::span<const uint8_t> bytes = parsed.Bytes();
            for (uint32_t b = 0; b < parsed.Count(); ++b) {
                const SpirvBinding& binding = parsed[b];
                if (binding.sampler == (kind == BindingKind::Sampler) && binding.IsNamed(bytes, name)) {
                    return true;
                }
            }
        }
        return false;
    };

    for (const std::string_view name: Declared::Resources::names) {
        if (!declaredBySomeModule(name, BindingKind::Resource)) {
            return false;
        }
    }
    for (const std::string_view name: Declared::Samplers::names) {
        if (!declaredBySomeModule(name, BindingKind::Sampler)) {
            return false;
        }
    }
    return true;
}

/// No name `Declared` records as dropped is declared by any module: the lists
/// are for bindings the shader source declares and the compiler strips, and a
/// name that reaches a module belongs in Resources or Samplers instead.
template <typename Declared>
[[nodiscard]] consteval auto NoDroppedNameIsInAModule(std::span<const SpirvBindings> modules) noexcept -> bool {
    static_assert(DeclaredBindings<Declared>, "a pass declaration lists Resources, Samplers and the dropped names of both");

    const auto inSomeModule = [&](std::string_view name) consteval {
        for (const SpirvBindings& parsed: modules) {
            if (parsed.DeclaresResource(name) || parsed.DeclaresSampler(name)) {
                return true;
            }
        }
        return false;
    };

    for (const std::string_view name: Declared::DroppedResources::names) {
        if (inSomeModule(name)) {
            return false;
        }
    }
    for (const std::string_view name: Declared::DroppedSamplers::names) {
        if (inSomeModule(name)) {
            return false;
        }
    }
    return true;
}

/// Every module is readable and agrees with `Declared` in both directions: the
/// conjunction of the three checks above, for a translation unit that wants one
/// line per pass.
template <typename Declared>
[[nodiscard]] consteval auto ModulesMatchDeclarations(std::span<const SpirvBindings> modules) noexcept -> bool {
    if (modules.empty()) {
        return false;
    }
    for (const SpirvBindings& parsed: modules) {
        if (!parsed.Complete()) {
            return false; // An inconclusive parse must not pass a consistency check.
        }
    }
    return EveryModuleBindingIsDeclared<Declared>(modules) && EveryDeclaredNameIsInSomeModule<Declared>(modules) &&
           NoDroppedNameIsInAModule<Declared>(modules);
}

} // namespace ZHLN::Vk
