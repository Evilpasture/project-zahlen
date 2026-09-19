// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/GpuAbi.hpp
//
// The GPU ABI, held against the module that declares it.
//
// Every struct in <Zahlen/Types.hpp>'s `GPUTypes` is the host half of a
// contract the shader half writes down in resources/shaders/gpu_abi.slang:
// `GpuAbiTypes` mirrors each leaf of `GPUTypes`, one
// `ConstantBuffer<GpuAbiTypes> abi` declares it, and the module's compiled
// bytes are what says whether the two still agree.
//
// The question used to be answered at engine boot (SPIRV-Reflect, in
// src/vulkan/pipeline/TypeLayout.cpp) and then at build time by a registry of
// assertions in a file of its own -- a list a new struct could be added
// without, reading the module on the side. It is asked here now, once per
// translation unit that includes this header: the walk visits every group of
// `GPUTypes` and every leaf in it through the reflection the structs already
// carry, so a struct nobody remembered to list is checked too, and a size the
// shader does not agree with fails the build in the renderer's own sources.
//
// Why here, and not next to the structs in <Zahlen/Types.hpp>: because the
// check needs two things the engine's public headers must not have --
//
//   * the cooked module's path, which is a build fact of this target
//     (`ZHLN_GPU_ABI_MODULE`, see src/render/CMakeLists.txt). `#embed` takes a
//     file name, and a preprocessor cannot read a string constant, so the
//     declaration has to reach the compiler as a macro. The embed makes the
//     cook a build dependency of the sources that include this header, so a
//     stale module is recompiled against rather than checked for;
//
//   * the reader, `Vk::SpirvTypes::Parse` (pipeline/SpirvLayout.hpp), which is
//     an RHI implementation header: reachable only from the one target allowed
//     to carry src/vulkan on its include path, which is this one. A public
//     header that included it would make every consumer of `Zahlen/Types.hpp`
//     -- physics, the GUI, the asset tools, the tests -- depend on a layer they
//     cannot see, and would have to be told to skip it when that path is
//     absent: a check that can compile itself away is not a check.
//
// So the dependency points the way the layers do: this header includes
// <Zahlen/Types.hpp>, not the other way round. It is included by
// RenderInternal.hpp, which every render source compiles, so the failure lands
// in the renderer's own translation units and not in a file someone reads on
// the side.
//
// The module is embedded here and nowhere else. It is not a pass: nothing
// builds a pipeline from it, so the generated catalog carries no declarations
// of it to hold against its bytes, and ShaderBytecode.cpp does not embed it.
// One copy of the bytes, in the translation units that check them.

#pragma once
#include "Rendering.hpp"

#include "pipeline/SpirvLayout.hpp" // Vk::SpirvTypes, Vk::kHeapPushDataLayout

#include <Zahlen/Core/Reflection/Annotations.hpp> // Reflect::AnnotatedName
#include <Zahlen/Core/Reflection/Class.hpp>       // Reflect::ForEachNestedType
#include <Zahlen/Types.hpp>                       // GPUTypes

#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN::GpuAbi {

/// gpu_abi.slang's cooked module, and the type table its bytes parse to. One
/// parse per translation unit, read by every assertion below -- and read by the
/// offline layout checks over the project's cooked shaders too, which is where
/// these numbers were held against SPIRV-Reflect before they were checked here.
inline constexpr uint8_t kModuleBytes[] = {
#embed ZHLN_GPU_ABI_MODULE
};
inline constexpr Vk::SpirvTypes kTypes = Vk::SpirvTypes::Parse(std::span<const uint8_t>(kModuleBytes));

/// True when the module declares `name` and means `sizeof(T)` by it.
///
/// A name the module does not declare is a failure, not a skip: it means the
/// shader renamed the struct or moved it to another module, which is exactly the
/// drift this header exists to catch. `ambiguous` is the other way a lookup can
/// be unsure -- two declarations the same plain name answers to.
template <typename T>
[[nodiscard]] consteval auto Matches(std::string_view name) noexcept -> bool {
    const Vk::SpirvTypeLookup found = kTypes.LookupStruct(name);
    return found.found && !found.ambiguous && found.size == sizeof(T);
}

/// The walk: every group of `Root`, then every leaf in it -- the shape the
/// runtime loop had, with the comparison lifted into a `static_assert` so the
/// compiler, not the device, is what finds out. The inner assertion is the
/// diagnostic: it names the leaf that drifted.
template <typename Root>
[[nodiscard]] consteval auto CheckGpuAbiTypes() noexcept -> bool {
    Reflect::ForEachNestedType<Root>([]<typename Group>() {
        Reflect::ForEachNestedType<Group>([]<typename T>() {
            static_assert(
                Matches<T>(Reflect::AnnotatedName<T>()),
                "a GPU type does not have the size its .slang declaration compiles to: the struct and the shader have drifted, and every buffer, push "
                "blob or descriptor write through it would land misaligned"
            );
        });
    });
    return true;
}

} // namespace ZHLN::GpuAbi

// A reader that stopped early would prove nothing, so the parse has to have
// completed before anything below is believed.
static_assert(ZHLN::GpuAbi::kTypes.Complete(), "the GPU ABI module did not parse: the checks below would prove nothing");

// The call the walk needs to run at all -- a consteval function is not
// evaluated by being defined.
static_assert(ZHLN::GpuAbi::CheckGpuAbiTypes<ZHLN::GPUTypes>(), "the GPU ABI type walk did not complete");

// The heap push-data layout: six frame addresses, then the descriptor index.
// `Vk::kHeapPushDataLayout` is what the heap writer and every pass push read;
// this is what the shader says. They have to be the same struct.
static_assert(
    ZHLN::Vk::HeapPushDataMatchesShader(ZHLN::GpuAbi::kTypes),
    "DescriptorHeapPushData in gpu_abi.slang no longer matches the frame-address/heap-index layout the engine writes into the push-data blob"
);
