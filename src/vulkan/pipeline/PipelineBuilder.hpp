// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <span>

#include "../VkError.hpp"
#include "PipelineTypes.hpp"
#include <Zahlen/Error.hpp>
#include <optional>

namespace ZHLN::Vk {

enum class PipelineBuilderError : uint8_t {
    MissingShaders         ZHLN_ANNOTATION(ZHLN::Description<"Missing shader stages."> {}) = 1,
    PipelineCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Pipeline creation failed."> {}) = 4,
    OutOfHostMemory        ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory."> {}) = 5,
};

struct StencilState {
    VkStencilOpState front {};
    VkStencilOpState back {};
};

// Every pipeline created by this backend uses descriptor-heap mode; the required
// null layout is supplied internally and is not part of this configuration.
struct PipelineConfig {
    // Keep stage metadata by value across typed builder transitions; Vulkan's
    // create structures borrow the backing data only during synchronous creation.
    std::optional<ShaderStagesView> stages;
    VkPipelineCache                pipelineCache = VK_NULL_HANDLE;

    const VkShaderDescriptorSetAndBindingMappingInfoEXT* vsMapping    = nullptr;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* psMapping    = nullptr;

    const VkVertexInputBindingDescription*   bindings       = nullptr;
    const VkVertexInputAttributeDescription* attributes     = nullptr;
    uint32_t                                 bindingCount   = 0;
    uint32_t                                 attributeCount = 0;

    std::vector<VkFormat> colorFormats = {VK_FORMAT_B8G8R8A8_SRGB};
    VkFormat              depthFormat  = VK_FORMAT_D32_SFLOAT;

    VkPrimitiveTopology topology        = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode       polygonMode     = VK_POLYGON_MODE_FILL;
    VkCullModeFlags     cullMode        = VK_CULL_MODE_BACK_BIT;
    bool                dynamicCullMode = false;
    VkFrontFace         frontFace       = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    bool depthTest  = true;
    bool depthWrite = true;

    bool blendEnable   = false;
    bool additiveBlend = false;

    uint32_t viewMask = 0;

    const VkSpecializationInfo* specializationInfo = nullptr;

    std::optional<StencilState> stencil;
    bool                        colorWriteEnable = true;
};

struct ComputePipelineConfig {
    ShaderDesc                                           shader {};
    VkPipelineCache                                      cache          = VK_NULL_HANDLE;
    const VkSpecializationInfo*                          specialization = nullptr;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping        = nullptr;
};

[[nodiscard]] auto CreateGraphicsPipeline(VkDevice device, const PipelineConfig& config) noexcept -> std::expected<VkPipeline, ErrorCode>;
[[nodiscard]] auto CreateComputePipeline(VkDevice device, const ComputePipelineConfig& config) noexcept -> std::expected<VkPipeline, ErrorCode>;

template <size_t ColorCount = 1, bool HasDepth = true, typename Formats = RuntimeAttachmentFormats>
class PipelineBuilder {
  public:
    PipelineBuilder()
        requires std::same_as<Formats, RuntimeAttachmentFormats>
    = default;

    auto Shaders(ShaderStagesView stages) noexcept -> PipelineBuilder& {
        const bool mesh_pipeline = stages.IsMeshPipeline();
        _cfg.stages              = stages;
        if (mesh_pipeline) {
            _cfg.bindings       = nullptr;
            _cfg.attributes     = nullptr;
            _cfg.bindingCount   = 0;
            _cfg.attributeCount = 0;
        }
        return *this;
    }

    auto Cache(VkPipelineCache cache) noexcept -> PipelineBuilder& {
        _cfg.pipelineCache = cache;
        return *this;
    }

    auto HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* vsMapping, const VkShaderDescriptorSetAndBindingMappingInfoEXT* psMapping) noexcept
        -> PipelineBuilder& {
        _cfg.vsMapping      = vsMapping;
        _cfg.psMapping      = psMapping;
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
        _cfg.polygonMode = VK_POLYGON_MODE_LINE;
        return *this;
    }

    auto CullNone() noexcept -> PipelineBuilder& {
        _cfg.cullMode = VK_CULL_MODE_NONE;
        return *this;
    }

    auto CullFront() noexcept -> PipelineBuilder& {
        _cfg.cullMode = VK_CULL_MODE_FRONT_BIT;
        return *this;
    }

    auto CullBack() noexcept -> PipelineBuilder& {
        _cfg.cullMode = VK_CULL_MODE_BACK_BIT;
        return *this;
    }

    auto ViewMask(uint32_t mask) noexcept -> PipelineBuilder& {
        _cfg.viewMask = mask;
        return *this;
    }

    auto DepthTest(bool v) noexcept -> PipelineBuilder& {
        _cfg.depthTest = v;
        return *this;
    }

    auto DepthWrite(bool v) noexcept -> PipelineBuilder& {
        _cfg.depthWrite = v;
        return *this;
    }

    auto AlphaBlend() noexcept -> PipelineBuilder& {
        _cfg.blendEnable = true;
        return *this;
    }

    auto AdditiveBlend() noexcept -> PipelineBuilder& {
        _cfg.blendEnable   = true;
        _cfg.additiveBlend = true;
        return *this;
    }

    auto Specialization(const VkSpecializationInfo* info) noexcept -> PipelineBuilder& {
        _cfg.specializationInfo = info;
        return *this;
    }

    auto ColorFormats(std::initializer_list<VkFormat> formats) & noexcept -> PipelineBuilder&
        requires std::same_as<Formats, RuntimeAttachmentFormats>
    {
        _cfg.colorFormats = formats;
        return *this;
    }

    auto ColorFormats(std::span<const VkFormat> formats) & noexcept -> PipelineBuilder&
        requires std::same_as<Formats, RuntimeAttachmentFormats>
    {
        _cfg.colorFormats.assign(formats.begin(), formats.end());
        return *this;
    }

    auto DepthFormat(VkFormat f) & noexcept -> PipelineBuilder&
        requires std::same_as<Formats, RuntimeAttachmentFormats>
    {
        _cfg.depthFormat = f;
        return *this;
    }

    auto DepthOnly() & noexcept -> PipelineBuilder&
        requires std::same_as<Formats, RuntimeAttachmentFormats>
    {
        _cfg.colorFormats.clear();
        _cfg.depthTest  = true;
        _cfg.depthWrite = true;
        return *this;
    }

    auto NoDepth() & noexcept -> PipelineBuilder&
        requires std::same_as<Formats, RuntimeAttachmentFormats>
    {
        _cfg.depthTest   = false;
        _cfg.depthWrite  = false;
        _cfg.depthFormat = VK_FORMAT_UNDEFINED;
        return *this;
    }

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    auto Build(VkDevice device) const& noexcept -> std::expected<Pipeline, ErrorCode> {
        return Validate().and_then([&]() -> std::expected<Pipeline, ErrorCode> {
            auto pipeline = CreateGraphicsPipeline(device, _cfg);
            if (!pipeline) {
                return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
            }
            return Pipeline(device, *pipeline);
        });
    }

    [[nodiscard]] auto DepthOnly() && noexcept -> PipelineBuilder<0, true, typename ClearAttachmentColors<Formats>::type> {
        _cfg.colorFormats.clear();
        _cfg.depthTest  = true;
        _cfg.depthWrite = true;
        return PipelineBuilder<0, true, typename ClearAttachmentColors<Formats>::type> {std::move(_cfg)};
    }

    [[nodiscard]] auto NoDepth() && noexcept -> PipelineBuilder<ColorCount, false, typename WithoutAttachmentDepth<Formats>::type> {
        _cfg.depthTest   = false;
        _cfg.depthWrite  = false;
        _cfg.depthFormat = VK_FORMAT_UNDEFINED;
        return PipelineBuilder<ColorCount, false, typename WithoutAttachmentDepth<Formats>::type> {std::move(_cfg)};
    }

    template <VkFormat Depth>
    [[nodiscard]] auto DepthFormat() && noexcept -> PipelineBuilder<ColorCount, true, typename SetAttachmentDepth<Formats, Depth>::type> {
        static_assert(Depth != VK_FORMAT_UNDEFINED, "A depth attachment needs a concrete format.");
        _cfg.depthFormat = Depth;
        return PipelineBuilder<ColorCount, true, typename SetAttachmentDepth<Formats, Depth>::type> {std::move(_cfg)};
    }

    auto StencilOp(VkStencilOpState front, VkStencilOpState back) noexcept -> PipelineBuilder& {
        _cfg.stencil = StencilState {.front = front, .back = back};
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
        _cfg.colorWriteEnable = enable;
        return *this;
    }

    template <size_t N>
        requires (N != std::dynamic_extent)
    [[nodiscard]] auto ColorFormats(std::span<const VkFormat, N> formats) && noexcept -> PipelineBuilder<N, HasDepth> {
        _cfg.colorFormats.assign(formats.begin(), formats.end());
        return PipelineBuilder<N, HasDepth> {std::move(_cfg)};
    }

    template <size_t N>
    [[nodiscard]] auto ColorFormats(const std::array<VkFormat, N>& formats) && noexcept -> PipelineBuilder<N, HasDepth> {
        return std::move(*this).ColorFormats(std::span<const VkFormat, N> {formats});
    }

    // Unlike a runtime array, these values are part of the resulting pipeline
    // type. A typed Build must subsequently specify the depth format or call
    // NoDepth() so the entire rendering signature is known.
    template <VkFormat... Colors>
    [[nodiscard]] auto ColorFormats() && noexcept -> PipelineBuilder<sizeof...(Colors), HasDepth, AttachmentFormats<VK_FORMAT_UNDEFINED, Colors...>> {
        static_assert(((Colors != VK_FORMAT_UNDEFINED) && ...), "Color attachments need concrete formats.");
        _cfg.colorFormats = {Colors...};
        return PipelineBuilder<sizeof...(Colors), HasDepth, AttachmentFormats<VK_FORMAT_UNDEFINED, Colors...>> {std::move(_cfg)};
    }

    [[nodiscard]] auto Build(VkDevice device) const&& noexcept -> std::expected<TypedPipeline<ColorCount, HasDepth, Formats>, ErrorCode> {
        if constexpr (!std::same_as<Formats, RuntimeAttachmentFormats>) {
            static_assert(Formats::color_formats.size() == ColorCount, "Typed pipeline color count does not match its formats.");
            static_assert(!HasDepth || Formats::depth_format != VK_FORMAT_UNDEFINED, "Typed pipeline must specify its depth attachment format.");
        }
        return Validate().and_then([&]() -> std::expected<TypedPipeline<ColorCount, HasDepth, Formats>, ErrorCode> {
            auto pipeline = CreateGraphicsPipeline(device, _cfg);
            if (!pipeline) {
                return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
            }
            using Result = TypedPipeline<ColorCount, HasDepth, Formats>;
            return Result {Pipeline(device, *pipeline), typename Result::BuilderToken {}};
        });
    }

  private:
    template <size_t, bool, typename>
    friend class PipelineBuilder;
    explicit PipelineBuilder(PipelineConfig cfg) noexcept: _cfg(std::move(cfg)) {
    }

    [[nodiscard]] auto Validate() const noexcept -> std::expected<void, ErrorCode> {
        using enum PipelineBuilderError;
        if (!_cfg.stages) {
            return std::unexpected(MissingShaders);
        }
        const ShaderStages* stages = _cfg.stages->Get();
        if (stages->vert.code == nullptr && stages->mesh.code == nullptr) {
            return std::unexpected(MissingShaders);
        }
        for (const ShaderStageData* shader: {&stages->vert, &stages->task, &stages->mesh, &stages->frag}) {
            if ((shader->code == nullptr) != (shader->size == 0) || shader->size % sizeof(uint32_t) != 0) {
                return std::unexpected(ShaderStageCreationError::InvalidSpirvSize);
            }
        }
        return {};
    }

    PipelineConfig _cfg;
};

class ComputePipelineBuilder {
  public:
    ComputePipelineBuilder() = default;

    auto Shader(const uint32_t* code, size_t size, const char* entry = nullptr) noexcept -> ComputePipelineBuilder&;
    auto Shader(const ShaderDesc& desc) noexcept -> ComputePipelineBuilder&;
    auto Specialization(const VkSpecializationInfo* info) noexcept -> ComputePipelineBuilder&;

    auto Cache(VkPipelineCache cache) noexcept -> ComputePipelineBuilder&;

    auto HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) noexcept -> ComputePipelineBuilder&;

    [[nodiscard]] auto Build(VkDevice device) const noexcept -> std::expected<Pipeline, ErrorCode>;

  private:
    [[nodiscard]] auto Validate() const noexcept -> std::expected<void, ErrorCode>;

    ShaderDesc                                           _shader {};
    VkPipelineCache                                      _cache               = VK_NULL_HANDLE;
    const VkSpecializationInfo*                          _specialization_info = nullptr;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* _mapping             = nullptr;
};

} // namespace ZHLN::Vk
