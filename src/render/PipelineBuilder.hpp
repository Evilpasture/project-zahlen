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
	VkFormat color_format = VK_FORMAT_B8G8R8A8_SRGB;
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

	PipelineBuilder& Shaders(const ShaderStages& s) noexcept {
		_cfg.stages = s.Get();
		return *this;
	}

	PipelineBuilder& Layout(VkPipelineLayout l) noexcept {
		_cfg.layout = l;
		return *this;
	}

	// TMP: pulls bindings/attributes directly from your VertexTraits<T>
	// The arrays are constexpr statics so their lifetime is the program's.
	template <IsVertex V> PipelineBuilder& Vertex() noexcept {
		static constexpr auto bindings = VertexTraits<V>::Bindings();
		static constexpr auto attributes = VertexTraits<V>::Attributes();
		_cfg.bindings = bindings.data();
		_cfg.attributes = attributes.data();
		_cfg.bindingCount = static_cast<uint32_t>(bindings.size());
		_cfg.attributeCount = static_cast<uint32_t>(attributes.size());
		return *this;
	}

	// --- Formats ---

	PipelineBuilder& ColorFormat(VkFormat f) noexcept {
		_cfg.color_format = f;
		return *this;
	}

	PipelineBuilder& DepthFormat(VkFormat f) noexcept {
		_cfg.depth_format = f;
		return *this;
	}

	// Convenience: shadow pass (depth-only, no color attachment)
	PipelineBuilder& DepthOnly() noexcept {
		_cfg.color_format = VK_FORMAT_UNDEFINED;
		return *this;
	}

	// --- Rasterization ---

	PipelineBuilder& Topology(VkPrimitiveTopology t) noexcept {
		_cfg.topology = t;
		return *this;
	}

	PipelineBuilder& Wireframe() noexcept {
		_cfg.polygon_mode = VK_POLYGON_MODE_LINE;
		return *this;
	}

	PipelineBuilder& CullNone() noexcept {
		_cfg.cull_mode = VK_CULL_MODE_NONE;
		return *this;
	}

	PipelineBuilder& CullFront() noexcept {
		_cfg.cull_mode = VK_CULL_MODE_FRONT_BIT;
		return *this;
	}

	PipelineBuilder& CullBack() noexcept {
		_cfg.cull_mode = VK_CULL_MODE_BACK_BIT;
		return *this;
	}

	PipelineBuilder& WindingCW() noexcept {
		_cfg.front_face = VK_FRONT_FACE_CLOCKWISE;
		return *this;
	}

	// --- Depth ---

	PipelineBuilder& DepthTest(bool v) noexcept {
		_cfg.depth_test = v;
		return *this;
	}

	PipelineBuilder& DepthWrite(bool v) noexcept {
		_cfg.depth_write = v;
		return *this;
	}

	PipelineBuilder& NoDepth() noexcept {
		_cfg.depth_test = false;
		_cfg.depth_write = false;
		_cfg.depth_format = VK_FORMAT_UNDEFINED;
		return *this;
	}

	// --- Blending ---

	PipelineBuilder& AlphaBlend() noexcept {
		_cfg.blend_enable = true;
		return *this;
	}

	// --- Terminal ---

	// Validate and hand off to your existing C backend.
	// Returns an empty Pipeline on misconfiguration so callers can check .Valid().
	[[nodiscard]] Pipeline Build(VkDevice device) const noexcept {
		if (!Validate())
			return {};

		const ZHLN_GraphicsPipelineDesc desc = {
			.stages = const_cast<ZHLN_ShaderStages*>(_cfg.stages),
			.layout = _cfg.layout,
			.vertex_bindings = _cfg.bindings,
			.vertex_attributes = _cfg.attributes,
			.vertex_binding_count = _cfg.bindingCount,
			.vertex_attribute_count = _cfg.attributeCount,
			.color_format = _cfg.color_format,
			.depth_format = _cfg.depth_format,
			.topology = _cfg.topology,
			.polygon_mode = _cfg.polygon_mode,
			.cull_mode = _cfg.cull_mode,
			.front_face = _cfg.front_face,
			.depth_test = _cfg.depth_test,
			.depth_write = _cfg.depth_write,
			.blend_enable = _cfg.blend_enable,
		};

		return Pipeline(device, ZHLN_CreateGraphicsPipeline(device, &desc));
	}

  private:
	[[nodiscard]] bool Validate() const noexcept {
		if (!_cfg.stages) {
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

} // namespace ZHLN::Vk