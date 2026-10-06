// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderStages.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <spirv_reflect.h>

namespace ZHLN::Vk {
namespace {

[[nodiscard]] auto IsAbsent(const ShaderDesc& desc) noexcept -> bool {
    return desc.code == nullptr && desc.size == 0;
}

[[nodiscard]] auto IsValid(const ShaderDesc& desc) noexcept -> bool {
    return desc.code != nullptr && desc.size != 0 && desc.size % sizeof(uint32_t) == 0;
}

[[nodiscard]] auto DetectViewMask(const ShaderDesc& desc) noexcept -> uint32_t {
    if (!IsValid(desc)) {
        return 0;
    }
    SpvReflectShaderModule module {};
    if (spvReflectCreateShaderModule(desc.size, desc.code, &module) != SPV_REFLECT_RESULT_SUCCESS) {
        return 0;
    }

    uint32_t viewMask = 0;
    for (uint32_t i = 0; i < module.input_variable_count; ++i) {
        if (module.input_variables[i]->built_in == SpvBuiltInViewIndex) {
            viewMask = 0x3FU;
            break;
        }
    }
    spvReflectDestroyShaderModule(&module);
    return viewMask;
}

[[nodiscard]] auto ReflectedEntryPoint(const ShaderDesc& desc) noexcept -> const char* {
    SpvReflectShaderModule module {};
    if (spvReflectCreateShaderModule(desc.size, desc.code, &module) != SPV_REFLECT_RESULT_SUCCESS) {
        return nullptr;
    }
    const char* name = module.entry_point_name;
    if ((name == nullptr || name[0] == '\0') && module.entry_point_count > 0) {
        name = module.entry_points[0].name;
    }
    // The SPIRV-Reflect-owned name is copied into ShaderStageData immediately,
    // before destroying the reflection module.
    static thread_local std::array<char, 64> nameCopy {};
    nameCopy.fill('\0');
    if (name != nullptr) {
        std::strncpy(nameCopy.data(), name, nameCopy.size() - 1);
    }
    spvReflectDestroyShaderModule(&module);
    return name != nullptr && nameCopy[0] != '\0' ? nameCopy.data() : nullptr;
}

[[nodiscard]] auto FallbackEntryPoint(const VkShaderStageFlagBits stage) noexcept -> const char* {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            return "VSMain";
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return "PSMain";
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            return "TaskMain";
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            return "MeshMain";
        default:
            return "main";
    }
}

[[nodiscard]] auto MakeStage(const ShaderDesc& desc, const VkShaderStageFlagBits stage) noexcept -> std::optional<ShaderStageData> {
    if (IsAbsent(desc)) {
        return ShaderStageData {};
    }
    if (!IsValid(desc)) {
        return std::nullopt;
    }

    ShaderStageData result {
        .code = desc.code,
        .size = desc.size,
        .stage = stage,
        .entry_point = {},
        .view_mask = DetectViewMask(desc),
    };

    const char* entry = desc.entry_point != nullptr && desc.entry_point[0] != '\0' ? desc.entry_point : ReflectedEntryPoint(desc);
    if (entry == nullptr || entry[0] == '\0') {
        entry = FallbackEntryPoint(stage);
    }
    std::strncpy(result.entry_point, entry, sizeof(result.entry_point) - 1);
    result.entry_point[sizeof(result.entry_point) - 1] = '\0';
    return result;
}

} // namespace

auto ShaderStagesView::Create(const ShaderDesc& vert, const ShaderDesc& frag) -> std::expected<ShaderStagesView, ZHLN::ErrorCode> {
    if (vert.code == nullptr || vert.size == 0) {
        return std::unexpected(ShaderStageCreationError::VertexShaderEmpty);
    }
    auto vertex = MakeStage(vert, VK_SHADER_STAGE_VERTEX_BIT);
    auto fragment = MakeStage(frag, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!vertex || !fragment) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }

    ShaderStages stages {};
    stages.vert = *vertex;
    stages.frag = *fragment;
    return ShaderStagesView {stages};
}

auto ShaderStagesView::CreateMesh(const ShaderDesc& task, const ShaderDesc& mesh, const ShaderDesc& frag)
    -> std::expected<ShaderStagesView, ZHLN::ErrorCode> {
    if (mesh.code == nullptr || mesh.size == 0) {
        return std::unexpected(ShaderStageCreationError::VertexShaderEmpty);
    }
    auto taskStage = MakeStage(task, VK_SHADER_STAGE_TASK_BIT_EXT);
    auto meshStage = MakeStage(mesh, VK_SHADER_STAGE_MESH_BIT_EXT);
    auto fragment = MakeStage(frag, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!taskStage || !meshStage || !fragment) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }

    ShaderStages stages {};
    stages.task = *taskStage;
    stages.mesh = *meshStage;
    stages.frag = *fragment;
    return ShaderStagesView {stages};
}

OwnedShaderStages::OwnedShaderStages(
    ShaderBytecode vert, ShaderBytecode frag, ShaderBytecode task, ShaderBytecode mesh, const ShaderStagesView& validated
) noexcept:
    _vert(std::move(vert)), _frag(std::move(frag)), _task(std::move(task)), _mesh(std::move(mesh)),
    _vertMeta(MetadataOf(validated.Get()->vert)), _fragMeta(MetadataOf(validated.Get()->frag)),
    _taskMeta(MetadataOf(validated.Get()->task)), _meshMeta(MetadataOf(validated.Get()->mesh)) {}

auto OwnedShaderStages::MetadataOf(const ShaderStageData& shader) noexcept -> StageMetadata {
    StageMetadata metadata {.stage = shader.stage, .viewMask = shader.view_mask};
    std::copy_n(shader.entry_point, metadata.entryPoint.size(), metadata.entryPoint.begin());
    return metadata;
}

auto OwnedShaderStages::MakeStage(const ShaderBytecode& source, const StageMetadata& metadata) noexcept -> ShaderStageData {
    if (metadata.stage == VkShaderStageFlagBits {}) {
        return {};
    }
    const ShaderDesc desc = CreateShaderDesc(source.Code());
    ShaderStageData shader {
        .code = desc.code,
        .size = desc.size,
        .stage = metadata.stage,
        .entry_point = {},
        .view_mask = metadata.viewMask,
    };
    std::copy(metadata.entryPoint.begin(), metadata.entryPoint.end(), shader.entry_point);
    shader.entry_point[sizeof(shader.entry_point) - 1] = '\0';
    return shader;
}

auto OwnedShaderStages::Create(ShaderBytecode vert, ShaderBytecode frag, const char* vertEntry, const char* fragEntry)
    -> std::expected<OwnedShaderStages, ZHLN::ErrorCode> {
    auto view = ShaderStagesView::Create(CreateShaderDesc(vert.Code(), vertEntry), CreateShaderDesc(frag.Code(), fragEntry));
    if (!view) {
        return std::unexpected(view.error());
    }
    return OwnedShaderStages {std::move(vert), std::move(frag), {}, {}, *view};
}

auto OwnedShaderStages::CreateMesh(
    ShaderBytecode task, ShaderBytecode mesh, ShaderBytecode frag, const char* taskEntry, const char* meshEntry, const char* fragEntry
) -> std::expected<OwnedShaderStages, ZHLN::ErrorCode> {
    auto view = ShaderStagesView::CreateMesh(
        CreateShaderDesc(task.Code(), taskEntry), CreateShaderDesc(mesh.Code(), meshEntry), CreateShaderDesc(frag.Code(), fragEntry)
    );
    if (!view) {
        return std::unexpected(view.error());
    }
    return OwnedShaderStages {{}, std::move(frag), std::move(task), std::move(mesh), *view};
}

auto OwnedShaderStages::View() const noexcept -> ShaderStagesView {
    ShaderStages stages {};
    stages.vert = MakeStage(_vert, _vertMeta);
    stages.frag = MakeStage(_frag, _fragMeta);
    stages.task = MakeStage(_task, _taskMeta);
    stages.mesh = MakeStage(_mesh, _meshMeta);
    return ShaderStagesView {stages};
}

} // namespace ZHLN::Vk
