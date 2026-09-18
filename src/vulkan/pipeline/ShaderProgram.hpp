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

#include <Zahlen/Core/Description.hpp> // StringLiteral: a binding name is a template argument

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace ZHLN::Vk {

/// Which half of a pass's descriptors a binding belongs to. The static sampler
/// heap holds the samplers, the resource heap everything else, and a write is
/// checked against the half it writes (`BindingKind::Resource` for
/// WriteHeapParameters, `BindingKind::Sampler` for InitHeapPassSamplers).
enum class BindingKind : uint8_t { Resource, Sampler };

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
    /// module's bytes: the direction a stale or wrong generated list trips.
    [[nodiscard]] static constexpr auto Spells(const SpirvBindings& declarations, const SpirvBinding& candidate) noexcept -> bool {
        return (candidate.IsNamed(declarations.Bytes(), Slots::name) || ...);
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

/// A module's declaration list of one kind; the half a write of that kind is
/// checked against.
template <BindingKind Kind, typename Program>
struct DeclaredListOf;
template <typename Program>
struct DeclaredListOf<BindingKind::Resource, Program> {
    using type = typename Program::Resources;
};
template <typename Program>
struct DeclaredListOf<BindingKind::Sampler, Program> {
    using type = typename Program::Samplers;
};
template <BindingKind Kind, typename Program>
using DeclaredList = typename DeclaredListOf<Kind, Program>::type;

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
///        BindingKind::Resource, ZHLN::StringLiteral<10>{"texInpuut"}>'
///
/// -- so a misspelling is named rather than left to be found by looking at the
/// image.
template <typename Set, BindingKind Kind, ZHLN::StringLiteral Name>
struct UndeclaredBinding;

/// A binding a module declares that the write does not spell: the forgotten
/// argument. The diagnostic names the module that declares it and the binding
/// number that module gave it -- a transient block has no previous frame's
/// descriptor to fall back on, so the shader would read a stale one.
template <typename Set, BindingKind Kind, typename Program, uint32_t Binding>
struct UnspelledBinding;

/// Complete exactly when `Spelled` -- the `false` specialization deliberately
/// does not exist, so reaching it is the diagnostic.
template <typename Set, BindingKind Kind, typename Program, uint32_t Binding, bool Spelled>
struct DeclarationSpelledBy;
template <typename Set, BindingKind Kind, typename Program, uint32_t Binding>
struct DeclarationSpelledBy<Set, Kind, Program, Binding, true> {};

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
template <BindingKind Kind, typename Program, typename Slot>
[[nodiscard]] consteval auto SlotIsDeclared() noexcept -> bool {
    if constexpr (IsUnreadSlot<Slot>()) {
        return true;
    } else {
        return DeclaredList<Kind, Program>::Declares(Slot::name);
    }
}

/// Whatever a write's slots are, this bitmask says which of them the module
/// declares: one membership test per slot per module, no walking of bytes.
template <BindingKind Kind, typename Program, typename... Slots>
[[nodiscard]] consteval auto DeclaredSlotMask() noexcept -> uint64_t {
    uint64_t mask  = 0;
    uint32_t index = 0;
    ((mask |= SlotIsDeclared<Kind, Program, Slots>() ? (uint64_t {1} << index) : uint64_t {0}, ++index), ...);
    return mask;
}

/// Complete for a slot whose bit is set; the diagnostic for the one whose is not.
template <typename Set, BindingKind Kind, uint64_t Mask, typename... Slots, size_t... Index>
consteval void RequireDeclaredBits(std::index_sequence<Index...>) {
    (static_cast<void>(sizeof(std::conditional_t<
                           ((Mask >> Index) & 1u) != 0,
                           std::true_type,
                           UndeclaredBinding<Set, Kind, std::tuple_element_t<Index, std::tuple<Slots...>>::literal>
                       >)), ...);
}

/// True when one of the write's slots names this declared binding.
template <typename DeclaredSlot, typename... Slots>
[[nodiscard]] consteval auto SpellsDeclaredSlot() noexcept -> bool {
    return ((Slots::name == DeclaredSlot::name) || ...) || (IsUnreadSlot<Slots>() || ...);
}

/// One instantiation per binding the module declares: complete when the write
/// spells it.
template <typename Set, BindingKind Kind, typename Program, typename... Slots, size_t... Index>
consteval void RequireSpelledBindings(std::index_sequence<Index...>) {
    using List = DeclaredList<Kind, Program>;
    (static_cast<void>(sizeof(DeclarationSpelledBy<
                           Set,
                           Kind,
                           Program,
                           std::tuple_element_t<Index, SlotsOfT<List>>::binding,
                           SpellsDeclaredSlot<std::tuple_element_t<Index, SlotsOfT<List>>, Slots...>()
                       >)), ...);
}

/// One program's half of the cover check: true when it declares no binding of
/// `Kind` that the write's slots leave unspoken.
template <typename Set, BindingKind Kind, ShaderProgram Program, typename... Slots>
[[nodiscard]] consteval auto SpellsEveryDeclaration() -> bool {
    constexpr size_t declared = DeclaredList<Kind, Program>::count;
    RequireSpelledBindings<Set, Kind, Program, Slots...>(std::make_index_sequence<declared> {});
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

/// One module's declaration of `Kind` for the name this write slot spells, held
/// to `Check`. A slot written through `Unread` names a binding the module does
/// not declare, so there is no declaration to hold it to.
template <typename Check, BindingKind Kind, typename Program, typename WriteSlot>
[[nodiscard]] consteval auto ModuleSatisfiesCheck() noexcept -> bool {
    if constexpr (IsUnreadSlot<WriteSlot>()) {
        return true;
    } else {
        using List = DeclaredList<Kind, Program>;
        return CheckDeclaredSlotsAt<Check, List, WriteSlot>(std::make_index_sequence<List::count> {});
    }
}

/// Every slot of one write, against one module: the fold a set runs per program.
template <typename Check, BindingKind Kind, typename Program, typename... Slots>
[[nodiscard]] consteval auto ModuleSatisfiesChecks() noexcept -> bool {
    return (ModuleSatisfiesCheck<Check, Kind, Program, Slots>() && ...);
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

    /// True when some module of the set declares a binding of `kind` named
    /// `name`.
    template <BindingKind Kind>
    [[nodiscard]] static consteval auto Declares(std::string_view name) -> bool {
        return (DeclaredList<Kind, Programs>::Declares(name) || ...);
    }

    /// Every name `Slots...` spell is declared by some module of the set: the
    /// typo direction. The failing slot is the one the compiler names.
    template <BindingKind Kind, typename... Slots>
    [[nodiscard]] static consteval auto SpellsDeclaredNames() -> bool {
        constexpr uint64_t mask = (TemplatedDetail::DeclaredSlotMask<Kind, Programs, Slots...>() | ...);
        TemplatedDetail::RequireDeclaredBits<ShaderSet, Kind, mask, Slots...>(std::index_sequence_for<Slots...> {});
        return true;
    }

    /// Every binding the set's modules declare is spelled by `Slots...`: the
    /// forgotten-argument direction. Per module and not per union -- whichever
    /// configuration of the pass runs, its bindings are written.
    template <BindingKind Kind, typename... Slots>
    [[nodiscard]] static consteval auto DeclarationsAreSpelled() -> bool {
        return (TemplatedDetail::SpellsEveryDeclaration<ShaderSet, Kind, Programs, Slots...>() && ...);
    }

    /// Every module's declaration for each name a write spells, held to `Check`:
    /// the hook for a check that needs the declaration itself -- its descriptor
    /// type -- and not only its name. `Check` is a type with
    /// `template <typename DeclaredSlot, typename WriteSlot> static consteval
    /// auto Holds() -> bool`; HeapBindings.hpp supplies the one that knows what
    /// shape of value this writer can carry.
    template <BindingKind Kind, typename Check, typename... Slots>
    [[nodiscard]] static consteval auto DeclarationsHold() -> bool {
        return (TemplatedDetail::ModuleSatisfiesChecks<Check, Kind, Programs, Slots...>() && ...);
    }
};

/// A set of shader programs: what the gates below take, and what a descriptor
/// write hands them.
template <typename T>
concept ShaderProgramSet = requires {
    { T::programCount } -> std::convertible_to<uint32_t>;
    { T::template Declares<BindingKind::Resource>(std::string_view {}) } -> std::same_as<bool>;
};

/// True when every name `Slots...` spell is a binding some module of `Set`
/// declares. A name that is neither is a typo, and the compiler says which name.
template <typename Set, BindingKind Kind, typename... Slots>
[[nodiscard]] consteval auto NamesAreDeclared() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template SpellsDeclaredNames<Kind, Slots...>();
}

/// True when every binding the set's modules declare is spelled by `Slots...`:
/// the direction that catches a forgotten argument.
template <typename Set, BindingKind Kind, typename... Slots>
[[nodiscard]] consteval auto NamesCoverDeclarations() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template DeclarationsAreSpelled<Kind, Slots...>();
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
template <typename Set, BindingKind Kind, typename Check, typename... Slots>
[[nodiscard]] consteval auto DeclarationsSatisfy() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template DeclarationsHold<Kind, Check, Slots...>();
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
template <typename Slot, bool Sampler>
[[nodiscard]] consteval auto DeclaredIsInModule(const SpirvBindings& declarations) noexcept -> bool {
    if constexpr (Sampler) {
        return declarations.DeclaresSampler(Slot::name);
    } else {
        return declarations.DeclaresResource(Slot::name);
    }
}

template <typename List, bool Sampler, size_t... Index>
[[nodiscard]] consteval auto EveryDeclaredSlotIsInModuleAt(const SpirvBindings& declarations, std::index_sequence<Index...>) noexcept -> bool {
    return (DeclaredIsInModule<std::tuple_element_t<Index, SlotsOfT<List>>, Sampler>(declarations) && ...);
}

/// The direction a stale or wrong generated list trips: every slot the tool
/// wrote down has to be a binding the module's bytes actually declare.
template <typename List, bool Sampler>
[[nodiscard]] consteval auto EveryDeclaredSlotIsInModule(const SpirvBindings& declarations) noexcept -> bool {
    return EveryDeclaredSlotIsInModuleAt<List, Sampler>(declarations, std::make_index_sequence<std::tuple_size_v<SlotsOfT<List>>> {});
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
    const SpirvBindings declarations = SpirvBindings::Parse(bytes, 0);
    if (!declarations.Complete() || declarations.EntryPointCount() != 1) {
        return false;
    }
    if (!declarations.IsEntryPoint(Module::EntryPoint)) {
        return false;
    }
    if (declarations.ExecutionModel() != ExecutionModelOf(Module::Stage)) {
        return false;
    }
    // Both directions, per kind: the bytes and the generated list are the same
    // set of bindings, not merely overlapping sets.
    for (uint32_t i = 0; i < declarations.Count(); ++i) {
        const SpirvBinding& binding  = declarations[i];
        const bool          declared = binding.sampler ?
                                           Module::Samplers::Spells(declarations, binding) :
                                           Module::Resources::Spells(declarations, binding);
        if (!declared) {
            return false;
        }
    }
    if (!TemplatedDetail::EveryDeclaredSlotIsInModule<typename Module::Resources, false>(declarations)) {
        return false;
    }
    if (!TemplatedDetail::EveryDeclaredSlotIsInModule<typename Module::Samplers, true>(declarations)) {
        return false;
    }
    return true;
}

} // namespace ZHLN::Vk
