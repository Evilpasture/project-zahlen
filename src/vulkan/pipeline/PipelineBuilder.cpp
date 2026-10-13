// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PipelineBuilder.hpp"
#include "Rendering.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <spirv_reflect.h>

namespace ZHLN::Vk {

auto FormatHasStencil(const VkFormat format) noexcept -> bool {
    return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

auto ResolveComputeEntryPoint(const ShaderDesc& shader, const std::span<char, 64> name) noexcept -> const char* {
    std::ranges::fill(name, '\0');
    if (shader.entry_point != nullptr && shader.entry_point[0] != '\0') {
        std::strncpy(name.data(), shader.entry_point, name.size() - 1);
        return name.data();
    }

    SpvReflectShaderModule module {};
    if (spvReflectCreateShaderModule(shader.size, shader.code, &module) == SPV_REFLECT_RESULT_SUCCESS) {
        const char* entry = module.entry_point_name;
        if ((entry == nullptr || entry[0] == '\0') && module.entry_point_count > 0) {
            entry = module.entry_points[0].name;
        }
        if (entry != nullptr) {
            std::strncpy(name.data(), entry, name.size() - 1);
        }
        spvReflectDestroyShaderModule(&module);
    }
    if (name[0] == '\0') {
        std::strncpy(name.data(), "CSMain", name.size() - 1);
    }
    return name.data();
}

auto ValidShader(const ShaderDesc& shader) noexcept -> bool {
    return shader.code != nullptr && shader.size != 0 && shader.size % sizeof(uint32_t) == 0;
}

auto CreateGraphicsPipeline(const VkDevice device, const VkGraphicsPipelineCreateInfo& createInfo, const std::span<const VkPipelineBinaryKHR> binaries) noexcept
    -> std::expected<VkPipeline, ErrorCode> {
    VkGraphicsPipelineCreateInfo info = createInfo;
    VkPipelineBinaryInfoKHR      binaryInfo {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
        .pNext             = info.pNext,
        .binaryCount       = static_cast<uint32_t>(binaries.size()),
        .pPipelineBinaries = binaries.data(),
    };
    if (!binaries.empty()) {
        info.pNext = &binaryInfo;
    }

    VkPipeline     pipeline = VK_NULL_HANDLE;
    const VkResult result   = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        return std::unexpected(ToError(result));
    }
    return pipeline;
}

auto CreateComputePipeline(const VkDevice device, const VkComputePipelineCreateInfo& createInfo, const std::span<const VkPipelineBinaryKHR> binaries) noexcept
    -> std::expected<VkPipeline, ErrorCode> {
    VkComputePipelineCreateInfo info = createInfo;
    VkPipelineBinaryInfoKHR     binaryInfo {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
        .pNext             = info.pNext,
        .binaryCount       = static_cast<uint32_t>(binaries.size()),
        .pPipelineBinaries = binaries.data(),
    };
    if (!binaries.empty()) {
        info.pNext = &binaryInfo;
    }

    VkPipeline     pipeline = VK_NULL_HANDLE;
    const VkResult result   = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        return std::unexpected(ToError(result));
    }
    return pipeline;
}

auto ComputePipelineBuilder::Shader(const uint32_t* code, const size_t size, const char* entry) noexcept -> ComputePipelineBuilder& {
    _shader = CreateShaderDesc(code, size, entry);
    return *this;
}

auto ComputePipelineBuilder::Shader(const ShaderDesc& desc) noexcept -> ComputePipelineBuilder& {
    _shader = desc;
    return *this;
}

auto ComputePipelineBuilder::Specialization(const VkSpecializationInfo* info) noexcept -> ComputePipelineBuilder& {
    _specialization_info = info;
    return *this;
}

auto ComputePipelineBuilder::Binaries(PipelineBinaryStore* binaries) noexcept -> ComputePipelineBuilder& {
    _binaries = binaries;
    return *this;
}

auto ComputePipelineBuilder::HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) noexcept -> ComputePipelineBuilder& {
    _mapping = mapping;
    return *this;
}

auto ComputePipelineBuilder::Validate() const noexcept -> std::expected<void, ErrorCode> {
    using enum PipelineBuilderError;
    if (_shader.code == nullptr || _shader.size == 0) {
        return std::unexpected(MissingShaders);
    }
    if (_shader.size % sizeof(uint32_t) != 0) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }
    return {};
}

} // namespace ZHLN::Vk
