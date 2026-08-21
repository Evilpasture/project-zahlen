// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/PipelineBuilder.cpp
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "PipelineBuilder.hpp"
#include <print>

namespace ZHLN::Vk {

void ReportPipelineBuilderError(PipelineBuilderResult result) noexcept {
    switch (result) {
        case PipelineBuilderResult::Succeeded:
            break;
        case PipelineBuilderResult::MissingShaders:
            std::println(stderr, "[PipelineBuilder] Missing shader stages.");
            break;
        case PipelineBuilderResult::MissingLayout:
            std::println(stderr, "[PipelineBuilder] Missing pipeline layout.");
            break;
    }
}

void ReportComputePipelineBuilderError(PipelineBuilderResult result) noexcept {
    switch (result) {
        case PipelineBuilderResult::Succeeded:
            break;
        case PipelineBuilderResult::MissingShaders:
            std::println(stderr, "[ComputePipelineBuilder] Missing or invalid shader code.");
            break;
        case PipelineBuilderResult::MissingLayout:
            std::println(stderr, "[ComputePipelineBuilder] Missing pipeline layout.");
            break;
    }
}

// ============================================================================
// ComputePipelineBuilder Implementation
// ============================================================================

auto ComputePipelineBuilder::Shader(const uint32_t* code, size_t size, const char* entry) noexcept -> ComputePipelineBuilder& {
    _code  = code;
    _size  = size;
    _entry = entry;
    return *this;
}

auto ComputePipelineBuilder::Shader(const ZHLN_ShaderDesc& desc) noexcept -> ComputePipelineBuilder& {
    _code  = desc.code;
    _size  = desc.size;
    _entry = desc.entry_point;
    return *this;
}

auto ComputePipelineBuilder::Layout(const VkPipelineLayout l) noexcept -> ComputePipelineBuilder& {
    _layout = l;
    return *this;
}

auto ComputePipelineBuilder::Specialization(const VkSpecializationInfo* info) noexcept -> ComputePipelineBuilder& {
    _specialization_info = info;
    return *this;
}

auto ComputePipelineBuilder::HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) noexcept -> ComputePipelineBuilder& {
    _descriptor_heap = true;
    _mapping         = mapping;
    return *this;
}

auto ComputePipelineBuilder::HeapPipeline() noexcept -> ComputePipelineBuilder& {
    _descriptor_heap = true;
    return *this;
}

auto ComputePipelineBuilder::Build(const VkDevice device) const noexcept -> std::expected<Pipeline, ZHLN::Error> {
    const auto result = Validate();
    if (result != PipelineBuilderResult::Succeeded) {
        ReportComputePipelineBuilderError(result);
        return std::unexpected(result);
    }

    const ZHLN_ComputePipelineDesc desc = {
        .shader              = {.code = _code, .size = _size, .entry_point = _entry},
        .layout              = _layout,
        .specialization_info = _specialization_info,
        .descriptor_heap     = _descriptor_heap,
        .cs_mapping          = _mapping,
    };

    return Pipeline(device, ZHLN_CreateComputePipeline(device, &desc));
}

auto ComputePipelineBuilder::Validate() const noexcept -> PipelineBuilderResult {
    if ((_code == nullptr) || _size == 0) {
        return PipelineBuilderResult::MissingShaders;
    }
    // VUID-VkComputePipelineCreateInfo-flags-11311: heap pipelines require
    // layout == VK_NULL_HANDLE.
    if (_layout == VK_NULL_HANDLE && !_descriptor_heap) {
        return PipelineBuilderResult::MissingLayout;
    }
    return PipelineBuilderResult::Succeeded;
}

// ============================================================================
// PipelineLayoutBuilder Implementation
// ============================================================================

PipelineLayoutBuilder::PipelineLayoutBuilder(VkDevice device) noexcept: _device(device) {
}

PipelineLayoutBuilder& PipelineLayoutBuilder::AddPushConstant(VkShaderStageFlags stages, uint32_t size, uint32_t offset) noexcept {
    _pushConstants.push_back({.stageFlags = stages, .offset = offset, .size = size});
    return *this;
}

auto PipelineLayoutBuilder::Build() const noexcept -> std::expected<PipelineLayout, ZHLN::Error> {
    const ZHLN_PipelineLayoutDesc desc = {
        .set_layouts         = nullptr,
        .set_layout_count    = 0,
        .push_constants      = _pushConstants.empty() ? nullptr : _pushConstants.data(),
        .push_constant_count = static_cast<uint32_t>(_pushConstants.size())
    };

    VkPipelineLayout layout = ZHLN_CreatePipelineLayout(_device, &desc);
    if (layout == VK_NULL_HANDLE) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    return PipelineLayout(_device, layout);
}

} // namespace ZHLN::Vk
