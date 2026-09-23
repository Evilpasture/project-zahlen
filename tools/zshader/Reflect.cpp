// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: tools/zshader/Reflect.cpp
//
// Slang in, the catalog's model out. The tool compiles every module from its
// Slang source in-process (SlangReflect.cpp), with the cooks' flags, and
// names what it finds with static reflection over the Vulkan enumerators
// themselves, so the strings the generated header spells are the ones the
// engine's headers declare and there is no second list to drift from them.
//
// The Slang kinds arrive as Slang spells them; the switches below are what
// turn them into the descriptor types and stages the engine builds pipelines
// for. Anything they cannot place is a build error naming the binding, not a
// guess: the generated header holds every kind against the module's own
// bytes (ModuleMatchesBytes), so a guess would fail there instead of here.
//
// Reflection is a property of the build: a compiler without it compiles the
// engine's stand-ins, which name every value "Unknown". Rather than generate a
// catalog full of sentinels, NamesEnumerators() is asked once, up front, and the
// tool refuses to write anything -- see the guard in RunCommandLine. The
// configuration that makes a reflection-free build work is the same one the
// engine uses: zahlen_transpile_sources, which rewrites the EnumToString calls
// below into the switch they stand in for.

#include "ZShader.hpp"

#include "SlangReflect.hpp"

#include <Zahlen/Core/Reflection/Enums.hpp>

#include <vulkan/vulkan_core.h> // the enumerators the generated header spells, read here and never called

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::ZShader {

namespace {

// The answer EnumToString gives for a value that names no enumerator: in a
// reflection-capable build because there is no such enumerator, and in a
// transpiled build because the switch the transpiler writes in its place ends
// in the same default. Both ways of asking give the tool the same sentinel,
// which is what every check below is built on.
constexpr std::string_view kUnnamedEnumerator = "Unknown";

// The descriptor type as the engine spells it, or a build error: a descriptor
// whose value the enumeration does not name is one the engine has no heap for.
auto DescriptorTypeName(std::string_view path, VkDescriptorType type) -> std::string_view {
    const std::string_view name = ZHLN::Reflect::EnumToString(type);
    if (name.empty() || name == kUnnamedEnumerator) {
        Fail("{} declares descriptor type {}, which the engine has no heap for", path, static_cast<int>(type));
    }
    return name;
}

// The stage the module was compiled for, or a build error: a stage this engine
// builds no pipeline for is not something the catalog can carry.
auto StageName(std::string_view path, VkShaderStageFlagBits stage) -> std::string_view {
    const std::string_view name = ZHLN::Reflect::EnumToString(stage);
    if (name.empty() || name == kUnnamedEnumerator) {
        Fail("{} is compiled for a stage this engine does not build pipelines for", path);
    }
    return name;
}

// A binding the descriptor switch cannot place: the message carries the Slang
// triple, so the fix is one proven arm, not archaeology.
[[noreturn]] void FailUnmappedBinding(
    std::string_view path, std::string_view entry, const SlangBinding& binding
) {
    Fail(
        "'{}' entry '{}': binding '{}' has Slang kind {}, shape {} and access {}; extend DescriptorTypeFor to place it",
        path, entry, binding.name, static_cast<int>(binding.kind), static_cast<int>(binding.shape),
        static_cast<int>(binding.access)
    );
}

// Slang's (kind, shape, access) as the Vulkan descriptor type it emits: image
// shapes read sampled and written stored (the array/multisample/shadow flags
// select the image, not the type, so the base mask takes them off); buffer
// shapes are storage, the only descriptor type Vulkan gives a buffer; texel
// buffers split by access; acceleration structures and subpass inputs have the
// one type each. Proven binding by binding by the gate's TUP table (216 of
// them); anything outside it fails naming the triple.
auto DescriptorTypeFor(std::string_view path, std::string_view entry, const SlangBinding& binding)
    -> VkDescriptorType {
    using enum slang::TypeReflection::Kind;
    switch (binding.kind) {
    case SamplerState:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    case ConstantBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case Resource:
        break;
    default:
        FailUnmappedBinding(path, entry, binding);
    }
    // A combined image-sampler is neither a sampled image nor a sampler on its
    // own, so it is not placed as one: the tree declares none (the gate's TUP
    // table holds no combined shape), and one that appears fails naming the
    // triple rather than emitting a descriptor type Vulkan would refuse.
    if ((binding.shape & SLANG_TEXTURE_COMBINED_FLAG) != 0) {
        FailUnmappedBinding(path, entry, binding);
    }
    const SlangResourceShape baseShape = static_cast<SlangResourceShape>(binding.shape & SLANG_RESOURCE_BASE_SHAPE_MASK);
    switch (baseShape) {
    case SLANG_TEXTURE_1D:
    case SLANG_TEXTURE_2D:
    case SLANG_TEXTURE_3D:
    case SLANG_TEXTURE_CUBE:
        switch (binding.access) {
        case SLANG_RESOURCE_ACCESS_READ:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case SLANG_RESOURCE_ACCESS_READ_WRITE:
        case SLANG_RESOURCE_ACCESS_WRITE:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        default:
            FailUnmappedBinding(path, entry, binding);
        }
    case SLANG_STRUCTURED_BUFFER:
    case SLANG_BYTE_ADDRESS_BUFFER:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case SLANG_TEXTURE_BUFFER:
        switch (binding.access) {
        case SLANG_RESOURCE_ACCESS_READ:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case SLANG_RESOURCE_ACCESS_READ_WRITE:
        case SLANG_RESOURCE_ACCESS_WRITE:
            return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        default:
            FailUnmappedBinding(path, entry, binding);
        }
    case SLANG_ACCELERATION_STRUCTURE:
        if (binding.access == SLANG_RESOURCE_ACCESS_READ) {
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        }
        FailUnmappedBinding(path, entry, binding);
    case SLANG_TEXTURE_SUBPASS:
        return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    default:
        FailUnmappedBinding(path, entry, binding);
    }
}

// Slang's stage as the Vulkan stage bit it emits. The tree exercises five;
// the rest is the same one-to-one correspondence, spelled out so a future
// tessellation or ray-tracing stage lands placed instead of failing.
auto StageFor(std::string_view path, std::string_view entry, SlangStage stage) -> VkShaderStageFlagBits {
    switch (stage) {
    case SLANG_STAGE_VERTEX:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case SLANG_STAGE_HULL:
        return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case SLANG_STAGE_DOMAIN:
        return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case SLANG_STAGE_GEOMETRY:
        return VK_SHADER_STAGE_GEOMETRY_BIT;
    case SLANG_STAGE_FRAGMENT:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case SLANG_STAGE_COMPUTE:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    case SLANG_STAGE_RAY_GENERATION:
        return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case SLANG_STAGE_INTERSECTION:
        return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    case SLANG_STAGE_ANY_HIT:
        return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    case SLANG_STAGE_CLOSEST_HIT:
        return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case SLANG_STAGE_MISS:
        return VK_SHADER_STAGE_MISS_BIT_KHR;
    case SLANG_STAGE_CALLABLE:
        return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    case SLANG_STAGE_MESH:
        return VK_SHADER_STAGE_MESH_BIT_EXT;
    case SLANG_STAGE_AMPLIFICATION:
        return VK_SHADER_STAGE_TASK_BIT_EXT;
    default:
        Fail(
            "'{}' entry '{}': Slang stage {} is not a stage this engine builds pipelines for", path, entry,
            static_cast<int>(stage)
        );
    }
}

} // namespace

auto NamesEnumerators() -> bool {
    // A value that certainly has a name: the stand-ins answer "Unknown" for
    // every value there is, so this is false exactly when the tool was compiled
    // without the reflection helpers -- reflection off and not transpiled -- and
    // every name it would generate is that sentinel. Asking once, up front, is
    // what turns that into one sentence about the build instead of one lie per
    // module about its descriptors.
    const std::string_view name = ZHLN::Reflect::EnumToString(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    return !name.empty() && name != kUnnamedEnumerator;
}

void ReflectModule(Module& module, const SlangSource& source, const std::vector<std::string>& searchPaths) {
    const std::vector<uint8_t> bytes = ReadFile(module.path);
    module.byteSize                  = static_cast<uint32_t>(bytes.size());

    const SlangCompileArgs args{
        .file        = source.path,
        .entry       = source.entry,
        .stage       = ParseSlangStage(source.path, source.entry, source.stage),
        .searchPaths = searchPaths,
        .defines     = source.defines,
    };
    SlangEntryInfo info = ReflectCatalogModule(args);
    module.entryPoint   = std::move(info.entryPoint);
    module.stage        = StageName(module.path, StageFor(module.path, module.entryPoint, info.stage));
    // No unnamed check: the walk fails on an unnamed binding before one can
    // arrive here, as the old reader did on the spot.
    for (SlangBinding& binding : info.bindings) {
        const VkDescriptorType type = DescriptorTypeFor(module.path, module.entryPoint, binding);
        Descriptor             descriptor{
                        .name    = std::move(binding.name),
                        .type    = DescriptorTypeName(module.path, type),
                        .set     = binding.set,
                        .binding = binding.binding,
        };
        // A sampler is a sampler because the module says so, not because of
        // where it sits: the two halves are written by different heaps.
        (type == VK_DESCRIPTOR_TYPE_SAMPLER ? module.samplers : module.resources).push_back(std::move(descriptor));
    }
    // The iterator spelling rather than std::ranges: this is the one file in the
    // tool that includes the reflection headers, and their <meta> is what makes
    // the ranges header a hazard next to them. A plain sort says the same thing.
    const auto byBinding = [](const Descriptor& a, const Descriptor& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    };
    std::sort(module.resources.begin(), module.resources.end(), byBinding);
    std::sort(module.samplers.begin(), module.samplers.end(), byBinding);

    for (SlangPushBlock& push : info.pushes) {
        PushBlock block;
        block.paddedSize = push.extent;
        for (SlangPushMember& member : push.members) {
            block.members.push_back(
                PushMember{.name = std::move(member.name), .offset = member.offset, .size = member.size});
        }
        module.pushes.push_back(std::move(block));
    }

    module.reflected = true;
}

} // namespace ZHLN::ZShader
