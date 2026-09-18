// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/vulkan/pipeline/ShaderProgram.hpp
//
// A shader module that knows itself at compile time, and the checks a descriptor
// write runs against it.
//
// Everything a pass needs to know about a module -- which descriptors it
// declares, of which kind, under which names -- is in the module's own bytes,
// and those bytes are available to a constant expression: `#embed` on the cooked
// SPIR-V, walked by the consteval reader in SpirvBindings.hpp. So a module is a
// type, and one type is both halves of the interface at once:
//
//   * `Vk::ShaderSet<...>` names the programs one descriptor block serves. A
//     write hands the set to `HeapManager::WriteHeapParameters` /
//     `InitHeapPassSamplers` and every slot it spells is checked against the
//     modules themselves: a name no module declares is a compile error that
//     prints the name, a binding a module declares and the write forgets is a
//     compile error that prints the module and the binding number. There is no
//     table to keep in step with the shaders, because the shaders are the table;
//
//   * the same program type is what the pipeline is built from --
//     `Vk::CreateShaderDesc<Program>()` hands the module's own bytes and entry
//     point to the stage, so the module that was checked is the module that gets
//     loaded, byte for byte, and not a second registration of it.
//
// A program states two things about itself: `Bytes()` (the cooked SPIR-V, the
// `#embed`ded array) and `Path` (the source a hot reload would reread). Nothing
// else: the entry point, the stage and the descriptors are all read back out of
// `Bytes()`, so none of them can be stated wrongly here.
//
// A program's bindings are read once per translation unit (`kDeclarations`), so
// a write site pays for the modules it names and nothing else.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "SpirvBindings.hpp" // SpirvBindings: the reader these checks are built on

#include <Zahlen/Core/Description.hpp> // StringLiteral: a binding name is a template argument

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

// ============================================================================
// A module, as a type
// ============================================================================

/// Which half of a descriptor block a name belongs to, and therefore which write
/// path fills it. A sampler is a static sampler-heap slot filled once
/// (InitHeapPassSamplers); everything else is a transient resource-heap slot
/// filled per write (WriteHeapParameters). Checked apart because the two halves
/// have different shapes: a sampler spelled at a resource write would be skipped
/// at run time and only a runtime assertion would notice.
enum class BindingKind : uint8_t {
    Resource,
    Sampler,
};

/// One compiled shader module, known at compile time.
///
/// Satisfied by a type that states the two things only the build knows: its bytes
/// (the `#embed`ded cooked SPIR-V) and the source path a hot reload would reread.
/// Everything else about the module -- the stage it was compiled for, the entry
/// point a pipeline calls it as, every binding it declares -- is read back out of
/// those bytes, so a program holds no second copy of anything its shader already
/// says and cannot drift from it. `Bytes()` is `constexpr` rather than `consteval`
/// because the pipeline builder wants the same bytes at run time; it is the same
/// function in both places, which is the point.
template <typename T>
concept ShaderProgram = requires {
    { T::Path } -> std::convertible_to<const char*>;
    { T::Bytes() } -> std::same_as<std::span<const uint8_t>>;
};

/// What a program declares, read from the bytes it embeds.
///
/// The module a write is checked against and the module a pipeline loads are the
/// same object, so this is not a transcription of a shader's interface: it is the
/// interface.
template <ShaderProgram Program>
[[nodiscard]] consteval auto Declarations() -> SpirvBindings {
    return SpirvBindings::Parse(Program::Bytes(), 0);
}

/// True when the embedded bytes are a module this reader can read.
///
/// A parse that stopped early -- bad magic, a truncated instruction, a name that
/// never terminates -- proves nothing, so the checks refuse to run on it rather
/// than passing it: a short parse passing a check is the one failure mode that
/// would make a check worse than no check at all.
template <ShaderProgram Program>
[[nodiscard]] consteval auto IsReadable() -> bool {
    return Declarations<Program>().Complete();
}

namespace TemplatedDetail {

/// A program's declarations, read once per translation unit: what a write of
/// this TU is checked against, and the reason two write sites naming the same
/// module do not walk its bytes twice.
template <ShaderProgram Program>
inline constexpr SpirvBindings kDeclarations = Declarations<Program>();

/// True when the declarations hold a binding of `kind` named `name`.
template <BindingKind Kind>
[[nodiscard]] consteval auto DeclaresBinding(const SpirvBindings& declarations, std::string_view name) noexcept -> bool {
    if constexpr (Kind == BindingKind::Sampler) {
        return declarations.DeclaresSampler(name);
    } else {
        return declarations.DeclaresResource(name);
    }
}

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

/// A module compiled for a stage this engine does not build pipelines for. The
/// instantiation names the program and the execution model it declares.
template <ShaderProgram Program, uint32_t ExecutionModel>
struct UnknownExecutionModel;

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
/// the check must not read the deliberate ones as misspellings. What it can
/// still tell apart is a name a module declares (spelled plainly) and a name no
/// module declares (either `Unread`, or a typo the compiler reports).
template <typename Slot>
[[nodiscard]] consteval auto IsUnreadSlot() noexcept -> bool {
    if constexpr (requires { Slot::unread; }) {
        return Slot::unread;
    } else {
        return false;
    }
}

/// Whatever a write's slots are, this bitmask says which of them the module
/// declares: one walk of the module per program, not one per slot.
template <ShaderProgram Program, BindingKind Kind, typename... Slots>
[[nodiscard]] consteval auto DeclaredSlotMask() noexcept -> uint64_t {
    constexpr SpirvBindings declarations = TemplatedDetail::kDeclarations<Program>;
    if (!declarations.Complete()) {
        return 0;
    }
    uint64_t mask  = 0;
    uint32_t index = 0;
    ((mask |= (IsUnreadSlot<Slots>() || DeclaresBinding<Kind>(declarations, Slots::name)) ? (uint64_t {1} << index) : uint64_t {0}, ++index), ...);
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

/// True when one of `Slots` names this binding -- and true for a binding of the
/// other kind, which the write of that kind checks.
template <BindingKind Kind, typename... Slots>
[[nodiscard]] consteval auto SpellsBinding(const SpirvBindings& declarations, uint32_t ordinal) noexcept -> bool {
    const SpirvBinding& binding = declarations[ordinal];
    if (binding.sampler != (Kind == BindingKind::Sampler)) {
        return true;
    }
    const std::span<const uint8_t> bytes = declarations.Bytes();
    return ((binding.IsNamed(bytes, Slots::name)) || ...) || ((IsUnreadSlot<Slots>()) || ...);
}

/// One instantiation per binding the module declares: complete when the write
/// spells it (or when it belongs to the other kind's half, checked there).
template <typename Set, BindingKind Kind, typename Program, typename... Slots, size_t... Index>
consteval void RequireSpelledBindings(std::index_sequence<Index...>) {
    constexpr SpirvBindings declarations = TemplatedDetail::kDeclarations<Program>;
    (static_cast<void>(sizeof(DeclarationSpelledBy<
                           Set,
                           Kind,
                           Program,
                           declarations[Index].binding,
                           SpellsBinding<Kind, Slots...>(declarations, static_cast<uint32_t>(Index))
                       >)), ...);
}

/// One program's half of the cover check: true when it declares no binding of
/// `Kind` that `Slots...` leave unspoken.
template <typename Set, BindingKind Kind, ShaderProgram Program, typename... Slots>
[[nodiscard]] consteval auto SpellsEveryDeclaration() -> bool {
    constexpr SpirvBindings declarations = kDeclarations<Program>;
    if (!declarations.Complete()) {
        return false;
    }
    RequireSpelledBindings<Set, Kind, Program, Slots...>(std::make_index_sequence<declarations.Count()> {});
    return true;
}

} // namespace TemplatedDetail

/// The pipeline stage description for a program: its own bytes, and the entry
/// point its own OpEntryPoint names. The module that was checked is the module
/// that gets loaded.
template <ShaderProgram Program>
[[nodiscard]] auto CreateShaderDesc() noexcept -> ZHLN_ShaderDesc {
    const std::span<const uint8_t> bytes = Program::Bytes();
    constexpr SpirvBindings        declarations = TemplatedDetail::kDeclarations<Program>;
    static_assert(declarations.Complete(), "a shader program's bytes are not a module this reader can read (ShaderProgram.hpp)");
    static_assert(declarations.EntryPointCount() == 1, "a shader program is one entry point (ShaderProgram.hpp)");
    return ZHLN_ShaderDesc {
        .code = std::bit_cast<const uint32_t*>(bytes.data()),
        .size = bytes.size_bytes(),
        // The entry point is a null-terminated literal inside the module, so the
        // address of its first byte is the string the loader wants.
        .entry_point = reinterpret_cast<const char*>(bytes.data() + declarations.EntryPointOffset()),
    };
}

/// The execution model a module declares, as the Vulkan stage it was compiled
/// for. A model this engine does not build for is a compile error naming the
/// module: a stage nobody planned for is not something to discover at pipeline
/// creation.
template <ShaderProgram Program>
[[nodiscard]] consteval auto StageOf() -> VkShaderStageFlagBits {
    constexpr SpirvBindings declarations = TemplatedDetail::kDeclarations<Program>;
    static_assert(declarations.Complete(), "a shader program's bytes are not a module this reader can read (ShaderProgram.hpp)");
    static_assert(declarations.EntryPointCount() == 1, "a shader program is one entry point (ShaderProgram.hpp)");
    constexpr uint32_t model = declarations.ExecutionModel();
    // SPIR-V execution models, as numbers: this header maps them and no other
    // header has to know them (0 Vertex, 4 Fragment, 5 GLCompute, 5364 TaskEXT,
    // 5365 MeshEXT).
    if constexpr (model == 0) {
        return VK_SHADER_STAGE_VERTEX_BIT;
    } else if constexpr (model == 4) {
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    } else if constexpr (model == 5) {
        return VK_SHADER_STAGE_COMPUTE_BIT;
    } else if constexpr (model == 5364) {
        return VK_SHADER_STAGE_TASK_BIT_EXT;
    } else if constexpr (model == 5365) {
        return VK_SHADER_STAGE_MESH_BIT_EXT;
    } else {
        static_cast<void>(sizeof(TemplatedDetail::UnknownExecutionModel<Program, model>));
        return VK_SHADER_STAGE_ALL;
    }
}

/// The entry point a program declares, as a byte range into its own bytes. The
/// bytes are not null-terminated for a caller that wants a string_view; what a
/// caller needs is either the address (CreateShaderDesc) or a comparison
/// (SpirvBindings::IsEntryPoint).
template <ShaderProgram Program>
[[nodiscard]] consteval auto EntryPointOf() -> std::span<const uint8_t> {
    constexpr SpirvBindings declarations = TemplatedDetail::kDeclarations<Program>;
    static_assert(declarations.Complete(), "a shader program's bytes are not a module this reader can read (ShaderProgram.hpp)");
    static_assert(declarations.EntryPointCount() == 1, "a shader program is one entry point (ShaderProgram.hpp)");
    return Program::Bytes().subspan(declarations.EntryPointOffset(), declarations.EntryPointLength());
}

// ============================================================================
// The programs of one pass
// ============================================================================


/// The programs one descriptor block serves, as a type.
///
/// A pass is not one module: lighting.slang compiles as RT and NoRT, SMAA.slang
/// as EDGE, WEIGHT and BLEND, and one call site writes the block all of them
/// read. The set is what makes that explicit -- and what makes a declaration
/// unnecessary, because the checks below ask the modules themselves.
template <ShaderProgram... Programs>
struct ShaderSet {
    static constexpr uint32_t programCount = sizeof...(Programs);

    /// True when every module in the set parses as a module.
    [[nodiscard]] static consteval auto Readable() -> bool {
        return (IsReadable<Programs>() && ...);
    }

    /// True when some module declares a binding of `kind` named `name`.
    template <BindingKind Kind>
    [[nodiscard]] static consteval auto Declares(std::string_view name) -> bool {
        return (TemplatedDetail::DeclaresBinding<Kind>(TemplatedDetail::kDeclarations<Programs>, name) || ...);
    }

    /// Every name `Slots...` spell is declared by some module of the set: the
    /// typo direction. The failing slot is the one the compiler names.
    template <BindingKind Kind, typename... Slots>
    [[nodiscard]] static consteval auto SpellsDeclaredNames() -> bool {
        constexpr uint64_t mask = (TemplatedDetail::DeclaredSlotMask<Programs, Kind, Slots...>() | ...);
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
};

/// A set of shader programs: what the gates below take, and what a descriptor
/// write hands them.
template <typename T>
concept ShaderProgramSet = requires {
    T::programCount;
    T::Readable();
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

} // namespace ZHLN::Vk
