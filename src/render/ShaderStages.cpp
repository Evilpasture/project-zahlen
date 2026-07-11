// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderStages.hpp"
#include <format>
#include <fstream>

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
) noexcept -> std::expected<ShaderStages, std::string> {
    auto load = [](const std::filesystem::path& path) -> std::expected<std::vector<uint32_t>, std::string> {
        if (path.empty()) {
            return std::vector<uint32_t> {};
        }
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected("Could not open file: " + path.string());
        }
        const std::streamsize size = file.tellg();
        if (size % 4 != 0) {
            return std::unexpected("File size is not a multiple of 4 bytes: " + path.string());
        }
        std::vector<uint32_t> buffer(size / sizeof(uint32_t));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        return buffer;
    };

    auto vert_res = load(vertPath);
    if (!vert_res) {
        return std::unexpected("Failed to load vertex shader: " + vert_res.error());
    }
    auto vert_spv = std::move(vert_res.value());

    std::vector<uint32_t> frag_spv;
    if (!fragPath.empty()) {
        auto frag_res = load(fragPath);
        if (!frag_res) {
            return std::unexpected("Failed to load fragment shader: " + frag_res.error());
        }
        frag_spv = std::move(frag_res.value());
    }

    if (vert_spv.empty()) {
        return std::unexpected("Vertex shader code is empty: " + vertPath.string());
    }

    const ZHLN_ShaderDesc v_desc = {.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = vertEntry};
    const ZHLN_ShaderDesc f_desc = {.code = frag_spv.data(), .size = frag_spv.size() * 4, .entry_point = fragEntry};

    return Create(device, v_desc, f_desc).transform_error([&](const std::string& err) {
        return std::format("Failed to create ShaderStages from files '{}' and '{}': {}", vertPath.string(), fragPath.string(), err);
    });
}

auto ShaderStages::Create(const VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) noexcept
    -> std::expected<ShaderStages, std::string> {
    const ZHLN_ShaderStagesDesc desc = {.device = device, .vert = vert, .frag = frag};
    ZHLN_ShaderStages           stages {};
    if (!ZHLN_CreateShaderStages(&desc, &stages)) {
        if (vert.code && vert.size > 0 && stages.vert.handle == VK_NULL_HANDLE) {
            return std::unexpected("Failed to compile or load Vertex Shader Module.");
        }
        if (frag.code && frag.size > 0 && stages.frag.handle == VK_NULL_HANDLE) {
            return std::unexpected("Failed to compile or load Fragment Shader Module.");
        }
        return std::unexpected("Failed to create shader stages (unknown driver compilation error).");
    }
    stages.vert.view_mask = ZHLN_DetectShaderViewMask(&vert);
    stages.frag.view_mask = ZHLN_DetectShaderViewMask(&frag);
    return ShaderStages {device, stages};
}
} // namespace ZHLN::Vk
