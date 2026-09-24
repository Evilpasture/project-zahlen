// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/vulkan/pipeline/ShaderProgram.hpp
//
// What a shader module is, and the compile-time checks a descriptor write runs
// against it.
//
// A module is a type stating its cooked bytes and the source a hot reload would
// reread; the entry point, stage and every declared binding are generated from
// those bytes by `tools/zshader` into `ShaderBindings.hpp`. The generated lists
// are not taken on trust -- the translation unit holding the bytes asserts them
// with `ModuleMatchesBytes` against the independent reader in SpirvBindings.hpp:
//
//   * `ShaderSet<...>` names the programs one descriptor block serves; a write
//     is checked against their binding lists in both directions (a name no module
//     declares, a declaration no argument spells);
//   * `CreateShaderDesc<Program>()` builds the stage from the same program type,
//     so the module the checks ran against is the one that gets loaded;
//   * push constants are checked at the writing call: `Dispatch*`/`Execute*`/
//     `Draw*` and `PushHeapData` assert `PushConstantLayoutMatchesAll` against
//     the modules whose bytes read the payload.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "PushDataLayout.hpp" // AlignUp

#include <Zahlen/Core/Description.hpp>          // StringLiteral
#include <Zahlen/Core/Reflection/Structs.hpp>   // ForEachFieldInfo

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility> // std::forward

namespace ZHLN::Vk {

// One descriptor binding a module declares, as `tools/zshader` read it out of the
// module. A type rather than a string so the write side can be held against it at
// compile time: the name is printable in a diagnostic, the descriptor type is a
// constant, and set/binding are what the module said.
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

// The two halves of a module's declarations, as types: resources (everything that
// is not a sampler, written by `WriteHeapParameters`) and static samplers
// (`InitHeapPassSamplers`). `Half::Of<Program>` is that half's declaration list,
// so the half travels as a type through every check below and a diagnostic says
// which half a write got wrong.
struct ResourceBindings {
    template <typename Program>
    using Of = typename Program::Resources;
};

struct SamplerBindings {
    template <typename Program>
    using Of = typename Program::Samplers;
};

// One member of a module's push-constant block, as the module's own
// OpMemberDecorate states it. The host's push structs carry VkDeviceAddress and
// math types SPIR-V has no name for -- and materialize the layout's padding as
// visible members -- so this is what holds one against the other.
struct PushMember {
    const char* name   = nullptr;
    uint32_t    offset = 0;
    uint32_t    size   = 0;
};

// True when a module declares a push-constant block at all. A stage that pushes
// nothing (most fragment stages, cluster bounds, the SMAA edges) has nothing to
// compare, which is not the same as an empty match.
template <typename Module>
concept DeclaresPushBlock = requires {
    Module::PushSize;
    Module::Push;
};

// True when `CppPush` is a push layout `Module` declares: every struct member
// either found in `Module::Push` at the same offset and size under the same
// name, or spanning a hole the block declares no member at all, and a
// `sizeof` that reaches exactly as far as the furthest matched member. A
// renamed member or a field that moved four bytes is a build failure here
// instead of a value landing where nobody reads it.
//
// The second clause is for the filler. Generated host structs materialize
// Slang's padding as real members (a `uint8_t _padN[]` closing the gap
// between the block's furthest member and its rounded size, so that the
// struct's `sizeof` and every `offsetof` state what the layout walk said
// instead of trusting the host's packing) -- and a hole by definition has no
// declared member to name-match against. A field is therefore payload when it
// overlaps any declared member's span: overlap with a differently-named or
// moved member fails as loudly as the missing exact match always did, while a
// span nobody reads is tolerated by being exactly what it claims to be.
//
// The catalog lists every push block the module DECLARES: Slang's public
// reflection cannot say which blocks survive to the emitted SPIR-V (its usage
// table stops before the push category -- see tools/zshader/SlangReflect.hpp),
// so the check matches the struct against the declarations it needs rather
// than demanding the whole list. Blocks beyond the struct's extent are dead
// weight the cook may or may not bind; they cannot misplace a byte the shader
// reads, because every matched member sits at its declared offset.
//
// The extent rule keeps the old one's shape: `Module::PushSize` is how far a
// block's members reach (84 bytes for culling.slang's struct), and a C++
// struct's size is a multiple of its alignment, so the matched extent rounds
// up to `alignof(CppPush)` -- culling's 84 against the host's 96.
//
// Without reflection there are no names or offsets to walk, so such a build
// checks the size against the full declared extent only, exactly as it did;
// the tuple check is skipped rather than failed (the engine's own builds all
// have reflection -- see Reflection/Core.hpp).
template <typename CppPush, typename Module>
[[nodiscard]] consteval auto PushConstantLayoutMatches() noexcept -> bool {
    if constexpr (!DeclaresPushBlock<Module>) {
        return false;
    } else {
        constexpr uint32_t kMembers = static_cast<uint32_t>(sizeof(Module::Push) / sizeof(Module::Push[0]));
#if ZHLN_REFLECTION_AVAILABLE
        bool         ok     = true;
        uint32_t     extent = 0;
        Reflect::ForEachFieldInfo<CppPush>([&]<typename FieldType>(std::string_view name, std::size_t offset) {
            const uint32_t begin = static_cast<uint32_t>(offset);
            const uint32_t end   = begin + static_cast<uint32_t>(sizeof(FieldType));
            bool           found = false;
            bool           hole  = true; // no declared member's span overlaps this field's
            for (uint32_t i = 0; i < kMembers; ++i) {
                const PushMember& member = Module::Push[i];
                if (name == member.name && begin == member.offset && sizeof(FieldType) == member.size) {
                    found = true;
                    extent  = extent > member.offset + member.size ? extent : member.offset + member.size;
                }
                hole = hole && !(begin < member.offset + member.size && member.offset < end);
            }
            ok = ok && (found || hole);
        });
        ok = ok && sizeof(CppPush) == ::ZHLN::Vk::AlignUp(extent, static_cast<uint32_t>(alignof(CppPush)));
        return ok;
#else
        // Without reflection there are no names to anchor the extent, so the
        // struct is held against the size the catalog reaches -- the same
        // strict comparison it always was where sizes are all a tool can
        // check. A build that compiles the engine with reflection enabled
        // (CMake insists) never takes this branch.
        return sizeof(CppPush) == ::ZHLN::Vk::AlignUp(Module::PushSize, static_cast<uint32_t>(alignof(CppPush)));
#endif
    }
}

// The reader the generated catalog is verified with. Declared, not included: only
// `BindingList::Spells` (defined in CatalogChecks.hpp) touches it, and this keeps
// the ~500-line reader out of this header's include closure.
struct SpirvBinding;
class SpirvBindings;

// The bindings one module declares, in the order the tool reflected them.
template <typename... Slots>
struct BindingList {
    static constexpr size_t count = sizeof...(Slots);

    // True when one of the slots is named `name`. An empty list (a module whose
    // samplers are all statically bound -- most of them) declares nothing, hence
    // `[[maybe_unused]]` under -Wunused-but-set-parameter.
    [[nodiscard]] static constexpr auto Declares([[maybe_unused]] std::string_view name) noexcept -> bool {
        return ((Slots::name == name) || ...);
    }

    // True when one of the slots names the binding `candidate` holds in the
    // module's bytes -- same name in the same set, since a name in set 1 is not the
    // binding a list says sits in set 0. This is the direction a stale or wrong
    // generated list trips. Defined in CatalogChecks.hpp: the body is the one thing
    // here that needs the reader's types.
    [[nodiscard]] static constexpr auto Spells(const SpirvBindings& declarations, const SpirvBinding& candidate, uint32_t set) noexcept -> bool;
};

// The slot types of a `BindingList`, as a tuple, for indexed access.
template <typename List>
struct SlotsOf;
template <typename... Slots>
struct SlotsOf<BindingList<Slots...>> {
    using tuple = std::tuple<Slots...>;
};
template <typename List>
using SlotsOfT = typename SlotsOf<List>::tuple;

// A module's declaration list for one half: what a write of that half is
// checked against.
template <typename Half, typename Program>
using DeclaredList = typename Half::template Of<Program>;

// One cooked shader module, known at compile time.
//
// `Bytes()` is defined once, by the generated ShaderBytecode.cpp, next to the
// `#embed`ded array it returns -- a translation unit can call it but cannot use
// it in a constant expression, which is why the verification of the generated
// lists happens there and the checks here work on the lists.
template <typename T>
concept ShaderProgram = requires {
    typename T::Resources;
    typename T::Samplers;
    { T::EntryPoint } -> std::convertible_to<std::string_view>;
    { T::Stage } -> std::convertible_to<VkShaderStageFlagBits>;
    { T::Path } -> std::convertible_to<const char*>;
    { T::Bytes() } -> std::same_as<std::span<const uint8_t>>;
};

// One module's half of a fold over several modules: a module with no push block
// stays out, which is what lets a draw name both halves of a pipeline.
template <typename CppPush, ShaderProgram Module>
[[nodiscard]] consteval auto PushConstantLayoutMatchesOne() noexcept -> bool {
    if constexpr (!DeclaresPushBlock<Module>) {
        return true;
    } else {
        return PushConstantLayoutMatches<CppPush, Module>();
    }
}

// `PushConstantLayoutMatches` for a payload several configurations of a pass read
// (Lighting's RT and NoRT modules, a mesh pass's task and vertex halves). Both
// clauses matter: every module that declares a push block must declare *this* one
// (modules with none are skipped), and at least one named module must declare one
// -- naming no module that reads the bytes is the mistake this catches, so an empty
// pack fails rather than passing for lack of anything to compare.
template <typename CppPush, ShaderProgram... Modules>
[[nodiscard]] consteval auto PushConstantLayoutMatchesAll() noexcept -> bool {
    static_assert(
        sizeof...(Modules) > 0, "name the shader module(s) this push struct is written for: PushConstantLayoutMatchesAll<PushT, Shaders::Modules::X>()"
    );
    static_assert((DeclaresPushBlock<Modules> || ...), "none of the named shader modules declares a push-constant block: the bytes would be written for no one");
    return (PushConstantLayoutMatchesOne<CppPush, Modules>() && ...);
}

// `PushData` with the contract in it: the caller names the module(s) whose bytes
// read the struct, so a push site cannot hand a module a struct it does not
// declare, or nothing at all. Sibling of `PushHeapIndex`/`PushHeapFrameAddresses`,
// which write the blob's other half.
template <ShaderProgram... Modules, typename T>
void PushHeapData(const Context& ctx, VkCommandBuffer cmd, const T& value) noexcept {
    static_assert(sizeof...(Modules) > 0, "name the shader module(s) this push struct is written for: PushHeapData<Shaders::Modules::X>(...)");
    static_assert(
        PushConstantLayoutMatchesAll<T, Modules...>(),
        "the push struct is not the push-constant block the named shader module(s) declare: same members, same offsets, same sizes, or it is not the same struct"
    );
    PushData(ctx, cmd, 0, value);
}

// The checks

namespace TemplatedDetail {

// A name a write spells that no module in the set declares. Declared and never
// defined on purpose: instantiating it is the diagnostic, and it carries the
// offending name into the compiler's own words.
template <typename Set, typename Half, ZHLN::StringLiteral Name>
struct UndeclaredBinding;

// A binding a module declares that the write does not spell: the forgotten
// argument. The diagnostic names the module and binding number -- a transient
// block has no previous frame's descriptor to fall back on, so the shader would
// read a stale one.
template <typename Set, typename Half, typename Program, uint32_t Binding>
struct UnspelledBinding;

// Complete exactly when `Spelled` -- the `false` specialization deliberately
// does not exist, so reaching it is the diagnostic.
template <typename Set, typename Half, typename Program, uint32_t Binding, bool Spelled>
struct DeclarationSpelledBy;
template <typename Set, typename Half, typename Program, uint32_t Binding>
struct DeclarationSpelledBy<Set, Half, Program, Binding, true> {};

// True when this slot was written through `Unread`: the pass holds the binding but
// the module does not read it (Slang strips an unreferenced parameter). A write may
// name those, and the check must not read them as misspellings.
template <typename Slot>
[[nodiscard]] consteval auto IsUnreadSlot() noexcept -> bool {
    if constexpr (requires { Slot::unread; }) {
        return Slot::unread;
    } else {
        return false;
    }
}

// True when one module declares `Slot`, or when the write marked it `Unread`.
template <typename Half, typename Program, typename Slot>
[[nodiscard]] consteval auto SlotIsDeclared() noexcept -> bool {
    if constexpr (IsUnreadSlot<Slot>()) {
        return true;
    } else {
        return DeclaredList<Half, Program>::Declares(Slot::name);
    }
}

// Bitmask of which of a write's slots the module declares: one membership test per
// slot per module, no walking of bytes.
template <typename Half, typename Program, typename... Slots>
[[nodiscard]] consteval auto DeclaredSlotMask() noexcept -> uint64_t {
    uint64_t mask  = 0;
    uint32_t index = 0;
    ((mask |= SlotIsDeclared<Half, Program, Slots>() ? (uint64_t {1} << index) : uint64_t {0}, ++index), ...);
    return mask;
}

// Complete for a slot whose bit is set; the diagnostic for the one whose is not.
template <typename Set, typename Half, uint64_t Mask, typename... Slots, size_t... Index>
consteval void RequireDeclaredBits(std::index_sequence<Index...>) {
    (static_cast<void>(sizeof(std::conditional_t<
                           ((Mask >> Index) & 1u) != 0,
                           std::true_type,
                           UndeclaredBinding<Set, Half, Slots...[Index]::literal>
                       >)), ...);
}

// True when one of the write's slots names this declared binding.
template <typename DeclaredSlot, typename... Slots>
[[nodiscard]] consteval auto SpellsDeclaredSlot() noexcept -> bool {
    return ((Slots::name == DeclaredSlot::name) || ...) || (IsUnreadSlot<Slots>() || ...);
}

// One instantiation per binding the module declares: complete when the write
// spells it.
template <typename Set, typename Half, typename Program, typename... Slots, size_t... Index>
consteval void RequireSpelledBindings(std::index_sequence<Index...>) {
    using List = DeclaredList<Half, Program>;
    [&]<typename... Declared>(std::type_identity<SlotsOfT<List>>) {
        (static_cast<void>(sizeof(DeclarationSpelledBy<
                               Set,
                               Half,
                               Program,
                               Declared...[Index]::binding,
                               SpellsDeclaredSlot<Declared...[Index], Slots...>()
                           >)), ...);
    }(std::type_identity<SlotsOfT<List>> {});
}

// One program's half of the cover check: true when it declares no binding of
// `Half` that the write's slots leave unspoken.
template <typename Set, typename Half, ShaderProgram Program, typename... Slots>
[[nodiscard]] consteval auto SpellsEveryDeclaration() -> bool {
    constexpr size_t declared = DeclaredList<Half, Program>::count;
    RequireSpelledBindings<Set, Half, Program, Slots...>(std::make_index_sequence<declared> {});
    return true;
}

// The checks that need the declaration itself. Matching names is one thing, the
// value under a name another: the declared type is a constant of the declaration,
// not of the write. So this header walks its own lists and calls
// `Check::Holds<DeclaredSlot, WriteSlot>()`, while the header that knows what shape
// of value a write can carry (HeapBindings.hpp) states the check.

// One declaration held to `Check`, when the write slot names it. An unspoken
// declaration is the cover check's business and stays out of this fold as true.
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
    return [&]<typename... Declared>(std::type_identity<SlotsOfT<List>>) {
        return (DeclaredSlotHoldsCheck<Check, Declared...[Index], WriteSlot>() && ...);
    }(std::type_identity<SlotsOfT<List>> {});
}

// One module's declaration of `Half` for the name this write slot spells, held to
// `Check`. An `Unread` slot names a binding the module does not declare, so there
// is no declaration to hold it to.
template <typename Check, typename Half, typename Program, typename WriteSlot>
[[nodiscard]] consteval auto ModuleSatisfiesCheck() noexcept -> bool {
    if constexpr (IsUnreadSlot<WriteSlot>()) {
        return true;
    } else {
        using List = DeclaredList<Half, Program>;
        return CheckDeclaredSlotsAt<Check, List, WriteSlot>(std::make_index_sequence<List::count> {});
    }
}

// Every slot of one write, against one module: the fold a set runs per program.
template <typename Check, typename Half, typename Program, typename... Slots>
[[nodiscard]] consteval auto ModuleSatisfiesChecks() noexcept -> bool {
    return (ModuleSatisfiesCheck<Check, Half, Program, Slots>() && ...);
}

} // namespace TemplatedDetail

// The programs one descriptor block serves, as a type. A pass is not one module:
// lighting.slang compiles as RT and NoRT, SMAA.slang as EDGE, WEIGHT and BLEND, and
// one call site writes the block all of them read.
template <ShaderProgram... Programs>
struct ShaderSet {
    static constexpr uint32_t programCount = sizeof...(Programs);

    // True when some module of the set declares a binding of `Half` named `name`.
    template <typename Half>
    [[nodiscard]] static consteval auto Declares([[maybe_unused]] std::string_view name) -> bool {
        return (DeclaredList<Half, Programs>::Declares(name) || ...);
    }

    // Every name `Slots...` spell is declared by some module of the set: the typo
    // direction. The compiler names the failing slot.
    template <typename Half, typename... Slots>
    [[nodiscard]] static consteval auto SpellsDeclaredNames() -> bool {
        constexpr uint64_t mask = (TemplatedDetail::DeclaredSlotMask<Half, Programs, Slots...>() | ...);
        TemplatedDetail::RequireDeclaredBits<ShaderSet, Half, mask, Slots...>(std::index_sequence_for<Slots...> {});
        return true;
    }

    // Every binding the set's modules declare is spelled by `Slots...`: the
    // forgotten-argument direction. Per module, not per union -- whichever
    // configuration runs, its bindings are written.
    template <typename Half, typename... Slots>
    [[nodiscard]] static consteval auto DeclarationsAreSpelled() -> bool {
        return (TemplatedDetail::SpellsEveryDeclaration<ShaderSet, Half, Programs, Slots...>() && ...);
    }

    // Every module's declaration for each name a write spells, held to `Check` --
    // the hook for a check that needs the declaration itself (its descriptor type),
    // not only its name. `Check` provides
    // `template <DeclaredSlot, WriteSlot> static consteval auto Holds() -> bool`;
    // HeapBindings.hpp supplies the one that knows what a write can carry.
    template <typename Half, typename Check, typename... Slots>
    [[nodiscard]] static consteval auto DeclarationsHold() -> bool {
        return (TemplatedDetail::ModuleSatisfiesChecks<Check, Half, Programs, Slots...>() && ...);
    }

    // Every module of the set that declares a push block declares *this* one.
    // Modules without one are skipped, which lets a set (SMAA's stages, a bloom
    // step) name the payload where the pass has one; a set whose modules declare
    // *different* blocks (the bakes) fails here, correctly.
    template <typename CppPush>
    [[nodiscard]] static consteval auto PushLayoutMatches() -> bool {
        return (PushConstantLayoutMatchesOne<CppPush, Programs>() && ...);
    }

    // The set's modules as one dispatch needs them: a compute chain holds the
    // pass's declaration and each step dispatches through it, so the expansion from
    // "the set" to "the modules the entry point names" happens here.
    template <typename Pass, typename... Args>
    static void DispatchHeapIndexed(Pass& pass, Args&&... args) noexcept {
        pass.template DispatchHeapIndexedThreads<Programs...>(std::forward<Args>(args)...);
    }
};

// A set of shader programs: what the gates below take, and what a write hands them.
template <typename T>
concept ShaderProgramSet = requires {
    { T::programCount } -> std::convertible_to<uint32_t>;
    { T::template Declares<ResourceBindings>(std::string_view {}) } -> std::same_as<bool>;
};

// True when every name `Slots...` spell is a binding some module of `Set` declares;
// a name that is not is a typo, and the compiler says which.
template <typename Set, typename Half, typename... Slots>
[[nodiscard]] consteval auto NamesAreDeclared() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template SpellsDeclaredNames<Half, Slots...>();
}

// True when every binding the set's modules declare is spelled by `Slots...`: the
// forgotten-argument direction.
template <typename Set, typename Half, typename... Slots>
[[nodiscard]] consteval auto NamesCoverDeclarations() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template DeclarationsAreSpelled<Half, Slots...>();
}

// True when no two arguments name the same binding: a repeat is a second write over
// the first, and only the last survives.
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

// True when every declared binding a write names satisfies `Check`: the descriptor
// type the module declares against the value the write carries. The two gates above
// say which names are wrong; this one says whether what sits under a right name has
// the right shape.
template <typename Set, typename Half, typename Check, typename... Slots>
[[nodiscard]] consteval auto DeclarationsSatisfy() noexcept -> bool {
    static_assert(ShaderProgramSet<Set>, "a descriptor write names a set of shader programs (ShaderProgram.hpp): Vk::ShaderSet<...>");
    return Set::template DeclarationsHold<Half, Check, Slots...>();
}

// What a program hands the pipeline

// The pipeline stage description for a program: its own bytes and its own entry
// point, so the module the checks ran against is the one that gets loaded.
template <ShaderProgram Program>
[[nodiscard]] auto CreateShaderDesc() noexcept -> ZHLN_ShaderDesc {
    const std::span<const uint8_t> bytes = Program::Bytes();
    // A catalog states the entry point as a literal, the concept also allows a
    // string_view; both are null-terminated, which is what the loader takes.
    const std::string_view entryPoint = Program::EntryPoint;
    return ZHLN_ShaderDesc {
        .code        = std::bit_cast<const uint32_t*>(bytes.data()),
        .size        = bytes.size_bytes(),
        .entry_point = entryPoint.data(),
    };
}

// The stage a program was compiled for, as the module declared it.
template <ShaderProgram Program>
[[nodiscard]] consteval auto StageOf() noexcept -> VkShaderStageFlagBits {
    return Program::Stage;
}

} // namespace ZHLN::Vk
