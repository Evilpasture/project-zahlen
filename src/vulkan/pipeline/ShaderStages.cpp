// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderStages.hpp"
#include <algorithm>
#include <utility>

namespace ZHLN::Vk {

auto ShaderStagesView::Create(const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag)
    -> std::expected<ShaderStagesView, ZHLN::ErrorCode> {
    if (vert.code == nullptr || vert.size == 0) {
        return std::unexpected(ShaderStageCreationError::VertexShaderEmpty);
    }
    const ZHLN_ShaderStagesDesc desc = {.vert = vert, .frag = frag, .task = {}, .mesh = {}};
    ZHLN_ShaderStages stages {};
    if (!ZHLN_InitShaderStages(&desc, &stages)) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }
    return ShaderStagesView {stages};
}

auto ShaderStagesView::CreateMesh(const ZHLN_ShaderDesc& task, const ZHLN_ShaderDesc& mesh, const ZHLN_ShaderDesc& frag)
    -> std::expected<ShaderStagesView, ZHLN::ErrorCode> {
    if (mesh.code == nullptr || mesh.size == 0) {
        return std::unexpected(ShaderStageCreationError::VertexShaderEmpty);
    }
    const ZHLN_ShaderStagesDesc desc = {.vert = {}, .frag = frag, .task = task, .mesh = mesh};
    ZHLN_ShaderStages stages {};
    if (!ZHLN_InitShaderStages(&desc, &stages)) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }
    return ShaderStagesView {stages};
}

OwnedShaderStages::OwnedShaderStages(
    ShaderBytecode vert, ShaderBytecode frag, ShaderBytecode task, ShaderBytecode mesh, const ShaderStagesView& validated
) noexcept:
    _vert(std::move(vert)), _frag(std::move(frag)), _task(std::move(task)), _mesh(std::move(mesh)),
    _vertMeta(MetadataOf(validated.Get()->vert)), _fragMeta(MetadataOf(validated.Get()->frag)),
    _taskMeta(MetadataOf(validated.Get()->task)), _meshMeta(MetadataOf(validated.Get()->mesh)) {
}

auto OwnedShaderStages::MetadataOf(const ZHLN_Shader& shader) noexcept -> StageMetadata {
    StageMetadata meta {.stage = shader.stage, .viewMask = shader.view_mask};
    std::copy_n(shader.entry_point, meta.entryPoint.size(), meta.entryPoint.begin());
    return meta;
}

auto OwnedShaderStages::MakeStage(const ShaderBytecode& source, const StageMetadata& meta) noexcept -> ZHLN_Shader {
    if (meta.stage == VkShaderStageFlagBits {}) {
        return {};
    }
    const auto desc = CreateShaderDesc(source.Code());
    ZHLN_Shader shader {.code = desc.code, .size = desc.size, .stage = meta.stage, .entry_point = {}, .view_mask = meta.viewMask};
    std::copy(meta.entryPoint.begin(), meta.entryPoint.end(), shader.entry_point);
    return shader;
}

auto OwnedShaderStages::Create(ShaderBytecode vert, ShaderBytecode frag, const char* vertEntry, const char* fragEntry)
    -> std::expected<OwnedShaderStages, ZHLN::ErrorCode> {
    // Validate while the source spans still refer to their original buffers.
    // Retain only the C ABI's entry names, stage flags and view masks; never
    // retain its SPIR-V pointers across a move of these buffers.
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
    ZHLN_ShaderStages stages {};
    stages.vert = MakeStage(_vert, _vertMeta);
    stages.frag = MakeStage(_frag, _fragMeta);
    stages.task = MakeStage(_task, _taskMeta);
    stages.mesh = MakeStage(_mesh, _meshMeta);
    return ShaderStagesView {stages};
}

}
