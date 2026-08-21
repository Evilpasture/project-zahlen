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
    // VK_EXT_mesh_shader: task/mesh declare the very same `scene` parameter
    // block as the vertex stage, so they must contribute their stage flags to
    // the reflected bindless layout or the heap mapping table would advertise
    // the resources as vertex-only.
    auto task_spv = shaders.GetTaskSpv();
    auto mesh_spv = shaders.GetMeshSpv();
    if (!vert_spv.empty()) {
        builder.AddStageUnsafe({.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_VERTEX_BIT);
    }
    if (!task_spv.empty()) {
        builder.AddStageUnsafe({.code = task_spv.data(), .size = task_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_TASK_BIT_EXT);
    }
    if (!mesh_spv.empty()) {
        builder.AddStageUnsafe({.code = mesh_spv.data(), .size = mesh_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_MESH_BIT_EXT);
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
