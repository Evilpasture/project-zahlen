// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/PipelineBuilder.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Error.hpp>

namespace ZHLN::Vk {

// ============================================================================
// Pipeline Builder Result Codes
// ============================================================================

enum class PipelineBuilderResult : uint8_t {
    Succeeded      = 0,
    MissingShaders = 1,
    MissingLayout  = 2,
};

void ReportPipelineBuilderError(PipelineBuilderResult result) noexcept;
void ReportComputePipelineBuilderError(PipelineBuilderResult result) noexcept;

// ============================================================================
// PipelineConfig — compile-time-friendly POD carrying all pipeline state
// ============================================================================

struct PipelineConfig {
    // Shaders (required)
    const ZHLN_ShaderStages* stages = nullptr;
    VkPipelineLayout         layout = VK_NULL_HANDLE;

    // VK_EXT_descriptor_heap: when true the pipeline is created with
    // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT and each stage maps its
    // legacy set/binding decorations onto the bound heaps via its mapping
    // chain. `layout` must then be an empty pipeline layout (no descriptor
    // set layouts, no push constant ranges — push data replaces both).
    bool                                                     descriptor_heap = false;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT*     vs_mapping      = nullptr;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT*     ps_mapping      = nullptr;

    // Vertex input (populated by Vertex<T>())
    const VkVertexInputBindingDescription*   bindings       = nullptr;
    const VkVertexInputAttributeDescription* attributes     = nullptr;
    uint32_t                                 bindingCount   = 0;
    uint32_t                                 attributeCount = 0;

    // Formats
    std::vector<VkFormat> color_formats = {VK_FORMAT_B8G8R8A8_SRGB};
    VkFormat              depth_format  = VK_FORMAT_D32_SFLOAT;

    // Rasterization
    VkPrimitiveTopology topology     = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode       polygon_mode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags     cull_mode    = VK_CULL_MODE_BACK_BIT;
    VkFrontFace         front_face   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // Depth
    bool depth_test  = true;
    bool depth_write = true;

    // Blending
    bool blend_enable   = false;
    bool additive_blend = false;

    // Multiview
    uint32_t view_mask = 0;

    // Specialization
    const VkSpecializationInfo* specialization_info = nullptr;

    bool             stencil_test = false;
    VkStencilOpState stencil_front {};
    VkStencilOpState stencil_back {};
    bool             color_write_enable = true;
};

// ============================================================================
// PipelineBuilder — strongly-typed typestate builder
// ============================================================================

template <size_t ColorCount = 1, bool HasDepth = true>
class PipelineBuilder {
  public:
    PipelineBuilder() = default;
    explicit PipelineBuilder(PipelineConfig cfg) noexcept: _cfg(std::move(cfg)) {
    }

    auto Shaders(const ShaderStages& s) noexcept -> PipelineBuilder& {
        _cfg.stages = s.Get();
        return *this;
    }

    auto Layout(VkPipelineLayout l) noexcept -> PipelineBuilder& {
        _cfg.layout = l;
        return *this;
    }

    /// Marks the pipeline as a VK_EXT_descriptor_heap consumer and supplies the
    /// per-stage set/binding -> heap mappings (may be null for stages whose
    /// resources are all BDA/push-data backed). The layout must be empty.
    auto HeapMappings(
        const VkShaderDescriptorSetAndBindingMappingInfoEXT* vsMapping, const VkShaderDescriptorSetAndBindingMappingInfoEXT* psMapping
    ) noexcept -> PipelineBuilder& {
        _cfg.descriptor_heap = true;
        _cfg.vs_mapping      = vsMapping;
        _cfg.ps_mapping      = psMapping;
        return *this;
    }

    auto HeapPipeline() noexcept -> PipelineBuilder& {
        _cfg.descriptor_heap = true;
        return *this;
    }

    template <IsVertex V>
    auto Vertex() noexcept -> PipelineBuilder& {
        static constexpr auto bindings   = VertexTraits<V>::Bindings();
        static constexpr auto attributes = VertexTraits<V>::Attributes();
        _cfg.bindings                    = bindings.data();
        _cfg.attributes                  = attributes.data();
        _cfg.bindingCount                = static_cast<uint32_t>(bindings.size());
        _cfg.attributeCount              = static_cast<uint32_t>(attributes.size());
        return *this;
    }

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

    auto ViewMask(uint32_t mask) noexcept -> PipelineBuilder& {
        _cfg.view_mask = mask;
        return *this;
    }

    auto DepthTest(bool v) noexcept -> PipelineBuilder& {
        _cfg.depth_test = v;
        return *this;
    }

    auto DepthWrite(bool v) noexcept -> PipelineBuilder& {
        _cfg.depth_write = v;
        return *this;
    }

    auto AlphaBlend() noexcept -> PipelineBuilder& {
        _cfg.blend_enable = true;
        return *this;
    }

    auto AdditiveBlend() noexcept -> PipelineBuilder& {
        _cfg.blend_enable   = true;
        _cfg.additive_blend = true;
        return *this;
    }

    auto Specialization(const VkSpecializationInfo* info) noexcept -> PipelineBuilder& {
        _cfg.specialization_info = info;
        return *this;
    }

    auto ColorFormats(std::initializer_list<VkFormat> formats) & noexcept -> PipelineBuilder& {
        _cfg.color_formats = formats;
        return *this;
    }

    auto ColorFormats(std::span<const VkFormat> formats) & noexcept -> PipelineBuilder& {
        _cfg.color_formats.assign(formats.begin(), formats.end());
        return *this;
    }

    auto DepthFormat(VkFormat f) & noexcept -> PipelineBuilder& {
        _cfg.depth_format = f;
        return *this;
    }

    auto DepthOnly() & noexcept -> PipelineBuilder& {
        _cfg.color_formats.clear();
        _cfg.depth_test  = true;
        _cfg.depth_write = true;
        return *this;
    }

    auto NoDepth() & noexcept -> PipelineBuilder& {
        _cfg.depth_test   = false;
        _cfg.depth_write  = false;
        _cfg.depth_format = VK_FORMAT_UNDEFINED;
        return *this;
    }

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    auto Build(VkDevice device) const& noexcept -> std::expected<Pipeline, ZHLN::Error> {
        const auto result = Validate();
        if (result != PipelineBuilderResult::Succeeded) {
            ReportPipelineBuilderError(result);
            return std::unexpected(result);
        }

        const ZHLN_GraphicsPipelineDesc desc = GetDesc();
        return Pipeline(device, ZHLN_CreateGraphicsPipeline(device, &desc));
    }

    [[nodiscard]] auto DepthOnly() && noexcept -> PipelineBuilder<0, true> {
        _cfg.color_formats.clear();
        _cfg.depth_test  = true;
        _cfg.depth_write = true;
        return PipelineBuilder<0, true> {std::move(_cfg)};
    }

    [[nodiscard]] auto NoDepth() && noexcept -> PipelineBuilder<ColorCount, false> {
        _cfg.depth_test   = false;
        _cfg.depth_write  = false;
        _cfg.depth_format = VK_FORMAT_UNDEFINED;
        return PipelineBuilder<ColorCount, false> {std::move(_cfg)};
    }

    auto StencilTest(bool enable) noexcept -> PipelineBuilder& {
        _cfg.stencil_test = enable;
        return *this;
    }

    auto StencilOp(VkStencilOpState front, VkStencilOpState back) noexcept -> PipelineBuilder& {
        _cfg.stencil_front = front;
        _cfg.stencil_back  = back;
        return *this;
    }

    auto ColorWriteEnable(bool enable) noexcept -> PipelineBuilder& {
        _cfg.color_write_enable = enable;
        return *this;
    }

    template <size_t N>
    [[nodiscard]] auto ColorFormats(const std::array<VkFormat, N>& formats) && noexcept -> PipelineBuilder<N, HasDepth> {
        _cfg.color_formats.assign(formats.begin(), formats.end());
        return PipelineBuilder<N, HasDepth> {std::move(_cfg)};
    }

    [[nodiscard]] auto Build(VkDevice device) const&& noexcept -> std::expected<TypedPipeline<ColorCount, HasDepth>, ZHLN::Error> {
        const auto result = Validate();
        if (result != PipelineBuilderResult::Succeeded) {
            ReportPipelineBuilderError(result);
            return std::unexpected(result);
        }

        const ZHLN_GraphicsPipelineDesc desc = GetDesc();
        return TypedPipeline<ColorCount, HasDepth> {Pipeline(device, ZHLN_CreateGraphicsPipeline(device, &desc))};
    }

  private:
    [[nodiscard]] auto Validate() const noexcept -> PipelineBuilderResult {
        if (_cfg.stages == nullptr) {
            return PipelineBuilderResult::MissingShaders;
        }
        if (_cfg.layout == VK_NULL_HANDLE) {
            return PipelineBuilderResult::MissingLayout;
        }
        return PipelineBuilderResult::Succeeded;
    }

    [[nodiscard]] constexpr auto GetDesc() const noexcept -> ZHLN_GraphicsPipelineDesc {
        return {
            .stages               = _cfg.stages,
            .layout               = _cfg.layout,
            .descriptor_heap      = _cfg.descriptor_heap,
            .vs_mapping           = _cfg.vs_mapping,
            .ps_mapping           = _cfg.ps_mapping,
            .vertex_bindings      = _cfg.bindings,
            .vertex_attributes    = _cfg.attributes,
            .vertex_binding_count = _cfg.bindingCount,
            .attribute_count      = _cfg.attributeCount,
            .color_formats        = _cfg.color_formats.data(),
            .color_format_count   = static_cast<uint32_t>(_cfg.color_formats.size()),
            .depth_format         = _cfg.depth_format,
            .topology             = _cfg.topology,
            .polygon_mode         = _cfg.polygon_mode,
            .cull_mode            = _cfg.cull_mode,
            .front_face           = _cfg.front_face,
            .depth_test           = _cfg.depth_test,
            .depth_write          = _cfg.depth_write,
            .blend_enable         = _cfg.blend_enable,
            .additive_blend       = _cfg.additive_blend,
            .view_mask            = _cfg.view_mask,
            .specialization_info  = _cfg.specialization_info,
            .stencil_test         = _cfg.stencil_test,
            .stencil_front        = _cfg.stencil_front,
            .stencil_back         = _cfg.stencil_back,
            .color_write_enable   = _cfg.color_write_enable,
        };
    }

    PipelineConfig _cfg;
};

// ============================================================================
// ComputePipelineBuilder — builder for compute pipelines
// ============================================================================

class ComputePipelineBuilder {
  public:
    ComputePipelineBuilder() = default;

    // entry == nullptr → the entry point is reflected from the SPIR-V module.
    auto Shader(const uint32_t* code, size_t size, const char* entry = nullptr) noexcept -> ComputePipelineBuilder&;
    auto Shader(const ZHLN_ShaderDesc& desc) noexcept -> ComputePipelineBuilder&;
    auto Layout(VkPipelineLayout l) noexcept -> ComputePipelineBuilder&;
    auto Specialization(const VkSpecializationInfo* info) noexcept -> ComputePipelineBuilder&;

    /// Marks the pipeline as a VK_EXT_descriptor_heap consumer with the given
    /// set/binding -> heap mapping for the compute stage.
    auto HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) noexcept -> ComputePipelineBuilder&;
    auto HeapPipeline() noexcept -> ComputePipelineBuilder&;

    [[nodiscard]] auto Build(VkDevice device) const noexcept -> std::expected<Pipeline, ZHLN::Error>;

  private:
    [[nodiscard]] auto Validate() const noexcept -> PipelineBuilderResult;

    const uint32_t*             _code                = nullptr;
    size_t                      _size                = 0;
    const char*                 _entry               = nullptr;
    VkPipelineLayout            _layout              = VK_NULL_HANDLE;
    const VkSpecializationInfo* _specialization_info = nullptr;
    bool                        _descriptor_heap     = false;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* _mapping = nullptr;
};

class PipelineLayoutBuilder {
  public:
    explicit PipelineLayoutBuilder(VkDevice device) noexcept;

    PipelineLayoutBuilder& AddDescriptorSetLayout(VkDescriptorSetLayout layout) noexcept;
    PipelineLayoutBuilder& AddPushConstant(VkShaderStageFlags stages, uint32_t size, uint32_t offset = 0) noexcept;

    [[nodiscard]] auto Build() const noexcept -> std::expected<PipelineLayout, ZHLN::Error>;

  private:
    VkDevice                           _device;
    std::vector<VkDescriptorSetLayout> _setLayouts;
    std::vector<VkPushConstantRange>   _pushConstants;
};

} // namespace ZHLN::Vk
