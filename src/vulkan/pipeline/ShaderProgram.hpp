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
//     module that gets loaded;
//
//   * push constants are checked at the call that writes them. The
//     `Dispatch*` / `Execute*` / `Draw*` entry points take the shader
//     module(s) whose bytes read the payload as their template arguments and
//     assert `PushConstantLayoutMatchesAll` inside, so a payload that is not
//     their push-constant block -- a renamed member, a moved word, a size the
//     host padded differently -- does not compile, and neither does a call
//     that names no module at all. The free `PushHeapData` is the same
//     contract for a write that is not a dispatch.
//
// There is no table to keep in step with the shaders and no parser in the way of
// a build: the lists are data in a header, the walk happens once per module in
// the one translation unit that has the bytes, and a write site costs a
// membership test over a type list.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "PushDataLayout.hpp" // AlignUp: the check below reads PushSize the way a host ABI does

#include <Zahlen/Core/Description.hpp> // StringLiteral: a binding name is a template argument
#include <Zahlen/Core/Reflection/Structs.hpp> // ForEachFieldInfo: what the hand-written struct declares

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility> // std::forward: the set's dispatch helper passes its arguments through

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

/// True when a module's push-constant block declares no more than a struct can
/// be held against. A module that declares no block at all -- every stage that
/// pushes nothing (most fragment stages, cluster bounds, the SMAA edges) -- is
/// a module with nothing to compare, and that is not the same as an empty match.
template <typename Module>
concept DeclaresPushBlock = requires {
    Module::PushSize;
    Module::Push;
};

/// True when `CppPush` is the struct `Module`'s push-constant block declares:
/// the same members in the same order, each at the same offset, with the same
/// size and the same name, and a `sizeof` the block accounts for.
///
/// The engine's push structs are hand-written -- they carry VkDeviceAddress and
/// engine math types SPIR-V has no name for -- so this is the one thing that
/// holds them against the shader. A member the shader renamed, a field that
/// moved by four bytes, a word the shader turns out to take from a
/// specialization constant while the host still writes it: each of those is a
/// build failure here instead of a value landing where nobody reads it.
///
/// `Module::PushSize` is SPIRV-Reflect's `padded_size` for the block, which for
/// push constants is how far the members reach and *not* the padded extent: 84
/// bytes for `culling.slang`'s matrix-plus-counters struct. A C++ struct's size
/// is always a multiple of its alignment, so the number to hold it to is that
/// extent rounded up to `alignof(CppPush)` -- `{ float; float3 }` is 28 bytes of
/// members and a 32-byte C++ struct, and culling's 84 is the host's 96. The
/// per-member offsets and sizes are compared exactly, which is where the
/// padding has to be right anyway.
///
/// Without reflection there are no field names or offsets to walk --
/// `ForEachFieldInfo` visits nothing -- so such a build checks the size and
/// nothing else. That is the most a build without reflection can honestly
/// claim, and the tuple check is skipped rather than failed: the engine's own
/// builds all have reflection (Reflection/Core.hpp refuses to compile without
/// it unless the stubs are asked for by name).
template <typename CppPush, typename Module>
[[nodiscard]] consteval auto PushConstantLayoutMatches() noexcept -> bool {
    if constexpr (!DeclaresPushBlock<Module>) {
        return false;
    } else {
        constexpr uint32_t kMembers = static_cast<uint32_t>(sizeof(Module::Push) / sizeof(Module::Push[0]));
        bool               ok       = sizeof(CppPush) == ::ZHLN::Vk::AlignUp(Module::PushSize, static_cast<uint32_t>(alignof(CppPush)));
#if ZHLN_REFLECTION_AVAILABLE
        uint32_t index = 0;
        Reflect::ForEachFieldInfo<CppPush>([&]<typename FieldType>(std::string_view name, std::size_t offset) {
            if (index >= kMembers) {
                ok = false;
                return;
            }
            const PushMember& member = Module::Push[index];
            ok = ok && name == member.name && static_cast<uint32_t>(offset) == member.offset && sizeof(FieldType) == member.size;
            ++index;
        });
        ok = ok && index == kMembers;
#endif
        return ok;
    }
}

/// The reader the generated catalog is verified with, and one binding it
/// reports. Only `BindingList::Spells` -- and the checks in CatalogChecks.hpp
/// that call it -- touch these; declaring them is all the list itself needs,
/// and it keeps the ~500-line reader out of this header's include closure.
struct SpirvBinding;
class SpirvBindings;

/// The bindings one module declares, in the order the tool reflected them.
template <typename... Slots>
struct BindingList {
    static constexpr size_t count = sizeof...(Slots);

    /// True when one of the slots is named `name`. An empty list -- every
    /// module whose samplers are all statically bound, which is most of them --
    /// declares nothing, so the fold answers without touching the argument;
    /// `[[maybe_unused]]` is what that costs under -Wunused-but-set-parameter.
    [[nodiscard]] static constexpr auto Declares([[maybe_unused]] std::string_view name) noexcept -> bool {
        return ((Slots::name == name) || ...);
    }

    /// True when one of the slots names the binding `candidate` holds in the
    /// module's bytes -- the same name in the same set, since a module may
    /// declare bindings in more than one set and a name in set 1 is not the
    /// binding a list says sits in set 0. The direction a stale or wrong
    /// generated list trips.
    ///
    /// Defined in CatalogChecks.hpp, with the checks that call it: the body is
    /// the one thing in this header that needs the reader's types, and the
    /// declaration is enough for everything that merely passes a list around.
    [[nodiscard]] static constexpr auto Spells(const SpirvBindings& declarations, const SpirvBinding& candidate, uint32_t set) noexcept -> bool;
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

/// One module's half of a fold over several modules: a module that declares no
/// push block has nothing to hold a payload against and stays out of it, which
/// is what lets a draw name both halves of a pipeline and a set name a payload
/// only some of its configurations read.
template <typename CppPush, ShaderProgram Module>
[[nodiscard]] consteval auto PushConstantLayoutMatchesOne() noexcept -> bool {
    if constexpr (!DeclaresPushBlock<Module>) {
        return true;
    } else {
        return PushConstantLayoutMatches<CppPush, Module>();
    }
}

/// `PushConstantLayoutMatches` for a payload more than one configuration of a
/// pass reads: Lighting's RT and NoRT modules, a mesh pass's task and vertex
/// halves, a draw that names both halves of a material's pipeline -- one struct,
/// several modules, and the push struct has to be all of them.
///
/// Two clauses, and both matter:
///
///   * every module that declares a push block declares *this* one. A module
///     that declares none -- a fragment stage that only reads interpolants --
///     has nothing to hold the payload against and is skipped, the same rule
///     `ShaderSet::PushLayoutMatches` uses for its configurations;
///
///   * at least one of the modules named declares one. Naming no module that
///     reads the bytes is the mistake this check exists to catch, so it fails
///     here rather than passing for lack of anything to compare, and a call
///     that names no module at all (an empty pack) fails the same way.
template <typename CppPush, ShaderProgram... Modules>
[[nodiscard]] consteval auto PushConstantLayoutMatchesAll() noexcept -> bool {
    static_assert(
        sizeof...(Modules) > 0, "name the shader module(s) this push struct is written for: PushConstantLayoutMatchesAll<PushT, Shaders::Modules::X>()"
    );
    static_assert((DeclaresPushBlock<Modules> || ...), "none of the named shader modules declares a push-constant block: the bytes would be written for no one");
    return (PushConstantLayoutMatchesOne<CppPush, Modules>() && ...);
}

/// The one call that puts a host push struct into the heap push-data blob, for
/// a caller that names the module(s) whose bytes read it -- the sibling of
/// `PushHeapIndex` and `PushHeapFrameAddresses`, which write the blob's other
/// half. `PushData` writes bytes and asks nothing; this is the same write with
/// the contract in it, so a push site cannot hand a module a struct it does not
/// declare, or hand nothing to a module at all. A struct declared inside a
/// lambda can name its module here like any other.
template <ShaderProgram... Modules, typename T>
void PushHeapData(const Context& ctx, VkCommandBuffer cmd, const T& value) noexcept {
    static_assert(sizeof...(Modules) > 0, "name the shader module(s) this push struct is written for: PushHeapData<Shaders::Modules::X>(...)");
    static_assert(
        PushConstantLayoutMatchesAll<T, Modules...>(),
        "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
    );
    PushData(ctx, cmd, 0, value);
}

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
    /// `name`. A set is generated with its modules, so it is never empty today;
    /// the attribute is here so that the empty fold stays silent if one is.
    template <typename Half>
    [[nodiscard]] static consteval auto Declares([[maybe_unused]] std::string_view name) -> bool {
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

    /// Every module of the set that declares a push block declares *this* one:
    /// the payload a pass writes and each configuration of it reads. Modules
    /// without a push block are skipped -- they have nothing to agree with --
    /// which is what lets a set (SMAA's stages, a bloom chain's step) name the
    /// payload where the pass has one. A set whose modules declare *different*
    /// blocks (the bakes, each with its own) fails here, which is the right
    /// answer: one payload is not all of them.
    template <typename CppPush>
    [[nodiscard]] static consteval auto PushLayoutMatches() -> bool {
        return (PushConstantLayoutMatchesOne<CppPush, Programs>() && ...);
    }

    /// The modules of this set, as one dispatch needs them: a compute chain
    /// holds the pass's declaration and each of its steps dispatches through
    /// the pass, so the expansion from "the set" to "the modules the entry
    /// point names" happens here -- and that entry point's check, the one that
    /// cannot be skipped, still sees every module of the set.
    template <typename Pass, typename... Args>
    static void DispatchHeapIndexed(Pass& pass, Args&&... args) noexcept {
        pass.template DispatchHeapIndexedThreads<Programs...>(std::forward<Args>(args)...);
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

} // namespace ZHLN::Vk
