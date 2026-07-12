// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderStages.hpp"
#include <fstream>

namespace ZHLN::Vk {

ShaderStages::~ShaderStages() {
    if (_device != VK_NULL_HANDLE) {
        ZHLN_DestroyShaderStages(_device, &_raw);
    }
}

ShaderStages::ShaderStages(ShaderStages&& other) noexcept: _device(std::exchange(other._device, VK_NULL_HANDLE)), _raw(std::exchange(other._raw, {})) {
}

auto ShaderStages::operator=(ShaderStages&& other) noexcept -> ShaderStages& {
    if (this != &other) {
        if (_device != VK_NULL_HANDLE) {
            ZHLN_DestroyShaderStages(_device, &_raw);
        }
        _device = std::exchange(other._device, VK_NULL_HANDLE);
        _raw    = std::exchange(other._raw, {});
    }
    return *this;
}

auto ShaderStages::FromFiles(
    VkDevice                     device,
    const std::filesystem::path& vertPath,
    const std::filesystem::path& fragPath,
    const char*                  vertEntry,
    const char*                  fragEntry
) -> std::expected<ShaderStages, ZHLN::Error> {
    auto load = [](const std::filesystem::path& path) -> std::expected<std::vector<uint32_t>, ZHLN::Error> {
        if (path.empty()) {
            return std::vector<uint32_t> {};
        }
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected(ShaderStageCreationError::FileOpenFailed);
        }
        const std::streamsize size = file.tellg();
        if (size % 4 != 0) {
            return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
        }
        std::vector<uint32_t> buffer(size / sizeof(uint32_t));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        return buffer;
    };

    auto vert_res = load(vertPath);
    if (!vert_res) {
        return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
    }
    auto vert_spv = std::move(vert_res.value());

    std::vector<uint32_t> frag_spv;
    if (!fragPath.empty()) {
        auto frag_res = load(fragPath);
        if (!frag_res) {
            return std::unexpected(ShaderStageCreationError::ShaderLoadingFailed);
        }
        frag_spv = std::move(frag_res.value());
    }

    if (vert_spv.empty()) {
        return std::unexpected(ShaderStageCreationError::VertexShaderEmpty);
    }

    const ZHLN_ShaderDesc v_desc = {.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = vertEntry};
    const ZHLN_ShaderDesc f_desc = {.code = frag_spv.data(), .size = frag_spv.size() * 4, .entry_point = fragEntry};

    return Create(device, v_desc, f_desc);
}

auto ShaderStages::Create(VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) -> std::expected<ShaderStages, ZHLN::Error> {
    const ZHLN_ShaderStagesDesc desc = {.device = device, .vert = vert, .frag = frag};
    ZHLN_ShaderStages           stages {};
    if (!ZHLN_CreateShaderStages(&desc, &stages)) {
        if (vert.code && vert.size > 0 && stages.vert.handle == VK_NULL_HANDLE) {
            return std::unexpected(ShaderStageCreationError::ShaderModuleCreationFailed);
        }
        if (frag.code && frag.size > 0 && stages.frag.handle == VK_NULL_HANDLE) {
            return std::unexpected(ShaderStageCreationError::ShaderModuleCreationFailed);
        }
        return std::unexpected(ShaderStageCreationError::ShaderModuleCreationFailed);
    }
    stages.vert.view_mask = ZHLN_DetectShaderViewMask(&vert);
    stages.frag.view_mask = ZHLN_DetectShaderViewMask(&frag);
    return ShaderStages {device, stages};
}

} // namespace ZHLN::Vk
