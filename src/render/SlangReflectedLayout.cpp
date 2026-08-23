// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SlangReflectedLayout.hpp"
#include "ReflectedLayout.hpp"
#include "ShaderStages.hpp"
#include <limits>
#include <slang-com-ptr.h>
#include <slang.h>

namespace ZHLN::Vk {

namespace {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
constexpr char kDescriptorHeapLayoutSource[] = {
#embed "../../resources/shaders/descriptor_heap_layout.slang"
    , '\0'
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

constexpr std::array<const char*, kHeapFrameAddressCount> kFrameAddressFieldNames = {
    "frameAddress", "lightsAddress", "instancesAddress", "jointsAddress", "previousJointsAddress", "morphDeltasAddress",
};

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

auto ReflectHeapPushDataLayout() noexcept -> std::optional<HeapPushDataLayout> {
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef()))) {
        return std::nullopt;
    }

    const slang::TargetDesc target = {
        .format  = SLANG_SPIRV,
        .profile = globalSession->findProfile("spirv_1_5"),
    };
    const slang::SessionDesc sessionDesc = {
        .targets                 = &target,
        .targetCount             = 1,
        .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
    };

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef()))) {
        return std::nullopt;
    }

    Slang::ComPtr<slang::IBlob> diagnostics;
    Slang::ComPtr<slang::IModule> module;
    module = session->loadModuleFromSourceString(
        "descriptor_heap_layout", "descriptor_heap_layout.slang", kDescriptorHeapLayoutSource, diagnostics.writeRef()
    );
    if (!module) {
        return std::nullopt;
    }

    auto* programLayout = module->getLayout(0, diagnostics.writeRef());
    if (programLayout == nullptr) {
        return std::nullopt;
    }

    auto* type = programLayout->findTypeByName("DescriptorHeapPushData");
    if (type == nullptr) {
        return std::nullopt;
    }

    auto* typeLayout = session->getTypeLayout(type, 0, slang::LayoutRules::Default, diagnostics.writeRef());
    if (typeLayout == nullptr) {
        return std::nullopt;
    }

    const auto field_offset = [typeLayout](const char* name) -> std::optional<uint32_t> {
        const SlangInt fieldIndex = typeLayout->findFieldIndexByName(name);
        if (fieldIndex < 0) {
            return std::nullopt;
        }
        auto* field = typeLayout->getFieldByIndex(static_cast<unsigned>(fieldIndex));
        if (field == nullptr) {
            return std::nullopt;
        }
        const size_t offset = field->getOffset(slang::ParameterCategory::Uniform);
        if (offset > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(offset);
    };

    HeapPushDataLayout result;
    for (size_t i = 0; i < kFrameAddressFieldNames.size(); ++i) {
        auto offset = field_offset(kFrameAddressFieldNames[i]);
        if (!offset || (*offset % alignof(uint64_t)) != 0) {
            return std::nullopt;
        }
        result.frameAddressOffsets[i] = *offset;
        if (i > 0 && result.frameAddressOffsets[i] < result.frameAddressOffsets[i - 1] + sizeof(uint64_t)) {
            return std::nullopt;
        }
    }

    auto heapIndexOffset = field_offset("heapIndex");
    if (!heapIndexOffset || (*heapIndexOffset % alignof(uint32_t)) != 0 ||
        *heapIndexOffset < result.frameAddressOffsets.back() + sizeof(uint64_t) ||
        *heapIndexOffset > std::numeric_limits<uint32_t>::max() - sizeof(uint32_t)) {
        return std::nullopt;
    }
    result.heapIndexOffset = *heapIndexOffset;
    result.requiredSize    = result.heapIndexOffset + sizeof(uint32_t);
    return result;
}

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
