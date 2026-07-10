// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderStages.hpp"
#include <fstream>
#include <print>

namespace ZHLN::Vk {
ShaderStages::~ShaderStages() noexcept {
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
    const VkDevice               device,
    const std::filesystem::path& vertPath,
    const std::filesystem::path& fragPath,
    const char*                  vertEntry,
    const char*                  fragEntry
) noexcept -> ShaderStages {
    auto load = [](const std::filesystem::path& path) -> std::vector<uint32_t> {
        if (path.empty()) {
            return {};
        }
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            return {};
        }
        const std::streamsize size = file.tellg();
        std::vector<uint32_t> buffer(size / sizeof(uint32_t));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        return buffer;
    };

    auto vert_spv = load(vertPath);
    auto frag_spv = load(fragPath);

    if (vert_spv.empty() || (!fragPath.empty() && frag_spv.empty())) {
        std::println(stderr, "[ZHLN::Vk] Failed to load shader files: {} or {}", vertPath.string(), fragPath.string());
        return {};
    }

    const ZHLN_ShaderDesc v_desc = {.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = vertEntry};
    const ZHLN_ShaderDesc f_desc = {.code = frag_spv.data(), .size = frag_spv.size() * 4, .entry_point = fragEntry};

    return Create(device, v_desc, f_desc);
}

auto ShaderStages::Create(const VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) noexcept -> ShaderStages {
    const ZHLN_ShaderStagesDesc desc = {.device = device, .vert = vert, .frag = frag};
    ZHLN_ShaderStages           stages {};
    if (!ZHLN_CreateShaderStages(&desc, &stages)) {
        return {};
    }
    stages.vert.view_mask = ZHLN_DetectShaderViewMask(&vert);
    stages.frag.view_mask = ZHLN_DetectShaderViewMask(&frag);
    return {device, stages};
}
} // namespace ZHLN::Vk
