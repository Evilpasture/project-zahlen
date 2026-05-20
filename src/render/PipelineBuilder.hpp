#pragma once

#include "RenderCore.hpp"
#include "Vertex.hpp"

namespace ZHLN::Vk {

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
};

// ============================================================================
// PipelineBuilder — fluent interface over PipelineConfig
// ============================================================================

class PipelineBuilder {
  public:
	// --- Required ---

	auto Shaders(const ShaderStages& s) noexcept -> PipelineBuilder& {
		_cfg.stages = s.Get();
		return *this;
	}

	auto Layout(VkPipelineLayout l) noexcept -> PipelineBuilder& {
		_cfg.layout = l;
		return *this;
	}

	// TMP: pulls bindings/attributes directly from your VertexTraits<T>
	// The arrays are constexpr statics so their lifetime is the program's.
	template <IsVertex V> auto Vertex() noexcept -> PipelineBuilder& {
		static constexpr auto bindings = VertexTraits<V>::Bindings();
		static constexpr auto attributes = VertexTraits<V>::Attributes();
		_cfg.bindings = bindings.data();
		_cfg.attributes = attributes.data();
		_cfg.bindingCount = static_cast<uint32_t>(bindings.size());
		_cfg.attributeCount = static_cast<uint32_t>(attributes.size());
		return *this;
	}

	// --- Formats ---

	auto ColorFormats(std::initializer_list<VkFormat> formats) noexcept -> PipelineBuilder& {
		_cfg.color_formats = formats;
		return *this;
	}

	auto DepthFormat(VkFormat f) noexcept -> PipelineBuilder& {
		_cfg.depth_format = f;
		return *this;
	}

	// Convenience: shadow pass (depth-only, no color attachment)
	auto DepthOnly() noexcept -> PipelineBuilder& {
		_cfg.color_formats.clear();
		return *this;
	}

	// --- Rasterization ---

	auto Topology(VkPrimitiveTopology t) noexcept -> PipelineBuilder& {
		_cfg.topology = t;
		return *this;
	}

	auto Wireframe() noexcept -> PipelineBuilder& {
		_cfg.polygon_mode = VK_POLYGON_MODE_LINE;
		return *this;
	}

	auto CullNone() noexcept -> PipelineBuilder& {
		_cfg.cull_mode = VK_CULL_MODE_NONE;
		return *this;
	}

	auto CullFront() noexcept -> PipelineBuilder& {
		_cfg.cull_mode = VK_CULL_MODE_FRONT_BIT;
		return *this;
	}

	auto CullBack() noexcept -> PipelineBuilder& {
		_cfg.cull_mode = VK_CULL_MODE_BACK_BIT;
		return *this;
	}

	auto WindingCW() noexcept -> PipelineBuilder& {
		_cfg.front_face = VK_FRONT_FACE_CLOCKWISE;
		return *this;
	}

	// --- Depth ---

	auto DepthTest(bool v) noexcept -> PipelineBuilder& {
		_cfg.depth_test = v;
		return *this;
	}

	auto DepthWrite(bool v) noexcept -> PipelineBuilder& {
		_cfg.depth_write = v;
		return *this;
	}

	auto NoDepth() noexcept -> PipelineBuilder& {
		_cfg.depth_test = false;
		_cfg.depth_write = false;
		_cfg.depth_format = VK_FORMAT_UNDEFINED;
		return *this;
	}

	// --- Blending ---

	auto AlphaBlend() noexcept -> PipelineBuilder& {
		_cfg.blend_enable = true;
		return *this;
	}

	// --- Terminal ---

	// Validate and hand off to your existing C backend.
	// Returns an empty Pipeline on misconfiguration so callers can check .Valid().
	[[nodiscard]] auto Build(VkDevice device) const noexcept -> Pipeline {
		if (!Validate()) {
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

  private:
	[[nodiscard]] auto Validate() const noexcept -> bool {
		if (_cfg.stages == nullptr) {
			std::println(stderr, "[PipelineBuilder] Missing shader stages.");
			return false;
		}
		if (_cfg.layout == VK_NULL_HANDLE) {
			std::println(stderr, "[PipelineBuilder] Missing pipeline layout.");
			return false;
		}
		return true;
	}

	PipelineConfig _cfg;
};

// ============================================================================
// ComputePipelineBuilder — builder for compute pipelines
// ============================================================================

class ComputePipelineBuilder {
  public:
	// Store individual fields rather than the const struct to allow mutation during "building"
	auto Shader(const uint32_t* code, size_t size, const char* entry = "main") noexcept
		-> ComputePipelineBuilder& {
		_code = code;
		_size = size;
		_entry = entry;
		return *this;
	}

	// Overload if you already have a descriptor from elsewhere
	auto Shader(const ZHLN_ShaderDesc& desc) noexcept -> ComputePipelineBuilder& {
		_code = desc.code;
		_size = desc.size;
		_entry = desc.entry_point;
		return *this;
	}

	auto Layout(const VkPipelineLayout l) noexcept -> ComputePipelineBuilder& {
		_layout = l;
		return *this;
	}

	[[nodiscard]] auto Build(const VkDevice device) const noexcept -> Pipeline {
		if (!Validate()) {
			return {};
		}

		// Aggregate initialization: We construct the 'const struct' locally.
		// This is valid C++ for 'const' typedefs.
		const ZHLN_ComputePipelineDesc desc = {
			.shader = {.code = _code, .size = _size, .entry_point = _entry},
			.layout = _layout,
		};

		return {device, ZHLN_CreateComputePipeline(device, &desc)};
	}

  private:
	[[nodiscard]] auto Validate() const noexcept -> bool {
		if ((_code == nullptr) || _size == 0) {
			std::println(stderr, "[ComputePipelineBuilder] Missing or invalid shader code.");
			return false;
		}
		if (_layout == VK_NULL_HANDLE) {
			std::println(stderr, "[ComputePipelineBuilder] Missing pipeline layout.");
			return false;
		}
		return true;
	}

	const uint32_t* _code = nullptr;
	size_t _size = 0;
	const char* _entry = nullptr;
	VkPipelineLayout _layout = VK_NULL_HANDLE;
};

} // namespace ZHLN::Vk
