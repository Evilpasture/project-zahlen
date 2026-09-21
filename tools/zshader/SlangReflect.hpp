// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: tools/zshader/SlangReflect.hpp
//
// Slang in, catalog records out: the one place the tool compiles Slang and
// reads the reflection back. The gpu-types mode compiles through it
// (CompileSlangEntry) and walks the layout itself; the catalog mode takes
// the whole answer (ReflectCatalogModule) and only names what it finds
// (Reflect.cpp). Nothing here reads SPIR-V: every name, type, offset, size
// and slot comes from libslang's own reflection and metadata APIs.
//
// The one thing a layout walk cannot say, and its replacement: ProgramLayout
// is the compiler's declaration model -- every global the source declares,
// whether the entry point references it or not. The emitter drops what the
// entry never touches (lighting.slang's blueNoise pair vanishes from the NoRT
// variant; a vertex stage keeps four of a ParameterBlock's twelve members).
// The filter is Slang's own answer to that question: the entry point's usage
// table (slang::IMetadata::isParameterLocationUsed, what slangc's
// -reflection-json prints as `"used"`) says which (set, binding) slots the
// emitted entry point binds, and it is computed by the compiler on the same
// linked program the walk reads. Where the table and the emitter disagree --
// slang@095a18a collects the table before the SPIR-V emitter's last pass, so
// a parameter fed only to legalized-away code stays "used" while its binding
// is not emitted (basic_mesh.slang's forward entry and scene.g_prevJoints) --
// the catalog keeps the table's word: an extra entry names a heap slot the
// scene declares anyway, and the engine's ModuleMatchesBytes check holds the
// direction that matters to the runtime (every binding the bytes declare is
// in the catalog) as a compile-time assert.
//
// Slots: Slang's per-variable (set, binding) for ParameterBlock members
// arrives block-relative and collides with the globals'; the walk composes
// them the Vulkan layout does -- a member's set is the slot its block took
// globally, its binding is its own index plus the block's register-space
// offset -- and the composed pair is both the usage query and what the
// catalog writes. On this tree the composition matches the cooked modules'
// bytes for every binding they carry.

#pragma once

#include <slang-com-ptr.h> // Slang::ComPtr
#include <slang.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::ZShader {

// One descriptor binding Slang reported for the entry point: the name, the
// resource kind/shape/access the catalog maps to a VkDescriptorType
// (Reflect.cpp), and the composed (set, binding) the usage table answered
// "used" for. ParameterBlock members arrive flattened with dotted names
// (scene.frame); bindless arrays arrive as the single binding they are.
struct SlangBinding {
    std::string                  name {};
    uint32_t                     set = 0;
    uint32_t                     binding = 0;
    slang::TypeReflection::Kind  kind {};
    SlangResourceShape           shape  = SLANG_RESOURCE_NONE;
    SlangResourceAccess          access = SLANG_RESOURCE_ACCESS_NONE;
};

// One push-constant member: the name Slang knows it by and where it sits. A
// null member name arrives empty.
struct SlangPushMember {
    std::string name {};
    uint32_t    offset = 0;
    uint32_t    size   = 0;
};

// One push-constant block: the global's name, the top-level members, and the
// tight extent (the furthest member end) the catalog's PushSize is. Slang
// seats the struct 16-rounded; the extent is what the push data spans. A
// module can declare several (common.slang carries one every importer also
// sees). The usage table cannot filter push blocks -- Slang's metadata stops
// tracking at the push category (slang-artifact-associated-impl:
// isUsageTracked) -- so every declared block is listed; the bytes are never
// consulted for push, and the engine holds push layout against its own
// hand-written structs instead (PushDataLayout.hpp).
struct SlangPushBlock {
    std::string                  name {};
    std::vector<SlangPushMember> members {};
    uint32_t                     extent = 0;
};

// Everything the catalog needs from one entry point: the name and stage plus
// the descriptor bindings the usage table kept and every declared push
// block. ReflectCatalogModule is the only producer.
struct SlangEntryInfo {
    std::string                   entryPoint {};
    SlangStage                    stage = SLANG_STAGE_NONE;
    std::vector<SlangBinding>     bindings {};
    std::vector<SlangPushBlock>   pushes {};
};

// How to compile one entry point: exactly one of the file or the module
// names the source, the entry and stage select it, and the search dirs and
// defines resolve it. The codegen levels default to the cooks'
// (no debug info, no -O flag); the gpu-types mode sets both explicitly.
struct SlangCompileArgs {
    std::string_view file {};   // catalog modules: read from this path
    std::string_view module {}; // gpu_abi: resolved through the search paths
    std::string_view entry {};
    SlangStage       stage = SLANG_STAGE_NONE;
    std::span<const std::string>                          searchPaths {};
    std::span<const std::pair<std::string, std::string>>  defines {}; // (name, value)
    int                debugInfoLevel   = SLANG_DEBUG_INFO_LEVEL_NONE;
    std::optional<int> optimizationLevel {}; // empty = the cooks' default (no -O flag)
};

// One compiled entry point: the linked component plus every Slang object it
// dangles without. The session, the composite and the entry stay alive here;
// returning the linked component alone reads freed memory on the first call
// through it. The module is borrowed, never held: the session owns loaded
// modules (it caches them by name), and releasing one double-frees at
// session teardown.
struct SlangProgram {
    Slang::ComPtr<slang::IGlobalSession> global;
    Slang::ComPtr<slang::ISession>       session;
    Slang::ComPtr<slang::IEntryPoint>    entry;
    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IComponentType> linked;
};

// The --slang-source stage word as Slang spells it, or a build error naming
// the module: vertex, fragment, compute, amplification, mesh.
auto ParseSlangStage(std::string_view file, std::string_view entry, std::string_view stage) -> SlangStage;

// Compiles one entry point to a linked program, warning-free or not at all:
// any Slang diagnostic fails naming the source, because a module the
// compiler complained about is not one the catalog can describe.
auto CompileSlangEntry(const SlangCompileArgs& args) -> SlangProgram;

// Walks the linked entry into catalog records: the entry name/stage, every
// descriptor binding the entry point's usage table marks used (ParameterBlocks
// flattened with composed slots, bindless arrays as one binding,
// specialization-constant scalars skipped), and every [vk::push_constant]
// block by name with its tight extent. A program Slang refuses metadata for
// fails: without the usage table there is no honest survivor set, and an
// unfiltered walk would describe a module the cook never emitted. Entry-point
// parameters are varyings, payloads and stage outputs, never bindings (the
// tree declares no uniform entry params); anything else fails naming it.
auto ReflectSlangEntry(slang::IComponentType* linked, std::string_view file, std::string_view entry) -> SlangEntryInfo;

// Compile + reflect for one catalog module: the linked program's records,
// exactly as Slang's layout and usage table describe them. No bytes -- the
// cooked file is the engine's proof, not the tool's input (the catalog mode
// sizes the module from the file; the check lives in CatalogChecks.hpp).
auto ReflectCatalogModule(const SlangCompileArgs& args) -> SlangEntryInfo;

} // namespace ZHLN::ZShader
