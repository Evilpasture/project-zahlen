// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SlangReflectedLayout.hpp"
#include "ReflectedLayout.hpp"
#include "ShaderStages.hpp"

namespace ZHLN::Vk {

namespace {

/// Copies the SPIRV-Reflect output into the Slang-typed binding structure.
void AdoptReflectedSets(const std::array<ReflectedSet, 4>& src, SlangReflectedLayout& dst) noexcept {
    for (size_t s = 0; s < 4; ++s) {
        dst.reflectedSets[s].bindings.clear();
        dst.reflectedSets[s].bindings.reserve(src[s].bindings.size());
        for (const auto& b: src[s].bindings) {
            dst.reflectedSets[s].bindings.push_back(
                {.binding         = b.binding,
                 .descriptorType  = b.descriptorType,
                 .descriptorCount = b.descriptorCount,
                 .stageFlags      = b.stageFlags,
                 .bindingFlags    = b.bindingFlags}
            );
        }
    }
}

} // namespace

bool SlangReflectedLayout::Build(VkDevice /*device*/, const ShaderStages& shaders) noexcept {
    UnsafeReflectedLayoutBuilder builder;
    auto                         vert_spv = shaders.GetVertSpv();
    auto                         frag_spv = shaders.GetFragSpv();
    if (!vert_spv.empty()) {
        builder.AddStageUnsafe({.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_VERTEX_BIT);
    }
    if (!frag_spv.empty()) {
        builder.AddStageUnsafe({.code = frag_spv.data(), .size = frag_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    std::array<ReflectedSet, 4> reflected {};
    if (!builder.BuildUnsafe(reflected)) {
        return false;
    }
    AdoptReflectedSets(reflected, *this);
    return true;
}

bool SlangReflectedLayout::Build(VkDevice /*device*/, const ZHLN_ShaderDesc& shader, VkShaderStageFlagBits stage) noexcept {
    UnsafeReflectedLayoutBuilder builder;
    builder.AddStageUnsafe(shader, stage);
    std::array<ReflectedSet, 4> reflected {};
    if (!builder.BuildUnsafe(reflected)) {
        return false;
    }
    AdoptReflectedSets(reflected, *this);
    return true;
}

bool SlangReflectedLayout::Build(VkDevice /*device*/, std::span<const ReflectedStageInput> stages) noexcept {
    UnsafeReflectedLayoutBuilder builder;
    for (const auto& s: stages) {
        builder.AddStageUnsafe(s.shader, s.stage);
    }
    std::array<ReflectedSet, 4> reflected {};
    if (!builder.BuildUnsafe(reflected)) {
        return false;
    }
    AdoptReflectedSets(reflected, *this);
    return true;
}

} // namespace ZHLN::Vk
