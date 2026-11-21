// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// File: utils/zshader/Reflect.cpp
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
// guess: the catalog is the only record of what the modules declare, so an
// unnamed kind would reach the engine as a slot nobody can write.
//
// Reflection is a property of the build, and the build requires it (see
// cmake/ReflectionConfig.cmake), so the checks below guard against an
// enumerator that genuinely has no name rather than against a compiler mode.

#include "ZShader.hpp"

#include "SlangReflect.hpp"

#include <Zahlen/Core/Reflection/Enums.hpp>

#include <vulkan/vulkan_core.h> // the enumerators the generated header spells, read here and never called

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::ZShader {

namespace {

// The answer EnumToString gives for a value that names no enumerator. Every
// check below is built on this sentinel.
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
    // A value that certainly has a name, checked once up front: one sentence
    // about the build instead of one wrong name per module.
    const std::string_view name = ZHLN::Reflect::EnumToString(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    return !name.empty() && name != kUnnamedEnumerator;
}

namespace {

void PopulateModuleFromInfo(Module& module, SlangEntryInfo info) {
    // The bytes and the declarations come from one linked program.
    module.spirv      = std::move(info.spirv);
    module.entryPoint = std::move(info.entryPoint);
    module.stage      = StageName(module.path, StageFor(module.path, module.entryPoint, info.stage));
    module.localSize  = info.localSize;
    for (SlangBinding& binding : info.bindings) {
        const VkDescriptorType type = DescriptorTypeFor(module.path, module.entryPoint, binding);
        Descriptor             descriptor {
            .name    = std::move(binding.name),
            .type    = DescriptorTypeName(module.path, type),
            .set     = binding.set,
            .binding = binding.binding,
        };
        (type == VK_DESCRIPTOR_TYPE_SAMPLER ? module.samplers : module.resources).push_back(std::move(descriptor));
    }
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
                PushMember {.name = std::move(member.name), .offset = member.offset, .size = member.size}
            );
        }
        module.pushes.push_back(std::move(block));
    }

    module.reflected = true;
}

} // namespace

void ReflectModule(Module& module, const SlangSource& source, std::span<const std::string> searchPaths) {
    const SlangCompileArgs args {
        .file        = source.path,
        .entry       = source.entry,
        .stage       = ParseSlangStage(source.path, source.entry, source.stage),
        .searchPaths = searchPaths,
        .defines     = source.defines,
    };
    PopulateModuleFromInfo(module, ReflectCatalogModule(args));
}

void ReflectAllModules(
    std::span<Module>                         modules,
    const std::map<std::string, SlangSource>& slangSources,
    std::span<const std::string>              searchPaths
) {
    std::vector<SlangBatchRequest> requests;
    requests.reserve(modules.size());
    for (const Module& module : modules) {
        const auto found = slangSources.find(module.macro);
        if (found == slangSources.end()) {
            Fail("--bytes {} has no --slang-source; the catalog compiles every module from source", module.macro);
        }
        const SlangSource& source = found->second;
        requests.push_back(SlangBatchRequest {
            .file    = source.path,
            .entry   = source.entry,
            .stage   = ParseSlangStage(source.path, source.entry, source.stage),
            .defines = source.defines,
        });
    }

    std::vector<SlangEntryInfo> infos = ReflectCatalogBatch(requests, searchPaths);
    for (size_t i = 0; i < modules.size(); ++i) {
        PopulateModuleFromInfo(modules[i], std::move(infos[i]));
    }
}

} // namespace ZHLN::ZShader
