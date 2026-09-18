// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/vulkan/pipeline/ShaderProgram.hpp
//
// What a shader module is, and the checks a descriptor write runs against it.
//
// A module is a type that states two things: its cooked bytes and the source a
// hot reload would reread. Everything else about it -- the entry point a
// pipeline calls it as, the stage it was compiled for, every binding it
// declares with its set, its binding number and its descriptor type -- is
// generated from those bytes by `tools/zshader` (the build runs it over the
// cooked SPIR-V, using SPIRV-Reflect) into `ShaderBindings.hpp`, next to the one
// translation unit that still `#embed`s them.
//
// The generated lists are not taken on trust. The same translation unit that
// holds a module's bytes asserts them against those bytes with
// `ModuleMatchesBytes`, which walks the module with the independent reader in
// SpirvBindings.hpp -- so a generator that reflects a module wrongly, or a
// module that changed under a stale generated header, is a build failure and not
// a descriptor written to the wrong place. Two readers, one of them the
// compiler's, have to agree:
//
//   * `Vk::ShaderSet<...>` names the programs one descriptor block serves. A
//     write hands the set to `HeapManager::WriteHeapParameters` /
//     `InitHeapPassSamplers`, and every slot it spells is checked against the
//     generated binding lists: a name no module of the set declares is a compile
//     error that prints the name, and a binding a module declares that the write
//     forgets is a compile error that prints the module and the binding number;
//
//   * the same program type is what the pipeline is built from --
//     `Vk::CreateShaderDesc<Program>()` hands the module's bytes and its own
//     entry point to the stage, so the module the checks ran against is the
//     module that gets loaded.
//
// There is no table to keep in step with the shaders and no parser in the way of
// a build: the lists are data in a header, the walk happens once per module in
// the one translation unit that has the bytes, and a write site costs a
// membership test over a type list.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "SpirvBindings.hpp" // the independent reader the generated catalog is verified with

#include <Zahlen/Core/Description.hpp>             // StringLiteral: a binding name is a template argument
#include <Zahlen/Core/Reflection/Structs.hpp> // Reflect::ForEachFieldInfo: the payload side of a push block

#include <algorithm> // std::max, in the cover merge
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace ZHLN::Vk {

/// One descriptor binding a module declares, as `tools/zshader` read it out of
/// the module: the name the shader knows it by, the Vulkan descriptor type it
/// expects, and where the module put it.
///
/// A type rather than a string, so the write side can hold itself against it at
/// compile time: the name is a template argument (the compiler can print it), the
/// descriptor type is a constant (a payload of the wrong shape can be rejected),
/// and set/binding are what the module said rather than what a call site assumes.
namespace Declared {

template <ZHLN::StringLiteral Name, VkDescriptorType Type, uint32_t Set, uint32_t Binding>
struct ResourceSlot {
    static constexpr std::string_view name    = Name;
    static constexpr VkDescriptorType type    = Type;
    static constexpr uint32_t         set     = Set;
    static constexpr uint32_t         binding = Binding;
};

template <ZHLN::StringLiteral Name, uint32_t Set, uint32_t Binding>
struct SamplerSlot {
    static constexpr std::string_view name    = Name;
    static constexpr uint32_t         set     = Set;
    static constexpr uint32_t         binding = Binding;
};

} // namespace Declared

/// The two halves of a module's declarations, as types: the resource heap holds
/// everything that is not a sampler, the static sampler heap holds the samplers,
/// and a gate takes the half it writes -- `WriteHeapParameters` the resources,
/// `InitHeapPassSamplers` the samplers.
///
/// `Half::Of<Program>` is the list of declarations that half is about, so the
/// half travels as a type through every check below: nothing enumerates it, and
/// a diagnostic that carries it says which half a write got wrong.
struct ResourceBindings {
    template <typename Program>
    using Of = typename Program::Resources;
};

struct SamplerBindings {
    template <typename Program>
    using Of = typename Program::Samplers;
};

/// One member of a module's push-constant block, as the module's own
/// OpMemberDecorate says it: where it sits, how big it is, and the name the
/// shader knows it by. The engine's push structs are still written by hand (they
/// carry VkDeviceAddress and engine math types SPIR-V has no name for), so this
/// is what holds one against the other instead of a second struct that would
/// drift from it.
struct PushMember {
    const char* name   = nullptr;
    uint32_t    offset = 0;
    uint32_t    size   = 0;
};

/// The bindings one module declares, in the order the tool reflected them.
template <typename... Slots>
struct BindingList {
    static constexpr size_t count = sizeof...(Slots);

    /// True when one of the slots is named `name`.
    [[nodiscard]] static constexpr auto Declares(std::string_view name) noexcept -> bool {
        return ((Slots::name == name) || ...);
    }

    /// True when one of the slots names the binding `candidate` holds in the
    /// module's bytes -- the same name in the same set, since a module may
    /// declare bindings in more than one set and a name in set 1 is not the
    /// binding a list says sits in set 0. The direction a stale or wrong
    /// generated list trips.
    [[nodiscard]] static constexpr auto Spells(const SpirvBindings& declarations, const SpirvBinding& candidate, uint32_t set) noexcept -> bool {
        return ((Slots::set == set && candidate.IsNamed(declarations.Bytes(), Slots::name)) || ...);
    }
};

/// The slot types of a `BindingList`, as a tuple, for indexed access.
template <typename List>
struct SlotsOf;
template <typename... Slots>
struct SlotsOf<BindingList<Slots...>> {
    using tuple = std::tuple<Slots...>;
};
template <typename List>
using SlotsOfT = typename SlotsOf<List>::tuple;

/// A module's declaration list for one half: what a write of that half is
/// checked against.
template <typename Half, typename Program>
using DeclaredList = typename Half::template Of<Program>;

/// One cooked shader module, known at compile time.
///
/// `Bytes()` is defined once, by the generated ShaderBytecode.cpp, next to the
/// `#embed`ded array it returns -- a translation unit can call it but cannot use
/// it in a constant expression, which is why the verification of the generated
/// lists happens there and the checks here work on the lists.
template <typename T>
concept ShaderProgram = requires {
    typename T::Resources;
    typename T::Samplers;
    { T::EntryPoint } -> std::convertible_to<std::string_view>;
    { T::Stage } -> std::convertible_to<VkShaderStageFlagBits>;
    { T::Path } -> std::convertible_to<const char*>;
    { T::Bytes() } -> std::same_as<std::span<const uint8_t>>;
};

// ============================================================================
// The checks
// ============================================================================

namespace TemplatedDetail {

/// A name a write spells that no module in the set declares. Declared and never
/// defined on purpose: reaching into it is how the check reports, and the
/// instantiation carries the name into the compiler's words --
///
///     error: implicit instantiation of undefined template
///       'ZHLN::Vk::TemplatedDetail::UndeclaredBinding<Shaders::Lighting,
///        ZHLN::Vk::ResourceBindings, ZHLN::StringLiteral<10>{"texInpuut"}>'
///
/// -- so a misspelling is named rather than left to be found by looking at the
/// image.
template <typename Set, typename Half, ZHLN::StringLiteral Name>
struct UndeclaredBinding;

/// A binding a module declares that the write does not spell: the forgotten
/// argument. The diagnostic names the module that declares it and the binding
/// number that module gave it -- a transient block has no previous frame's
/// descriptor to fall back on, so the shader would read a stale one.
template <typename Set, typename Half, typename Program, uint32_t Binding>
struct UnspelledBinding;

/// Complete exactly when `Spelled` -- the `false` specialization deliberately
/// does not exist, so reaching it is the diagnostic.
template <typename Set, typename Half, typename Program, uint32_t Binding, bool Spelled>
struct DeclarationSpelledBy;
template <typename Set, typename Half, typename Program, uint32_t Binding>
struct DeclarationSpelledBy<Set, Half, Program, Binding, true> {};

/// True when this slot was written through `Unread`: the pass holds the binding
/// and the module it serves does not read it (Slang strips a parameter the
/// configuration never references). A write is free to name those -- the runtime
/// skips them exactly as it skips a binding another configuration dropped -- and
/// the check must not read the deliberate ones as misspellings.
template <typename Slot>
[[nodiscard]] consteval auto IsUnreadSlot() noexcept -> bool {
    if constexpr (requires { Slot::unread; }) {
        return Slot::unread;
    } else {
        return false;
    }
}

/// True when one module declares `Slot` -- or when the write marked the slot
/// `Unread`, which is a statement about a binding the module does not declare.
template <typename Half, typename Program, typename Slot>
[[nodiscard]] consteval auto SlotIsDeclared() noexcept -> bool {
    if constexpr (IsUnreadSlot<Slot>()) {
        return true;
    } else {
        return DeclaredList<Half, Program>::Declares(Slot::name);
    }
}

/// Whatever a write's slots are, this bitmask says which of them the module
/// declares: one membership test per slot per module, no walking of bytes.
template <typename Half, typename Program, typename... Slots>
[[nodiscard]] consteval auto DeclaredSlotMask() noexcept -> uint64_t {
    uint64_t mask  = 0;
    uint32_t index = 0;
    ((mask |= SlotIsDeclared<Half, Program, Slots>() ? (uint64_t {1} << index) : uint64_t {0}, ++index), ...);
    return mask;
}

/// Complete for a slot whose bit is set; the diagnostic for the one whose is not.
template <typename Set, typename Half, uint64_t Mask, typename... Slots, size_t... Index>
consteval void RequireDeclaredBits(std::index_sequence<Index...>) {
    (static_cast<void>(sizeof(std::conditional_t<
                           ((Mask >> Index) & 1u) != 0,
                           std::true_type,
                           UndeclaredBinding<Set, Half, std::tuple_element_t<Index, std::tuple<Slots...>>::literal>
                       >)), ...);
}

/// True when one of the write's slots names this declared binding.
template <typename DeclaredSlot, typename... Slots>
[[nodiscard]] consteval auto SpellsDeclaredSlot() noexcept -> bool {
    return ((Slots::name == DeclaredSlot::name) || ...) || (IsUnreadSlot<Slots>() || ...);
}

/// One instantiation per binding the module declares: complete when the write
/// spells it.
template <typename Set, typename Half, typename Program, typename... Slots, size_t... Index>
consteval void RequireSpelledBindings(std::index_sequence<Index...>) {
    using List = DeclaredList<Half, Program>;
    (static_cast<void>(sizeof(DeclarationSpelledBy<
                           Set,
                           Half,
                           Program,
                           std::tuple_element_t<Index, SlotsOfT<List>>::binding,
                           SpellsDeclaredSlot<std::tuple_element_t<Index, SlotsOfT<List>>, Slots...>()
                       >)), ...);
}

/// One program's half of the cover check: true when it declares no binding of
/// `Half` that the write's slots leave unspoken.
template <typename Set, typename Half, ShaderProgram Program, typename... Slots>
[[nodiscard]] consteval auto SpellsEveryDeclaration() -> bool {
    constexpr size_t declared = DeclaredList<Half, Program>::count;
    RequireSpelledBindings<Set, Half, Program, Slots...>(std::make_index_sequence<declared> {});
    return true;
}

// ----------------------------------------------------------------------------
// The checks that need the declaration itself
// ----------------------------------------------------------------------------
//
// Matching names is one thing; the value under a name is another. A visit to the
// declared types is what the descriptor-type check needs -- the declared type is
// a constant of the declaration, not of the write -- so the two sides are kept
// apart: this header walks its own lists and calls `Check::Holds<DeclaredSlot,
// WriteSlot>()` for the declaration each write slot names, and the header that
// knows what shape of value a write can carry (HeapBindings.hpp) states the
// check. Neither has to know the other's vocabulary.

/// One declaration held to `Check`, when the write slot names it. A declared
/// binding the write leaves unspoken is the cover check's business, not this
/// one's, and stays out of the fold as true.
template <typename Check, typename DeclaredSlot, typename WriteSlot>
[[nodiscard]] consteval auto DeclaredSlotHoldsCheck() noexcept -> bool {
    if constexpr (DeclaredSlot::name == WriteSlot::name) {
        return Check::template Holds<DeclaredSlot, WriteSlot>();
    } else {
        return true;
    }
}

template <typename Check, typename List, typename WriteSlot, size_t... Index>
[[nodiscard]] consteval auto CheckDeclaredSlotsAt(std::index_sequence<Index...>) noexcept -> bool {
    return (DeclaredSlotHoldsCheck<Check, std::tuple_element_t<Index, SlotsOfT<List>>, WriteSlot>() && ...);
}

/// One module's declaration of `Half` for the name this write slot spells, held
/// to `Check`. A slot written through `Unread` names a binding the module does
/// not declare, so there is no declaration to hold it to.
template <typename Check, typename Half, typename Program, typename WriteSlot>
[[nodiscard]] consteval auto ModuleSatisfiesCheck() noexcept -> bool {
    if constexpr (IsUnreadSlot<WriteSlot>()) {
        return true;
    } else {
        using List = DeclaredList<Half, Program>;
        return CheckDeclaredSlotsAt<Check, List, WriteSlot>(std::make_index_sequence<List::count> {});
    }
}

/// Every slot of one write, against one module: the fold a set runs per program.
template <typename Check, typename Half, typename Program, typename... Slots>
[[nodiscard]] consteval auto ModuleSatisfiesChecks() noexcept -> bool {
    return (ModuleSatisfiesCheck<Check, Half, Program, Slots>() && ...);
}

// ----------------------------------------------------------------------------
// The push-constant check
// ----------------------------------------------------------------------------
//
// A pass's push block is declared twice: the module declares it -- the tool
// writes the padded size into `PushSize` and each member's name, offset and size
// into `Push` -- and the call site pushes a C++ struct. PushMember's comment
// says "this is what holds one against the other", and nothing read it: a
// reordered or retyped field in a hand-written push struct was an ABI change no
// compiler looked at.
//
// What can be proved is the bytes, plus the names wherever both sides describe
// the same bytes at the same granularity:
//
//   * the two sides describe the same bytes: the merged ranges of the payload's
//     fields and of the block's members must be equal, so a field that pokes
//     into a neighbouring word, a hole between two fields the shader has no hole
//     for, or a block the payload does not fill, is a build failure. The
//     payload's `sizeof` is deliberately not compared: a class ends on its
//     alignment, so `ScenePassPushConstants` is 192 bytes over a 180-byte block
//     (`alignas(16)`) and the trailing 12 bytes are a value no member of either
//     side has -- bytes, says the cover, and the cover is what the shader reads;
//
//   * where one payload field is one block member -- and not a member spanning
//     several fields, like the SMAA metrics payload against the shader's single
//     `rtMetrics`, which the bytes prove is the same word -- the two names must
//     agree. A reorder of two same-sized words is invisible to the bytes, and
//     this is what sees it.
//
// A payload goes to one pipeline out of a family (`Shaders::Bake` carries four
// bakes with four different blocks), so the check is existential: some module of
// the set must declare exactly this block. A family where none does is a payload
// nothing reads.

/// What the payload and one module's block say about each other: `Agrees`, or
/// the first statement that is not true. The compiler prints these words in the
/// diagnostic below.
enum class PushDisagreement : uint8_t { Agrees, Missing, Cover, Name };

/// The payload is not the push block of `Program`; `Member` is the member of the
/// block the disagreement is about. Declared and never defined, like the binding diagnostics above:
/// reaching into it is how the check reports, and the instantiation carries the
/// payload, the module and the member into the compiler's words --
///
///     error: implicit instantiation of undefined template
///       'ZHLN::Vk::TemplatedDetail::PushPayloadMismatch<BloomBrightPush,
///        Shaders::Modules::BloomThresholdCS, 3, PushDisagreement::Name>'
///
/// -- rather than leaving a shader to read a word the C++ wrote somewhere else.
/// The payload and the module are both in the line: which push struct, which
/// shader, which member of it, and what about that member.
template <typename Payload, typename Program, uint32_t Member, PushDisagreement Disagreement>
struct PushPayloadMismatch;

/// No module of the set declares a push block: the payload is bytes the shader
/// never reads, and no member of anything could be named.
template <typename Payload, typename Set>
struct PushPayloadDeclaredNowhere;

/// One word of either side: what it is called, where it starts, how wide it is.
struct PushWord {
    std::string_view name;
    uint32_t         offset = 0;
    uint32_t         size   = 0;
};

/// One run of bytes, as either side describes it.
struct ByteRun {
    uint32_t offset = 0;
    uint32_t size   = 0;
};

/// What the words of one side cover, merged into runs. Merging only ever
/// removes runs, so one run per word is the worst case and the array cannot
/// overflow: no size has to be invented here.
template <size_t Words>
struct ByteCover {
    std::array<ByteRun, Words> runs {};
    size_t                     count = 0;
};

/// One module's block, in the order the tool wrote it.
template <typename Program>
[[nodiscard]] consteval auto BlockWords() {
    std::array<PushWord, std::size(Program::Push)> words {};
    for (size_t index = 0; index < words.size(); ++index) {
        words[index] = PushWord {.name = Program::Push[index].name, .offset = Program::Push[index].offset, .size = Program::Push[index].size};
    }
    return words;
}

/// One payload's fields, as this compiler laid them out. A class lays its
/// members out in declaration order and the tool writes the block in decoration
/// order, so both sides arrive sorted by offset and the merge is one pass; a
/// header that arrived unsorted would be reported, not believed.
template <typename Payload>
[[nodiscard]] consteval auto PayloadWords() {
    std::array<PushWord, ZHLN::Reflect::FieldCount<Payload>()> words {};
    size_t                                                     index = 0;
    ZHLN::Reflect::ForEachFieldInfo<Payload>([&]<typename FieldType>(std::string_view name, std::size_t offset) {
        words[index++] = PushWord {.name = name, .offset = static_cast<uint32_t>(offset), .size = static_cast<uint32_t>(sizeof(FieldType))};
    });
    return words;
}

template <size_t Words>
[[nodiscard]] consteval auto MergedCover(const std::array<PushWord, Words>& words) -> ByteCover<Words> {
    ByteCover<Words> cover {};
    for (const PushWord& word: words) {
        if (word.size == 0) {
            continue;
        }
        if (cover.count > 0 && word.offset <= cover.runs[cover.count - 1].offset + cover.runs[cover.count - 1].size) {
            const uint32_t end = std::max(cover.runs[cover.count - 1].offset + cover.runs[cover.count - 1].size, word.offset + word.size);
            cover.runs[cover.count - 1].size = end - cover.runs[cover.count - 1].offset;
        } else {
            cover.runs[cover.count++] = ByteRun {.offset = word.offset, .size = word.size};
        }
    }
    return cover;
}

/// True when the two sides describe the same bytes. A member a payload splits
/// into several fields (or the reverse) is the same bytes and agrees here.
template <size_t BlockWords_, size_t PayloadWords_>
[[nodiscard]] consteval auto CoversAgree(const ByteCover<BlockWords_>& block, const ByteCover<PayloadWords_>& payload) -> bool {
    if (block.count != payload.count) {
        return false;
    }
    for (size_t run = 0; run < block.count; ++run) {
        if (block.runs[run].offset != payload.runs[run].offset || block.runs[run].size != payload.runs[run].size) {
            return false;
        }
    }
    return true;
}

/// The first member of the block no payload field covers byte for byte: where a
/// cover disagreement starts reading. Ranges, not offsets, so a payload that
/// describes the same bytes through a different number of fields is not
/// reported here.
template <typename Payload, size_t Members>
[[nodiscard]] consteval auto FirstUncoveredMember(const std::array<PushWord, Members>& members) -> uint32_t {
    constexpr auto fields = PayloadWords<Payload>();
    for (uint32_t index = 0; index < members.size(); ++index) {
        const uint32_t begin  = members[index].offset;
        const uint32_t end    = members[index].offset + members[index].size;
        uint32_t       filled = begin;
        for (uint32_t boundary = begin; boundary <= end; ++boundary) {
            bool inField = false;
            for (const PushWord& field: fields) {
                inField = inField || (field.offset <= boundary && boundary < field.offset + field.size);
            }
            filled = inField ? std::max(filled, boundary + 1) : filled;
        }
        if (filled != end) {
            return index;
        }
    }
    return 0;
}

/// Everything the payload and one module's block say about each other.
struct PushVerdict {
    PushDisagreement disagreement = PushDisagreement::Agrees;
    uint32_t         member       = 0;
};

/// The comparison itself, quiet, so a set can ask every module it holds: a
/// payload is pushed to one pipeline of a family, and the modules it was never
/// meant for must have nothing to say about it.
template <typename Payload, typename Program>
[[nodiscard]] consteval auto JudgePayload() -> PushVerdict {
    if constexpr (!requires { Program::PushSize; Program::Push; }) {
        return {.disagreement = PushDisagreement::Missing};
    } else {
        constexpr auto members = BlockWords<Program>();
        constexpr auto fields  = PayloadWords<Payload>();
        if (!CoversAgree(MergedCover(members), MergedCover(fields))) {
            return {.disagreement = PushDisagreement::Cover, .member = FirstUncoveredMember<Payload>(members)};
        }
        for (uint32_t index = 0; index < members.size(); ++index) {
            for (const PushWord& field: fields) {
                if (members[index].offset == field.offset && members[index].size == field.size && members[index].name != field.name) {
                    return {.disagreement = PushDisagreement::Name, .member = index};
                }
            }
        }
        return {};
    }
}

/// The payload against one module's block, as a yes or a no.
template <typename Payload, typename Program>
[[nodiscard]] consteval auto PayloadMatchesBlock() -> bool {
    return JudgePayload<Payload, Program>().disagreement == PushDisagreement::Agrees;
}

/// The same comparison, and the diagnostic for what it found.
template <typename Payload, typename Program>
consteval void ReportPayloadMismatch() {
    constexpr PushVerdict verdict = JudgePayload<Payload, Program>();
    if constexpr (verdict.disagreement == PushDisagreement::Cover) {
        static_cast<void>(sizeof(PushPayloadMismatch<Payload, Program, verdict.member, PushDisagreement::Cover>));
    } else if constexpr (verdict.disagreement == PushDisagreement::Name) {
        static_cast<void>(sizeof(PushPayloadMismatch<Payload, Program, verdict.member, PushDisagreement::Name>));
    }
}

/// True when any flag is set: how a set answers "does one of my modules
/// declare this block". The answer has to be a constant expression usable in an
/// `if constexpr`, because the diagnostic below must not be *instantiated* when
/// something did match -- and a call written after an early `return` inside a
/// consteval body is still instantiated. That was the difference between a set
/// of one module (which compiled) and a set whose second or fourth module was
/// the one that matched (which reported a mismatch).
template <size_t Count>
[[nodiscard]] consteval auto AnyOf(const std::array<bool, Count>& flags) -> bool {
    for (const bool flag: flags) {
        if (flag) {
            return true;
        }
    }
    return false;
}

/// True when the module declares a push-constant block at all.
template <typename Program>
[[nodiscard]] consteval auto DeclaresPushBlock() -> bool {
    return requires {
        Program::PushSize;
        Program::Push;
    };
}

/// Reports the payload against the first module of the pack that declares a
/// block: the family's block, which is what a caller meant when nothing
/// matched. A set whose modules declare none gets the other diagnostic.
template <typename Set, typename Payload>
consteval void ReportPushPayload() {
    static_cast<void>(sizeof(PushPayloadDeclaredNowhere<Payload, Set>));
}

template <typename Set, typename Payload, typename First, typename... Rest>
consteval void ReportPushPayload() {
    if constexpr (DeclaresPushBlock<First>()) {
        ReportPayloadMismatch<Payload, First>();
    } else {
        ReportPushPayload<Set, Payload, Rest...>();
    }
}

} // namespace TemplatedDetail

/// The programs one descriptor block serves, as a type.
///
/// A pass is not one module: lighting.slang compiles as RT and NoRT, SMAA.slang
/// as EDGE, WEIGHT and BLEND, and one call site writes the block all of them
/// read. The set is what makes that explicit -- and what makes a declaration
/// unnecessary, because the checks below ask the modules themselves.
template <ShaderProgram... Programs>
struct ShaderSet {
    static constexpr uint32_t programCount = sizeof...(Programs);

    /// True when some module of the set declares a binding of `Half` named
    /// `name`.
    template <typename Half>
    [[nodiscard]] static consteval auto Declares(std::string_view name) -> bool {
        return (DeclaredList<Half, Programs>::Declares(name) || ...);
    }

    /// Every name `Slots...` spell is declared by some module of the set: the
    /// typo direction. The failing slot is the one the compiler names.
    template <typename Half, typename... Slots>
    [[nodiscard]] static consteval auto SpellsDeclaredNames() -> bool {
        constexpr uint64_t mask = (TemplatedDetail::DeclaredSlotMask<Half, Programs, Slots...>() | ...);
        TemplatedDetail::RequireDeclaredBits<ShaderSet, Half, mask, Slots...>(std::index_sequence_for<Slots...> {});
        return true;
    }

    /// Every binding the set's modules declare is spelled by `Slots...`: the
    /// forgotten-argument direction. Per module and not per union -- whichever
    /// configuration of the pass runs, its bindings are written.
    template <typename Half, typename... Slots>
    [[nodiscard]] static consteval auto DeclarationsAreSpelled() -> bool {
        return (TemplatedDetail::SpellsEveryDeclaration<ShaderSet, Half, Programs, Slots...>() && ...);
    }

    /// Every module's declaration for each name a write spells, held to `Check`:
    /// the hook for a check that needs the declaration itself -- its descriptor
    /// type -- and not only its name. `Check` is a type with
    /// `template <typename DeclaredSlot, typename WriteSlot> static consteval
    /// auto Holds() -> bool`; HeapBindings.hpp supplies the one that knows what
    /// shape of value this writer can carry.
    template <typename Half, typename Check, typename... Slots>
    [[nodiscard]] static consteval auto DeclarationsHold() -> bool {
        return (TemplatedDetail::ModuleSatisfiesChecks<Check, Half, Programs, Slots...>() && ...);
    }

    /// True when `Payload` is the push block one of the set's modules declares.
    /// A payload is pushed to one pipeline of a family -- `Shaders::Bake`
    /// carries four bakes with four different blocks -- so the question is
    /// existential: the modules are asked as a list, and the report (which is
    /// an undefined template, so instantiating it is a hard error) sits in the
    /// `if constexpr` branch that only the all-no answer reaches.
    template <typename Payload>
    [[nodiscard]] static consteval auto HoldsPushPayload() -> bool {
        // A build whose compiler has no reflection (the project's explicit
        // ZHLN_ALLOW_REFLECTION_STUBS opt-out) has no member list to walk: the
        // check is inert there rather than wrong, and every reflecting build
        // runs it.
        if constexpr (!Reflect::ReflectionAvailable) {
            return true;
        } else {
            constexpr std::array<bool, sizeof...(Programs)> matches {TemplatedDetail::PayloadMatchesBlock<Payload, Programs>()...};
            if constexpr (TemplatedDetail::AnyOf(matches)) {
                return true;
            } else {
                TemplatedDetail::ReportPushPayload<ShaderSet, Payload, Programs...>();
                return false;
            }
        }
    }
};

/// A set of shader programs: what the gates below take, and what a descriptor
/// write hands them.
template <typename T>
concept ShaderProgramSet = requires {
    { T::programCount } -> std::convertible_to<uint32_t>;
    { T::template Declares<ResourceBindings>(std::string_view {}) } -> std::same_as<bool>;
};

/// True when every name `Slots...` spell is a binding some module of `Set`
/// declares. A name that is neither is a typo, and the compiler says which name.
template <typename Set, typename Half, typename... Slots>
[[nodiscard]] consteval auto NamesAreDeclared() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template SpellsDeclaredNames<Half, Slots...>();
}

/// True when every binding the set's modules declare is spelled by `Slots...`:
/// the direction that catches a forgotten argument.
template <typename Set, typename Half, typename... Slots>
[[nodiscard]] consteval auto NamesCoverDeclarations() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template DeclarationsAreSpelled<Half, Slots...>();
}

/// True when `Payload` -- the struct a call site pushes -- is the push-constant
/// block `Set` declares: the same size, the same bytes, and the same names
/// wherever one word of either side describes those bytes. This is what holds a
/// hand-written push struct against the shader it feeds.
template <typename Set, typename Payload>
[[nodiscard]] consteval auto PushPayloadMatchesDeclaration() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a push payload names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template HoldsPushPayload<Payload>();
}

/// True when no two arguments name the same binding: a repeated name is a second
/// write over the first, and only the last one survives.
template <typename... Slots>
[[nodiscard]] consteval auto NamesAreDistinct() noexcept -> bool {
    constexpr std::array<std::string_view, sizeof...(Slots)> spelled {Slots::name...};
    for (size_t i = 0; i < spelled.size(); ++i) {
        for (size_t j = i + 1; j < spelled.size(); ++j) {
            if (spelled[i] == spelled[j]) {
                return false;
            }
        }
    }
    return true;
}

/// True when every declared binding a write names satisfies `Check` for it: the
/// descriptor type the module declares against the value the write carries. The
/// two gates above say *which* names are wrong; this one says whether what sits
/// under a right name is the shape of descriptor the module reads.
template <typename Set, typename Half, typename Check, typename... Slots>
[[nodiscard]] consteval auto DeclarationsSatisfy() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template DeclarationsHold<Half, Check, Slots...>();
}

// ============================================================================
// What a program hands the pipeline
// ============================================================================

/// The pipeline stage description for a program: its own bytes, and its own
/// entry point. The module the checks ran against is the module that gets
/// loaded.
template <ShaderProgram Program>
[[nodiscard]] auto CreateShaderDesc() noexcept -> ZHLN_ShaderDesc {
    const std::span<const uint8_t> bytes = Program::Bytes();
    // A generated catalog states the entry point as a literal, and the concept
    // lets a program state it as either a literal or a string_view; both are a
    // null-terminated name, which is what the loader takes.
    const std::string_view entryPoint = Program::EntryPoint;
    return ZHLN_ShaderDesc {
        .code        = std::bit_cast<const uint32_t*>(bytes.data()),
        .size        = bytes.size_bytes(),
        .entry_point = entryPoint.data(),
    };
}

/// The stage a program was compiled for, as the module declared it.
template <ShaderProgram Program>
[[nodiscard]] consteval auto StageOf() noexcept -> VkShaderStageFlagBits {
    return Program::Stage;
}

// ============================================================================
// Holding the generated catalog to the modules it was generated from
// ============================================================================

/// The execution model a stage is compiled to, as the number OpEntryPoint
/// carries (0 Vertex, 4 Fragment, 5 GLCompute, 5364 TaskEXT, 5365 MeshEXT).
[[nodiscard]] consteval auto ExecutionModelOf(VkShaderStageFlagBits stage) noexcept -> uint32_t {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            return 0;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return 4;
        case VK_SHADER_STAGE_COMPUTE_BIT:
            return 5;
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            return 5364;
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            return 5365;
        default:
            return 0xFFFFFFFFu;
    }
}

namespace TemplatedDetail {

/// True when the module's bytes declare this slot, in the half it belongs to.
/// A slot of another set is another set's parse to answer, so it is not this
/// one's to fail.
template <uint32_t Set, typename Slot, bool Sampler>
[[nodiscard]] consteval auto DeclaredIsInSet(const SpirvBindings& declarations) noexcept -> bool {
    if constexpr (Slot::set != Set) {
        return true;
    } else if constexpr (Sampler) {
        return declarations.DeclaresSampler(Slot::name);
    } else {
        return declarations.DeclaresResource(Slot::name);
    }
}

template <uint32_t Set, typename List, bool Sampler, size_t... Index>
[[nodiscard]] consteval auto EveryDeclaredSlotIsInSetAt(const SpirvBindings& declarations, std::index_sequence<Index...>) noexcept -> bool {
    return (DeclaredIsInSet<Set, std::tuple_element_t<Index, SlotsOfT<List>>, Sampler>(declarations) && ...);
}

/// The direction a stale or wrong generated list trips: every slot the tool
/// wrote down for this set has to be a binding the module's bytes declare in it.
template <uint32_t Set, typename List, bool Sampler>
[[nodiscard]] consteval auto EveryDeclaredSlotIsInSet(const SpirvBindings& declarations) noexcept -> bool {
    return EveryDeclaredSlotIsInSetAt<Set, List, Sampler>(declarations, std::make_index_sequence<std::tuple_size_v<SlotsOfT<List>>> {});
}

/// The highest set either of a module's lists names (0 when both are empty):
/// how far ModuleMatchesBytes has to walk.
template <typename List, size_t... Index>
[[nodiscard]] consteval auto HighestSetAt(std::index_sequence<Index...>) noexcept -> uint32_t {
    uint32_t highest = 0;
    ((highest = std::tuple_element_t<Index, SlotsOfT<List>>::set > highest ? std::tuple_element_t<Index, SlotsOfT<List>>::set : highest), ...);
    return highest;
}
template <typename List>
[[nodiscard]] consteval auto HighestSetIn() noexcept -> uint32_t {
    return HighestSetAt<List>(std::make_index_sequence<std::tuple_size_v<SlotsOfT<List>>> {});
}

/// The highest set either list of a module names: how far the walk goes.
template <ShaderProgram Module>
[[nodiscard]] consteval auto HighestSetInModule() noexcept -> uint32_t {
    const uint32_t resources = HighestSetIn<typename Module::Resources>();
    const uint32_t samplers  = HighestSetIn<typename Module::Samplers>();
    return resources > samplers ? resources : samplers;
}

/// One set of a module's declarations, held against the module's own bytes in
/// both directions: nothing the bytes declare in the set is missing from the
/// list, and nothing the list declares is missing from the bytes. The parse
/// arrives from the caller, so a module whose lists name one set is parsed once.
template <ShaderProgram Module, uint32_t Set>
[[nodiscard]] consteval auto SetMatchesBytes(const SpirvBindings& declarations) noexcept -> bool {
    if (!declarations.Complete()) {
        return false;
    }
    for (uint32_t i = 0; i < declarations.Count(); ++i) {
        const SpirvBinding& binding  = declarations[i];
        const bool          declared = binding.sampler ? Module::Samplers::Spells(declarations, binding, Set) :
                                                         Module::Resources::Spells(declarations, binding, Set);
        if (!declared) {
            return false;
        }
    }
    if (!EveryDeclaredSlotIsInSet<Set, typename Module::Resources, false>(declarations)) {
        return false;
    }
    return EveryDeclaredSlotIsInSet<Set, typename Module::Samplers, true>(declarations);
}

/// Every set above the first: set 0 came parsed from ModuleMatchesBytes, and
/// the rest are walked here. One extra parse for the one module family in the
/// engine that spreads its bindings over two sets (decal.slang), none for the
/// other seventy.
template <ShaderProgram Module, size_t... Index>
[[nodiscard]] consteval auto HigherSetsMatch(const SpirvBindings& first, std::span<const uint8_t> bytes, std::index_sequence<Index...>) noexcept -> bool {
    constexpr uint32_t kFirstOfTheRest = 1;
    return (SetMatchesBytes<Module, static_cast<uint32_t>(Index) + kFirstOfTheRest>(
                SpirvBindings::Parse(bytes, static_cast<uint32_t>(Index) + kFirstOfTheRest)
            ) &&
            ...);
}

} // namespace TemplatedDetail

/// True when everything the generated catalog says about `Module` -- its entry
/// point, its stage, its bindings and their kinds -- is what its own bytes say,
/// read by the independent parser in SpirvBindings.hpp rather than by
/// SPIRV-Reflect, which the tool used.
///
/// Called from the generated ShaderBytecode.cpp, once per module, with the
/// `#embed`ded array in hand: that is the only place a module's bytes are
/// constant-expression data, and the only place this check can run. A generator
/// that reflects a module wrongly, or a generated header that a rebuild left
/// stale against a recoooked module, fails the build here instead of writing a
/// descriptor nobody declared.
template <ShaderProgram Module>
[[nodiscard]] consteval auto ModuleMatchesBytes(std::span<const uint8_t> bytes) noexcept -> bool {
    const SpirvBindings head = SpirvBindings::Parse(bytes, 0);
    if (!head.Complete() || head.EntryPointCount() != 1) {
        return false;
    }
    if (!head.IsEntryPoint(Module::EntryPoint)) {
        return false;
    }
    if (head.ExecutionModel() != ExecutionModelOf(Module::Stage)) {
        return false;
    }
    // Per set, both directions and both kinds: what the module declares in a set
    // and what the generated list says it declares in that set are the same set
    // of bindings, not merely overlapping ones. decal.slang is why this is not
    // "set 0": its vertex stage declares nothing at all in set 0 and its
    // fragment stage reads the scene block from set 1.
    constexpr uint32_t kHighest = TemplatedDetail::HighestSetInModule<Module>();
    // A set the module declares but no list names would go unchecked: the lists
    // have to reach at least as far as the module does.
    if (head.HighestDeclaredSet() > kHighest) {
        return false;
    }
    return TemplatedDetail::SetMatchesBytes<Module, 0>(head) &&
           TemplatedDetail::HigherSetsMatch<Module>(head, bytes, std::make_index_sequence<static_cast<size_t>(kHighest)> {});
}

} // namespace ZHLN::Vk
