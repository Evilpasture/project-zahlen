// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderStages.hpp"
#include <utility>

namespace ZHLN::Vk {

namespace {

constexpr size_t kVert = 0;
constexpr size_t kFrag = 1;
constexpr size_t kTask = 2;
constexpr size_t kMesh = 3;

[[nodiscard]] auto CopyCode(const ZHLN_ShaderDesc& desc) -> std::vector<uint32_t> {
    if (desc.code == nullptr) {
        return {};
    }
    return {desc.code, desc.code + desc.size / sizeof(uint32_t)};
}

}

void ShaderStages::RebindOwned() noexcept {
    const auto rebind = [](ZHLN_Shader& shader, const std::vector<uint32_t>& storage) {
        if (!storage.empty()) {
            shader.code = storage.data();
            shader.size = storage.size() * sizeof(uint32_t);
        }
    };
    rebind(_raw.vert, _ownedSpv[kVert]);
    rebind(_raw.frag, _ownedSpv[kFrag]);
    rebind(_raw.task, _ownedSpv[kTask]);
    rebind(_raw.mesh, _ownedSpv[kMesh]);
}

ShaderStages::ShaderStages(ShaderStages&& other) noexcept:
    _raw(std::exchange(other._raw, {})), _ownedSpv(std::move(other._ownedSpv)) {
    RebindOwned();
}

auto ShaderStages::operator=(ShaderStages&& other) noexcept -> ShaderStages& {
    if (this != &other) {
        _raw      = std::exchange(other._raw, {});
        _ownedSpv = std::move(other._ownedSpv);
        RebindOwned();
    }
    return *this;
}

auto ShaderStages::CreateBorrowed(const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag)
    -> std::expected<ShaderStages, ZHLN::ErrorCode> {
    if (vert.code == nullptr || vert.size == 0) {
        return std::unexpected(ShaderStageCreationError::VertexShaderEmpty);
    }
    const ZHLN_ShaderStagesDesc desc = {.vert = vert, .frag = frag};
    ZHLN_ShaderStages stages {};
    if (!ZHLN_InitShaderStages(&desc, &stages)) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }
    return ShaderStages {stages};
}

auto ShaderStages::Create(const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) -> std::expected<ShaderStages, ZHLN::ErrorCode> {
    auto result = CreateBorrowed(vert, frag);
    if (result) {
        result->_ownedSpv[kVert] = CopyCode(vert);
        result->_ownedSpv[kFrag] = CopyCode(frag);
        result->RebindOwned();
    }
    return result;
}

auto ShaderStages::CreateLoaded(
    const ZHLN_ShaderDesc& vert, std::vector<uint32_t> vertDisk, const ZHLN_ShaderDesc& frag, std::vector<uint32_t> fragDisk
) -> std::expected<ShaderStages, ZHLN::ErrorCode> {
    // The non-empty buffers must be the ones backing the provided descriptors;
    // otherwise rebinding after the move could point into unrelated SPIR-V.
    if ((!vertDisk.empty() && (vert.code != vertDisk.data() || vert.size != vertDisk.size() * sizeof(uint32_t))) ||
        (!fragDisk.empty() && (frag.code != fragDisk.data() || frag.size != fragDisk.size() * sizeof(uint32_t)))) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }
    auto result = CreateBorrowed(vert, frag);
    if (result) {
        result->_ownedSpv[kVert] = std::move(vertDisk);
        result->_ownedSpv[kFrag] = std::move(fragDisk);
        result->RebindOwned();
    }
    return result;
}

auto ShaderStages::CreateMeshBorrowed(const ZHLN_ShaderDesc& task, const ZHLN_ShaderDesc& mesh, const ZHLN_ShaderDesc& frag)
    -> std::expected<ShaderStages, ZHLN::ErrorCode> {
    if (mesh.code == nullptr || mesh.size == 0) {
        return std::unexpected(ShaderStageCreationError::VertexShaderEmpty);
    }
    const ZHLN_ShaderStagesDesc desc = {.frag = frag, .task = task, .mesh = mesh};
    ZHLN_ShaderStages stages {};
    if (!ZHLN_InitShaderStages(&desc, &stages)) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }
    return ShaderStages {stages};
}

auto ShaderStages::CreateMesh(const ZHLN_ShaderDesc& task, const ZHLN_ShaderDesc& mesh, const ZHLN_ShaderDesc& frag)
    -> std::expected<ShaderStages, ZHLN::ErrorCode> {
    auto result = CreateMeshBorrowed(task, mesh, frag);
    if (result) {
        result->_ownedSpv[kTask] = CopyCode(task);
        result->_ownedSpv[kMesh] = CopyCode(mesh);
        result->_ownedSpv[kFrag] = CopyCode(frag);
        result->RebindOwned();
    }
    return result;
}

}
