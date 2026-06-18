// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/PipelineBuilder.cpp

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
// PipelineBuilder Implementation
// ============================================================================

auto PipelineBuilder::Shaders(const ShaderStages& s) noexcept -> PipelineBuilder& {
	_cfg.stages = s.Get();
	return *this;
}

auto PipelineBuilder::Layout(VkPipelineLayout l) noexcept -> PipelineBuilder& {
	_cfg.layout = l;
	return *this;
}

auto PipelineBuilder::ColorFormats(std::initializer_list<VkFormat> formats) noexcept
	-> PipelineBuilder& {
	_cfg.color_formats = formats;
	return *this;
}

auto PipelineBuilder::ColorFormats(std::span<const VkFormat> formats) noexcept -> PipelineBuilder& {
	_cfg.color_formats.assign(formats.begin(), formats.end());
	return *this;
}

auto PipelineBuilder::DepthFormat(VkFormat f) noexcept -> PipelineBuilder& {
	_cfg.depth_format = f;
	return *this;
}

auto PipelineBuilder::DepthOnly() noexcept -> PipelineBuilder& {
	_cfg.color_formats.clear();
	return *this;
}

auto PipelineBuilder::Topology(VkPrimitiveTopology t) noexcept -> PipelineBuilder& {
	_cfg.topology = t;
	return *this;
}

auto PipelineBuilder::Wireframe() noexcept -> PipelineBuilder& {
	_cfg.polygon_mode = VK_POLYGON_MODE_LINE;
	return *this;
}

auto PipelineBuilder::CullNone() noexcept -> PipelineBuilder& {
	_cfg.cull_mode = VK_CULL_MODE_NONE;
	return *this;
}

auto PipelineBuilder::CullFront() noexcept -> PipelineBuilder& {
	_cfg.cull_mode = VK_CULL_MODE_FRONT_BIT;
	return *this;
}

auto PipelineBuilder::CullBack() noexcept -> PipelineBuilder& {
	_cfg.cull_mode = VK_CULL_MODE_BACK_BIT;
	return *this;
}
auto PipelineBuilder::WindingCW() noexcept -> PipelineBuilder& {
	_cfg.front_face = VK_FRONT_FACE_CLOCKWISE;
	return *this;
}

auto PipelineBuilder::DepthTest(bool v) noexcept -> PipelineBuilder& {
	_cfg.depth_test = v;
	return *this;
}

auto PipelineBuilder::DepthWrite(bool v) noexcept -> PipelineBuilder& {
	_cfg.depth_write = v;
	return *this;
}

auto PipelineBuilder::NoDepth() noexcept -> PipelineBuilder& {
	_cfg.depth_test = false;
	_cfg.depth_write = false;
	_cfg.depth_format = VK_FORMAT_UNDEFINED;
	return *this;
}

auto PipelineBuilder::AlphaBlend() noexcept -> PipelineBuilder& {
	_cfg.blend_enable = true;
	return *this;
}

auto PipelineBuilder::Build(VkDevice device) const noexcept -> Pipeline {
	const auto result = Validate();
	if (result != PipelineBuilderResult::Succeeded) {
		ReportPipelineBuilderError(result);
		return {};
	}

	const ZHLN_GraphicsPipelineDesc desc = {
		.stages = _cfg.stages,
		.layout = _cfg.layout,
		.vertex_bindings = _cfg.bindings,
		.vertex_attributes = _cfg.attributes,
		.vertex_binding_count = _cfg.bindingCount,
		.vertex_attribute_count = _cfg.attributeCount,
		.color_formats = _cfg.color_formats.data(),
		.color_format_count = static_cast<uint32_t>(_cfg.color_formats.size()),
		.depth_format = _cfg.depth_format,
		.topology = _cfg.topology,
		.polygon_mode = _cfg.polygon_mode,
		.cull_mode = _cfg.cull_mode,
		.front_face = _cfg.front_face,
		.depth_test = _cfg.depth_test,
		.depth_write = _cfg.depth_write,
		.blend_enable = _cfg.blend_enable,
	};

	return {device, ZHLN_CreateGraphicsPipeline(device, &desc)};
}

auto PipelineBuilder::Validate() const noexcept -> PipelineBuilderResult {
	if (_cfg.stages == nullptr) {
		return PipelineBuilderResult::MissingShaders;
	}
	if (_cfg.layout == VK_NULL_HANDLE) {
		return PipelineBuilderResult::MissingLayout;
	}
	return PipelineBuilderResult::Succeeded;
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

auto ComputePipelineBuilder::Build(const VkDevice device) const noexcept -> Pipeline {
	const auto result = Validate();
	if (result != PipelineBuilderResult::Succeeded) {
		ReportComputePipelineBuilderError(result);
		return {};
	}

	const ZHLN_ComputePipelineDesc desc = {
		.shader = {.code = _code, .size = _size, .entry_point = _entry},
		.layout = _layout,
	};

	return {device, ZHLN_CreateComputePipeline(device, &desc)};
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

} // namespace ZHLN::Vk
