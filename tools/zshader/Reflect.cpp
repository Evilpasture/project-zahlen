// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: tools/zshader/Reflect.cpp
//
// SPIR-V in, the catalog's model out. The tool reflects with SPIRV-Reflect --
// the same library the renderer reflects with at pipeline creation -- and names
// what it finds with static reflection over the Vulkan enumerators themselves,
// so the strings the generated header spells are the ones the engine's headers
// declare and there is no second list to drift from them.
//
// The names are read out of the enumeration rather than a switch because the
// tool is a tool: ZHLN::Reflect::EnumToString is the project's one answer to
// "what is this enumerator called", and the enumeration it is asked about here
// is Vulkan's, whose enumerators and SPIRV-Reflect's mirror each other value for
// value (SPV_REFLECT_DESCRIPTOR_TYPE_* against VK_DESCRIPTOR_TYPE_*, and the
// same for the stages), so the cast is a rename and not a translation.
//
// Reflection is a property of the build: a compiler without it compiles the
// engine's stand-ins, which name every value "Unknown". Rather than generate a
// catalog full of sentinels, NamesEnumerators() is asked once, up front, and the
// tool refuses to write anything -- see the guard in RunCommandLine. The
// configuration that makes a reflection-free build work is the same one the
// engine uses: zahlen_transpile_sources, which rewrites the EnumToString calls
// below into the switch they stand in for.

#include "ZShader.hpp"

#include <Zahlen/Core/Reflection/Enums.hpp>

#include <vulkan/vulkan_core.h> // the enumerators the generated header spells, read here and never called

#include "spirv_reflect.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace ZHLN::ZShader {

namespace {

/// The answer EnumToString gives for a value that names no enumerator: in a
/// reflection-capable build because there is no such enumerator, and in a
/// transpiled build because the switch the transpiler writes in its place ends
/// in the same default. Both ways of asking give the tool the same sentinel,
/// which is what every check below is built on.
constexpr std::string_view kUnnamedEnumerator = "Unknown";

/// The descriptor type as the engine spells it, or a build error: a descriptor
/// whose value the enumeration does not name is one the engine has no heap for.
auto DescriptorTypeName(std::string_view path, SpvReflectDescriptorType type) -> std::string_view {
    const std::string_view name = ZHLN::Reflect::EnumToString(static_cast<VkDescriptorType>(type));
    if (name.empty() || name == kUnnamedEnumerator) {
        Fail("{} declares descriptor type {}, which the engine has no heap for", path, static_cast<int>(type));
    }
    return name;
}

/// The stage the module was compiled for, or a build error: a stage this engine
/// builds no pipeline for is not something the catalog can carry.
auto StageName(std::string_view path, SpvReflectShaderStageFlagBits stage) -> std::string_view {
    const std::string_view name = ZHLN::Reflect::EnumToString(static_cast<VkShaderStageFlagBits>(stage));
    if (name.empty() || name == kUnnamedEnumerator) {
        Fail("{} is compiled for a stage this engine does not build pipelines for", path);
    }
    return name;
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

void ReflectModule(Module& module) {
    const std::vector<uint8_t> bytes = ReadFile(module.path);
    module.byteSize                  = static_cast<uint32_t>(bytes.size());

    SpvReflectShaderModule reflected {};
    const SpvReflectResult result = spvReflectCreateShaderModule(bytes.size(), bytes.data(), &reflected);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        Fail("SPIRV-Reflect could not read {} (result {})", module.path, static_cast<int>(result));
    }

    if (reflected.entry_point_count != 1) {
        const uint32_t count = reflected.entry_point_count;
        spvReflectDestroyShaderModule(&reflected);
        Fail("{} declares {} entry points; the catalog's modules declare exactly one", module.path, count);
    }
    const SpvReflectEntryPoint& entry = reflected.entry_points[0];
    if (entry.name != nullptr) {
        module.entryPoint = entry.name;
    }
    module.stage = StageName(module.path, entry.shader_stage);

    for (const SpvReflectDescriptorBinding& binding: std::span(reflected.descriptor_bindings, reflected.descriptor_binding_count)) {
        const std::string_view type = DescriptorTypeName(module.path, binding.descriptor_type);
        if (binding.name == nullptr || binding.name[0] == '\0') {
            spvReflectDestroyShaderModule(&reflected);
            Fail("{} declares an unnamed descriptor; a write has nothing to match it by", module.path);
        }
        Descriptor descriptor {
            .name    = binding.name,
            .type    = type,
            .set     = binding.set,
            .binding = binding.binding,
        };
        // A sampler is a sampler because the module says so, not because of
        // where it sits: the two halves are written by different heaps.
        (binding.descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER ? module.samplers : module.resources).push_back(std::move(descriptor));
    }
    // The iterator spelling rather than std::ranges: this is the one file in the
    // tool that includes the reflection headers, and their <meta> is what makes
    // the ranges header a hazard next to them. A plain sort says the same thing.
    const auto byBinding = [](const Descriptor& a, const Descriptor& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    };
    std::sort(module.resources.begin(), module.resources.end(), byBinding);
    std::sort(module.samplers.begin(), module.samplers.end(), byBinding);

    for (const SpvReflectBlockVariable& block: std::span(reflected.push_constant_blocks, reflected.push_constant_block_count)) {
        PushBlock push;
        push.paddedSize = block.padded_size;
        for (const SpvReflectBlockVariable& member: std::span(block.members, block.member_count)) {
            push.members.push_back(PushMember {
                .name   = member.name == nullptr ? std::string {} : std::string {member.name},
                .offset = member.offset,
                .size   = member.size,
            });
        }
        module.pushes.push_back(std::move(push));
    }

    spvReflectDestroyShaderModule(&reflected);
    module.reflected = true;
}

} // namespace ZHLN::ZShader
