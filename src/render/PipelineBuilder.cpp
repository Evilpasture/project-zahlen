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

auto ComputePipelineBuilder::Shader(const uint32_t* code, size_t size, const char* entry) noexcept
	-> ComputePipelineBuilder& {
	_code = code;
	_size = size;
	_entry = entry;
	return *this;
}

auto ComputePipelineBuilder::Shader(const ZHLN_ShaderDesc& desc) noexcept
	-> ComputePipelineBuilder& {
	_code = desc.code;
	_size = desc.size;
	_entry = desc.entry_point;
	return *this;
}

auto ComputePipelineBuilder::Layout(const VkPipelineLayout l) noexcept -> ComputePipelineBuilder& {
	_layout = l;
	return *this;
}

auto ComputePipelineBuilder::Specialization(const VkSpecializationInfo* info) noexcept
	-> ComputePipelineBuilder& {
	_specialization_info = info;
	return *this;
}

auto ComputePipelineBuilder::Build(const VkDevice device) const noexcept
	-> std::expected<Pipeline, PipelineBuilderResult> {
	const auto result = Validate();
	if (result != PipelineBuilderResult::Succeeded) {
		ReportComputePipelineBuilderError(result);
		return std::unexpected(result);
	}

	const ZHLN_ComputePipelineDesc desc = {
		.shader = {.code = _code, .size = _size, .entry_point = _entry},
		.layout = _layout,
		.specialization_info = _specialization_info,
	};

	return Pipeline(device, ZHLN_CreateComputePipeline(device, &desc));
}

auto ComputePipelineBuilder::Validate() const noexcept -> PipelineBuilderResult {
	if ((_code == nullptr) || _size == 0) {
		return PipelineBuilderResult::MissingShaders;
	}
	if (_layout == VK_NULL_HANDLE) {
		return PipelineBuilderResult::MissingLayout;
	}
	return PipelineBuilderResult::Succeeded;
}

// ============================================================================
// PipelineLayoutBuilder Implementation
// ============================================================================

PipelineLayoutBuilder::PipelineLayoutBuilder(VkDevice device) noexcept : _device(device) {}

PipelineLayoutBuilder&
PipelineLayoutBuilder::AddDescriptorSetLayout(VkDescriptorSetLayout layout) noexcept {
	_setLayouts.push_back(layout);
	return *this;
}

PipelineLayoutBuilder& PipelineLayoutBuilder::AddPushConstant(VkShaderStageFlags stages,
															  uint32_t size,
															  uint32_t offset) noexcept {
	_pushConstants.push_back({.stageFlags = stages, .offset = offset, .size = size});
	return *this;
}

PipelineLayout PipelineLayoutBuilder::Build() const noexcept {
	ZHLN_PipelineLayoutDesc desc = {
		.set_layouts = _setLayouts.empty() ? nullptr : _setLayouts.data(),
		.set_layout_count = static_cast<uint32_t>(_setLayouts.size()),
		.push_constants = _pushConstants.empty() ? nullptr : _pushConstants.data(),
		.push_constant_count = static_cast<uint32_t>(_pushConstants.size())};
	return {_device, ZHLN_CreatePipelineLayout(_device, &desc)};
}

} // namespace ZHLN::Vk
