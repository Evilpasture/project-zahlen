// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include "PipelineBuilder.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <spirv_reflect.h>

namespace ZHLN::Vk {
namespace {

[[nodiscard]] auto FormatHasStencil(const VkFormat format) noexcept -> bool {
    return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
        format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

[[nodiscard]] auto ResolveComputeEntryPoint(const ShaderDesc& shader, std::array<char, 64>& name) noexcept -> const char* {
    name.fill('\0');
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

[[nodiscard]] auto ValidShader(const ShaderDesc& shader) noexcept -> bool {
    return shader.code != nullptr && shader.size != 0 && shader.size % sizeof(uint32_t) == 0;
}

} // namespace

auto CreateGraphicsPipeline(const VkDevice device, const PipelineConfig& config) noexcept -> std::expected<VkPipeline, VkResult> {
    if (device == VK_NULL_HANDLE || !config.stages || config.color_formats.size() > UINT32_MAX ||
        (config.layout == VK_NULL_HANDLE && !config.descriptor_heap)) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const ShaderStages& shaders = *config.stages->Get();
    const bool meshPipeline = shaders.mesh.code != nullptr;
    if (!meshPipeline && shaders.vert.code == nullptr) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (config.stencil.has_value() && !FormatHasStencil(config.depth_format)) {
        return std::unexpected(VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    if (!meshPipeline && ((config.bindingCount != 0 && config.bindings == nullptr) ||
                          (config.attributeCount != 0 && config.attributes == nullptr))) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    constexpr uint32_t kMaxStages = 3;
    std::array<VkPipelineShaderStageCreateInfo, kMaxStages> stageInfos {};
    std::array<VkShaderModuleCreateInfo, kMaxStages> moduleInfos {};
    uint32_t stageCount = 0;

    const auto appendStage = [&](const ShaderStageData& shader, const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) {
        if (shader.code == nullptr) {
            return;
        }
        if (shader.size == 0 || shader.size % sizeof(uint32_t) != 0 || stageCount >= kMaxStages) {
            return;
        }

        moduleInfos[stageCount] = VkShaderModuleCreateInfo {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = config.descriptor_heap ? mapping : nullptr,
            .codeSize = shader.size,
            .pCode = shader.code,
        };
        stageInfos[stageCount] = VkPipelineShaderStageCreateInfo {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &moduleInfos[stageCount],
            .stage = shader.stage,
            .module = VK_NULL_HANDLE,
            .pName = shader.entry_point,
            .pSpecializationInfo = config.specialization_info,
        };
        ++stageCount;
    };

    if (meshPipeline) {
        appendStage(shaders.task, config.vs_mapping);
        appendStage(shaders.mesh, config.vs_mapping);
    } else {
        appendStage(shaders.vert, config.vs_mapping);
    }
    appendStage(shaders.frag, config.ps_mapping);
    if (stageCount == 0) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const VkPipelineVertexInputStateCreateInfo vertexInput {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = config.bindingCount,
        .pVertexBindingDescriptions = config.bindingCount > 0 ? config.bindings : nullptr,
        .vertexAttributeDescriptionCount = config.attributeCount,
        .pVertexAttributeDescriptions = config.attributeCount > 0 ? config.attributes : nullptr,
    };
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = config.topology,
        .primitiveRestartEnable = VK_FALSE,
    };
    const VkPipelineViewportStateCreateInfo viewportState {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterizer {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = config.polygon_mode,
        .cullMode = config.cull_mode,
        .frontFace = config.front_face,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisampling {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkStencilOpState noStencil {};
    const VkPipelineDepthStencilStateCreateInfo depthStencil {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = config.depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = config.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .stencilTestEnable = config.stencil.has_value() ? VK_TRUE : VK_FALSE,
        .front = config.stencil.has_value() ? config.stencil->front : noStencil,
        .back = config.stencil.has_value() ? config.stencil->back : noStencil,
    };

    const VkColorComponentFlags writeMask = config.color_write_enable
        ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
        : 0;
    const VkPipelineColorBlendAttachmentState blendState {
        .blendEnable = config.blend_enable ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = config.additive_blend ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = config.additive_blend ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = writeMask,
    };
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(config.color_formats.size(), blendState);
    const VkPipelineColorBlendStateCreateInfo colorBlend {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = static_cast<uint32_t>(blendAttachments.size()),
        .pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data(),
    };
    constexpr std::array<VkDynamicState, 2> dynamicStates {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamicState {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };
    const VkPipelineRenderingCreateInfo renderingInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .viewMask = config.view_mask,
        .colorAttachmentCount = static_cast<uint32_t>(config.color_formats.size()),
        .pColorAttachmentFormats = config.color_formats.empty() ? nullptr : config.color_formats.data(),
        .depthAttachmentFormat = config.depth_format,
        .stencilAttachmentFormat = FormatHasStencil(config.depth_format) ? config.depth_format : VK_FORMAT_UNDEFINED,
    };
    const VkPipelineCreateFlags2CreateInfoKHR heapFlags {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
        .pNext = &renderingInfo,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo pipelineInfo {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = config.descriptor_heap ? static_cast<const void*>(&heapFlags) : static_cast<const void*>(&renderingInfo),
        .stageCount = stageCount,
        .pStages = stageInfos.data(),
        .pVertexInputState = meshPipeline ? nullptr : &vertexInput,
        .pInputAssemblyState = meshPipeline ? nullptr : &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlend,
        .pDynamicState = &dynamicState,
        .layout = config.descriptor_heap ? VK_NULL_HANDLE : config.layout,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(device, config.pipeline_cache, 1, &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    return pipeline;
}

auto CreateComputePipeline(const VkDevice device, const ComputePipelineConfig& config) noexcept -> std::expected<VkPipeline, VkResult> {
    if (device == VK_NULL_HANDLE || !ValidShader(config.shader) || (config.layout == VK_NULL_HANDLE && !config.descriptor_heap)) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    std::array<char, 64> entryName {};
    const char* entryPoint = ResolveComputeEntryPoint(config.shader, entryName);
    const VkShaderModuleCreateInfo moduleInfo {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = config.descriptor_heap ? config.mapping : nullptr,
        .codeSize = config.shader.size,
        .pCode = config.shader.code,
    };
    const VkPipelineShaderStageCreateInfo stageInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = &moduleInfo,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = VK_NULL_HANDLE,
        .pName = entryPoint,
        .pSpecializationInfo = config.specialization,
    };
    const VkPipelineCreateFlags2CreateInfoKHR heapFlags {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkComputePipelineCreateInfo pipelineInfo {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = config.descriptor_heap ? static_cast<const void*>(&heapFlags) : nullptr,
        .stage = stageInfo,
        .layout = config.descriptor_heap ? VK_NULL_HANDLE : config.layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device, config.cache, 1, &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
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

auto ComputePipelineBuilder::Layout(const VkPipelineLayout layout) noexcept -> ComputePipelineBuilder& {
    _layout = layout;
    return *this;
}

auto ComputePipelineBuilder::Specialization(const VkSpecializationInfo* info) noexcept -> ComputePipelineBuilder& {
    _specialization_info = info;
    return *this;
}

auto ComputePipelineBuilder::Cache(const VkPipelineCache cache) noexcept -> ComputePipelineBuilder& {
    _cache = cache;
    return *this;
}

auto ComputePipelineBuilder::HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) noexcept -> ComputePipelineBuilder& {
    _descriptor_heap = true;
    _mapping = mapping;
    return *this;
}

auto ComputePipelineBuilder::HeapPipeline() noexcept -> ComputePipelineBuilder& {
    _descriptor_heap = true;
    return *this;
}

auto ComputePipelineBuilder::Build(const VkDevice device) const noexcept -> std::expected<Pipeline, ZHLN::ErrorCode> {
    if (auto valid = Validate(); !valid) {
        return std::unexpected(valid.error());
    }
    const ComputePipelineConfig config {
        .shader = _shader,
        .layout = _layout,
        .cache = _cache,
        .specialization = _specialization_info,
        .descriptor_heap = _descriptor_heap,
        .mapping = _mapping,
    };
    auto pipeline = CreateComputePipeline(device, config);
    if (!pipeline) {
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }
    return Pipeline(device, *pipeline);
}

auto ComputePipelineBuilder::Validate() const noexcept -> std::expected<void, ErrorCode> {
    using enum PipelineBuilderError;
    if (_shader.code == nullptr || _shader.size == 0) {
        return std::unexpected(MissingShaders);
    }
    if (_shader.size % sizeof(uint32_t) != 0) {
        return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
    }
    if (_layout == VK_NULL_HANDLE && !_descriptor_heap) {
        return std::unexpected(MissingLayout);
    }
    return {};
}

PipelineLayoutBuilder::PipelineLayoutBuilder(const VkDevice device) noexcept: _device(device) {}

auto PipelineLayoutBuilder::AddPushConstant(const VkShaderStageFlags stages, const uint32_t size, const uint32_t offset) noexcept
    -> PipelineLayoutBuilder& {
    _pushConstants.push_back({.stageFlags = stages, .offset = offset, .size = size});
    return *this;
}

auto PipelineLayoutBuilder::Build() const noexcept -> std::expected<PipelineLayout, ErrorCode> {
    if (_device == VK_NULL_HANDLE) {
        return std::unexpected(PipelineBuilderError::LayoutCreationFailed);
    }
    const VkPipelineLayoutCreateInfo info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0,
        .pSetLayouts = nullptr,
        .pushConstantRangeCount = static_cast<uint32_t>(_pushConstants.size()),
        .pPushConstantRanges = _pushConstants.empty() ? nullptr : _pushConstants.data(),
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(_device, &info, nullptr, &layout) != VK_SUCCESS) {
        return std::unexpected(PipelineBuilderError::LayoutCreationFailed);
    }
    return PipelineLayout(_device, layout);
}

} // namespace ZHLN::Vk
