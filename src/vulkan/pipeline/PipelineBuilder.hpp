// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Error.hpp>

#include <optional>

namespace ZHLN::Vk {


enum class PipelineBuilderError : uint8_t {
    MissingShaders ZHLN_ANNOTATION(ZHLN::Description<"Missing shader stages.">{})        = 1,
    MissingLayout ZHLN_ANNOTATION(ZHLN::Description<"Missing pipeline layout.">{})       = 2,
    LayoutCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Pipeline layout creation failed.">{}),
    PipelineCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Pipeline creation failed.">{}),
    TooManyColorAttachments ZHLN_ANNOTATION(ZHLN::Description<"More color formats than the descriptor's fixed blend table holds.">{}),
    OutOfHostMemory ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory.">{}),
};


struct PipelineConfig {
    const ZHLN_ShaderStages* stages = nullptr;
    VkPipelineLayout         layout = VK_NULL_HANDLE;

    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

    bool                                                 descriptor_heap = false;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* vs_mapping      = nullptr;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* ps_mapping      = nullptr;

    const VkVertexInputBindingDescription*   bindings       = nullptr;
    const VkVertexInputAttributeDescription* attributes     = nullptr;
    uint32_t                                 bindingCount   = 0;
    uint32_t                                 attributeCount = 0;

    std::vector<VkFormat> color_formats = {VK_FORMAT_B8G8R8A8_SRGB};
    VkFormat              depth_format  = VK_FORMAT_D32_SFLOAT;

    VkPrimitiveTopology topology     = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode       polygon_mode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags     cull_mode    = VK_CULL_MODE_BACK_BIT;
    VkFrontFace         front_face   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    bool depth_test  = true;
    bool depth_write = true;

    bool blend_enable   = false;
    bool additive_blend = false;

    uint32_t view_mask = 0;

    const VkSpecializationInfo* specialization_info = nullptr;

    std::optional<ZHLN_StencilState> stencil {};
    bool                             color_write_enable = true;
};


template <size_t ColorCount = 1, bool HasDepth = true, typename Formats = RuntimeAttachmentFormats>
class PipelineBuilder {
  public:
    PipelineBuilder() requires std::same_as<Formats, RuntimeAttachmentFormats> = default;

    auto Shaders(const ShaderStages& s) noexcept -> PipelineBuilder& {
        _cfg.stages = s.Get();
        if (s.IsMeshPipeline()) {
            _cfg.bindings       = nullptr;
            _cfg.attributes     = nullptr;
            _cfg.bindingCount   = 0;
            _cfg.attributeCount = 0;
        }
        return *this;
    }

    auto Layout(VkPipelineLayout l) noexcept -> PipelineBuilder& {
        _cfg.layout = l;
        return *this;
    }

    auto Cache(VkPipelineCache cache) noexcept -> PipelineBuilder& {
        _cfg.pipeline_cache = cache;
        return *this;
    }

    auto HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* vsMapping, const VkShaderDescriptorSetAndBindingMappingInfoEXT* psMapping) noexcept
        -> PipelineBuilder& {
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

    auto ColorFormats(std::initializer_list<VkFormat> formats) & noexcept -> PipelineBuilder&
        requires std::same_as<Formats, RuntimeAttachmentFormats> {
        _cfg.color_formats = formats;
        return *this;
    }

    auto ColorFormats(std::span<const VkFormat> formats) & noexcept -> PipelineBuilder&
        requires std::same_as<Formats, RuntimeAttachmentFormats> {
        _cfg.color_formats.assign(formats.begin(), formats.end());
        return *this;
    }

    auto DepthFormat(VkFormat f) & noexcept -> PipelineBuilder& requires std::same_as<Formats, RuntimeAttachmentFormats> {
        _cfg.depth_format = f;
        return *this;
    }

    auto DepthOnly() & noexcept -> PipelineBuilder& requires std::same_as<Formats, RuntimeAttachmentFormats> {
        _cfg.color_formats.clear();
        _cfg.depth_test  = true;
        _cfg.depth_write = true;
        return *this;
    }

    auto NoDepth() & noexcept -> PipelineBuilder& requires std::same_as<Formats, RuntimeAttachmentFormats> {
        _cfg.depth_test   = false;
        _cfg.depth_write  = false;
        _cfg.depth_format = VK_FORMAT_UNDEFINED;
        return *this;
    }

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    auto Build(VkDevice device) const& noexcept -> std::expected<Pipeline, ErrorCode> {
        return Validate().and_then([&]() -> std::expected<Pipeline, ErrorCode> {
            const ZHLN_GraphicsPipelineDesc desc     = GetDesc();
            VkPipeline                      pipeline = ZHLN_CreateGraphicsPipeline(device, &desc);
            if (pipeline == VK_NULL_HANDLE) {
                return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
            }
            return Pipeline(device, pipeline);
        });
    }

    [[nodiscard]] auto DepthOnly() && noexcept -> PipelineBuilder<0, true, typename ClearAttachmentColors<Formats>::type> {
        _cfg.color_formats.clear();
        _cfg.depth_test  = true;
        _cfg.depth_write = true;
        return PipelineBuilder<0, true, typename ClearAttachmentColors<Formats>::type> {std::move(_cfg)};
    }

    [[nodiscard]] auto NoDepth() && noexcept -> PipelineBuilder<ColorCount, false, typename WithoutAttachmentDepth<Formats>::type> {
        _cfg.depth_test   = false;
        _cfg.depth_write  = false;
        _cfg.depth_format = VK_FORMAT_UNDEFINED;
        return PipelineBuilder<ColorCount, false, typename WithoutAttachmentDepth<Formats>::type> {std::move(_cfg)};
    }

    template <VkFormat Depth>
    [[nodiscard]] auto DepthFormat() && noexcept -> PipelineBuilder<ColorCount, true, typename SetAttachmentDepth<Formats, Depth>::type> {
        static_assert(Depth != VK_FORMAT_UNDEFINED, "A depth attachment needs a concrete format.");
        _cfg.depth_format = Depth;
        return PipelineBuilder<ColorCount, true, typename SetAttachmentDepth<Formats, Depth>::type> {std::move(_cfg)};
    }

    auto StencilOp(VkStencilOpState front, VkStencilOpState back) noexcept -> PipelineBuilder& {
        _cfg.stencil = ZHLN_StencilState {.front = front, .back = back};
        return *this;
    }

    auto StencilWriteMask(uint8_t ref = 1, uint8_t mask = 0xFF) noexcept -> PipelineBuilder& {
        const VkStencilOpState state = {
            .failOp      = VK_STENCIL_OP_KEEP,
            .passOp      = VK_STENCIL_OP_REPLACE,
            .depthFailOp = VK_STENCIL_OP_KEEP,
            .compareOp   = VK_COMPARE_OP_ALWAYS,
            .compareMask = mask,
            .writeMask   = mask,
            .reference   = ref,
        };
        return StencilOp(state, state);
    }

    auto StencilCompareMask(VkCompareOp op, uint8_t ref = 1, uint8_t mask = 0xFF) noexcept -> PipelineBuilder& {
        const VkStencilOpState state = {
            .failOp      = VK_STENCIL_OP_KEEP,
            .passOp      = VK_STENCIL_OP_KEEP,
            .depthFailOp = VK_STENCIL_OP_KEEP,
            .compareOp   = op,
            .compareMask = mask,
            .writeMask   = 0x00,
            .reference   = ref,
        };
        return StencilOp(state, state);
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

    // Unlike a runtime array, these values are part of the resulting pipeline
    // type. A typed Build must subsequently specify the depth format or call
    // NoDepth() so the entire rendering signature is known.
    template <VkFormat... Colors>
    [[nodiscard]] auto ColorFormats() && noexcept -> PipelineBuilder<sizeof...(Colors), HasDepth, AttachmentFormats<VK_FORMAT_UNDEFINED, Colors...>> {
        static_assert(((Colors != VK_FORMAT_UNDEFINED) && ...), "Color attachments need concrete formats.");
        _cfg.color_formats = {Colors...};
        return PipelineBuilder<sizeof...(Colors), HasDepth, AttachmentFormats<VK_FORMAT_UNDEFINED, Colors...>> {std::move(_cfg)};
    }

    [[nodiscard]] auto Build(VkDevice device) const&& noexcept -> std::expected<TypedPipeline<ColorCount, HasDepth, Formats>, ErrorCode> {
        if constexpr (!std::same_as<Formats, RuntimeAttachmentFormats>) {
            static_assert(Formats::color_formats.size() == ColorCount, "Typed pipeline color count does not match its formats.");
            static_assert(!HasDepth || Formats::depth_format != VK_FORMAT_UNDEFINED, "Typed pipeline must specify its depth attachment format.");
        }
        return Validate().and_then([&]() -> std::expected<TypedPipeline<ColorCount, HasDepth, Formats>, ErrorCode> {
            const ZHLN_GraphicsPipelineDesc desc     = GetDesc();
            VkPipeline                      pipeline = ZHLN_CreateGraphicsPipeline(device, &desc);
            if (pipeline == VK_NULL_HANDLE) {
                return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
            }
            using Result = TypedPipeline<ColorCount, HasDepth, Formats>;
            return Result {Pipeline(device, pipeline), typename Result::BuilderToken {}};
        });
    }

  private:
    template <size_t, bool, typename>
    friend class PipelineBuilder;
    explicit PipelineBuilder(PipelineConfig cfg) noexcept: _cfg(std::move(cfg)) {
    }

    [[nodiscard]] auto Validate() const noexcept -> std::expected<void, ErrorCode> {
        using enum PipelineBuilderError;
        if (_cfg.stages == nullptr) {
            return std::unexpected(MissingShaders);
        }
        if (_cfg.stages->vert.handle == VK_NULL_HANDLE && _cfg.stages->mesh.handle == VK_NULL_HANDLE) {
            return std::unexpected(MissingShaders);
        }
        if (_cfg.layout == VK_NULL_HANDLE && !_cfg.descriptor_heap) {
            return std::unexpected(MissingLayout);
        }
        if (_cfg.color_formats.size() > ZHLN_MAX_COLOR_ATTACHMENTS) {
            return std::unexpected(TooManyColorAttachments);
        }
        return {};
    }

    [[nodiscard]] constexpr auto GetDesc() const noexcept -> ZHLN_GraphicsPipelineDesc {
        return {
            .stages               = _cfg.stages,
            .layout               = _cfg.layout,
            .pipeline_cache       = _cfg.pipeline_cache,
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
            .stencil              = _cfg.stencil.has_value() ? &*_cfg.stencil : nullptr,
            .color_write_enable   = _cfg.color_write_enable,
        };
    }

    PipelineConfig _cfg;
};


class ComputePipelineBuilder {
  public:
    ComputePipelineBuilder() = default;

    auto Shader(const uint32_t* code, size_t size, const char* entry = nullptr) noexcept -> ComputePipelineBuilder&;
    auto Shader(const ZHLN_ShaderDesc& desc) noexcept -> ComputePipelineBuilder&;
    auto Layout(VkPipelineLayout l) noexcept -> ComputePipelineBuilder&;
    auto Specialization(const VkSpecializationInfo* info) noexcept -> ComputePipelineBuilder&;

    auto Cache(VkPipelineCache cache) noexcept -> ComputePipelineBuilder&;

    auto HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) noexcept -> ComputePipelineBuilder&;
    auto HeapPipeline() noexcept -> ComputePipelineBuilder&;

    [[nodiscard]] auto Build(VkDevice device) const noexcept -> std::expected<Pipeline, ZHLN::ErrorCode>;

  private:
    [[nodiscard]] auto Validate() const noexcept -> std::expected<void, ErrorCode>;

    const uint32_t*                                      _code                = nullptr;
    size_t                                               _size                = 0;
    const char*                                          _entry               = nullptr;
    VkPipelineLayout                                     _layout              = VK_NULL_HANDLE;
    VkPipelineCache                                      _cache               = VK_NULL_HANDLE;
    const VkSpecializationInfo*                          _specialization_info = nullptr;
    bool                                                 _descriptor_heap     = false;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* _mapping             = nullptr;
};

class PipelineLayoutBuilder {
  public:
    explicit PipelineLayoutBuilder(VkDevice device) noexcept;

    PipelineLayoutBuilder& AddPushConstant(VkShaderStageFlags stages, uint32_t size, uint32_t offset = 0) noexcept;

    [[nodiscard]] auto Build() const noexcept -> std::expected<PipelineLayout, ZHLN::ErrorCode>;

  private:
    VkDevice                         _device;
    std::vector<VkPushConstantRange> _pushConstants;
};

}
