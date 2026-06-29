// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/PipelineBuilder.hpp

#pragma once

#include "RenderCore.hpp"
#include "Vertex.hpp"

#include <vector>

namespace ZHLN::Vk {

// ============================================================================
// Pipeline Builder Result Codes
// ============================================================================

enum class PipelineBuilderResult : uint8_t {
	Succeeded = 0,
	MissingShaders = 1,
	MissingLayout = 2,
};

void ReportPipelineBuilderError(PipelineBuilderResult result) noexcept;
void ReportComputePipelineBuilderError(PipelineBuilderResult result) noexcept;

// ============================================================================
// PipelineConfig — compile-time-friendly POD carrying all pipeline state
// ============================================================================

struct PipelineConfig {
	// Shaders (required)
	const ZHLN_ShaderStages* stages = nullptr;
	VkPipelineLayout layout = VK_NULL_HANDLE;

	// Vertex input (populated by Vertex<T>())
	const VkVertexInputBindingDescription* bindings = nullptr;
	const VkVertexInputAttributeDescription* attributes = nullptr;
	uint32_t bindingCount = 0;
	uint32_t attributeCount = 0;

	// Formats
	std::vector<VkFormat> color_formats = {VK_FORMAT_B8G8R8A8_SRGB};
	VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

	// Rasterization
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
	VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
	VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	// Depth
	bool depth_test = true;
	bool depth_write = true;

	// Blending
	bool blend_enable = false;
	bool additive_blend = false;

	// Specialization
	const VkSpecializationInfo* specialization_info = nullptr;
};

// ============================================================================
// PipelineBuilder — fluent interface over PipelineConfig
// ============================================================================

class PipelineBuilder {
  public:
	PipelineBuilder() = default;

	auto Shaders(const ShaderStages& s) noexcept -> PipelineBuilder&;
	auto Layout(VkPipelineLayout l) noexcept -> PipelineBuilder&;

	// TMP: pulls bindings/attributes directly from your VertexTraits<T>
	// This remains in the header as it is a template method.
	template <IsVertex V> auto Vertex() noexcept -> PipelineBuilder& {
		static constexpr auto bindings = VertexTraits<V>::Bindings();
		static constexpr auto attributes = VertexTraits<V>::Attributes();
		_cfg.bindings = bindings.data();
		_cfg.attributes = attributes.data();
		_cfg.bindingCount = static_cast<uint32_t>(bindings.size());
		_cfg.attributeCount = static_cast<uint32_t>(attributes.size());
		return *this;
	}

	auto ColorFormats(std::initializer_list<VkFormat> formats) noexcept -> PipelineBuilder&;
	auto ColorFormats(std::span<const VkFormat> formats) noexcept -> PipelineBuilder&;
	auto DepthFormat(VkFormat f) noexcept -> PipelineBuilder&;
	auto DepthOnly() noexcept -> PipelineBuilder&;

	auto Topology(VkPrimitiveTopology t) noexcept -> PipelineBuilder&;
	auto Wireframe() noexcept -> PipelineBuilder&;
	auto CullNone() noexcept -> PipelineBuilder&;
	auto CullFront() noexcept -> PipelineBuilder&;
	auto CullBack() noexcept -> PipelineBuilder&;
	[[gnu::warning(R"(
Forbidden. Use CCW whenever necessary.
This is for fixing weird assets.
)")]]
	auto WindingCW() noexcept -> PipelineBuilder&;

	auto DepthTest(bool v) noexcept -> PipelineBuilder&;
	auto DepthWrite(bool v) noexcept -> PipelineBuilder&;
	auto NoDepth() noexcept -> PipelineBuilder&;

	auto AlphaBlend() noexcept -> PipelineBuilder&;

	auto AdditiveBlend() noexcept -> PipelineBuilder&;

	auto Specialization(const VkSpecializationInfo* info) noexcept -> PipelineBuilder&;

	[[nodiscard("Pipeline creation may fail; verify validity before use")]]
	auto Build(VkDevice device) const noexcept -> Pipeline;

  private:
	[[nodiscard]] auto Validate() const noexcept -> PipelineBuilderResult;

	PipelineConfig _cfg;
};

// ============================================================================
// ComputePipelineBuilder — builder for compute pipelines
// ============================================================================

class ComputePipelineBuilder {
  public:
	ComputePipelineBuilder() = default;

	auto Shader(const uint32_t* code, size_t size, const char* entry = "main") noexcept
		-> ComputePipelineBuilder&;
	auto Shader(const ZHLN_ShaderDesc& desc) noexcept -> ComputePipelineBuilder&;
	auto Layout(VkPipelineLayout l) noexcept -> ComputePipelineBuilder&;
	auto Specialization(const VkSpecializationInfo* info) noexcept -> ComputePipelineBuilder&;

	[[nodiscard]] auto Build(VkDevice device) const noexcept -> Pipeline;

  private:
	[[nodiscard]] auto Validate() const noexcept -> PipelineBuilderResult;

	const uint32_t* _code = nullptr;
	size_t _size = 0;
	const char* _entry = nullptr;
	VkPipelineLayout _layout = VK_NULL_HANDLE;
	const VkSpecializationInfo* _specialization_info = nullptr;
};

class PipelineLayoutBuilder {
  public:
	explicit PipelineLayoutBuilder(VkDevice device) noexcept;

	PipelineLayoutBuilder& AddDescriptorSetLayout(VkDescriptorSetLayout layout) noexcept;
	PipelineLayoutBuilder& AddPushConstant(VkShaderStageFlags stages, uint32_t size,
										   uint32_t offset = 0) noexcept;

	[[nodiscard]] PipelineLayout Build() const noexcept;

  private:
	VkDevice _device;
	std::vector<VkDescriptorSetLayout> _setLayouts;
	std::vector<VkPushConstantRange> _pushConstants;
};

} // namespace ZHLN::Vk
