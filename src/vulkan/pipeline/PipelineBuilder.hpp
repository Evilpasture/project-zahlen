// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <span>
#include <array>
#include <type_traits>
#include <utility>

#include "../VkError.hpp"
#include "PipelineTypes.hpp"
#include <Zahlen/Error.hpp>
#include <optional>

namespace ZHLN::Vk {

class PipelineBinaryStore;

enum class PipelineBuilderError : uint8_t {
    MissingShaders         ZHLN_ANNOTATION(ZHLN::Description<"Missing shader stages."> {}) = 1,
    PipelineCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Pipeline creation failed."> {}) = 4,
    OutOfHostMemory        ZHLN_ANNOTATION(ZHLN::Description<"Out of host memory."> {}) = 5,
    MissingVertexOrMeshShader ZHLN_ANNOTATION(ZHLN::Description<"A vertex or mesh shader is required."> {}) = 6,
    StencilFormatRequired ZHLN_ANNOTATION(ZHLN::Description<"Stencil state requires an attachment format with stencil support."> {}) = 7,
    InvalidVertexInput ZHLN_ANNOTATION(ZHLN::Description<"Nonzero vertex input counts require corresponding data pointers."> {}) = 8,
    TooManyColorAttachments ZHLN_ANNOTATION(ZHLN::Description<"Color attachment count cannot be represented as uint32_t."> {}) = 9,
    NoUsableShaderStages ZHLN_ANNOTATION(ZHLN::Description<"No usable shader stages were assembled."> {}) = 10,
    InvalidDevice ZHLN_ANNOTATION(ZHLN::Description<"A non-null Vulkan device is required."> {}) = 11,
    InvalidComputeShader ZHLN_ANNOTATION(ZHLN::Description<"Compute shader code must be non-null with a nonzero, word-aligned size."> {}) = 12,
};

struct StencilState {
    VkStencilOpState front {};
    VkStencilOpState back {};
};

struct PipelineConfig {
    std::optional<ShaderStagesView> stages;
    PipelineBinaryStore*             binaries = nullptr;

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

    bool        depthTest    = true;
    bool        depthWrite   = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;

    bool blendEnable   = false;
    bool additiveBlend = false;
    bool preserveBlendAlpha = false;
    bool accumulateBlendAlpha = false;

    uint32_t viewMask = 0;

    const VkSpecializationInfo* specializationInfo = nullptr;

    std::optional<StencilState> stencil;
    bool                        colorWriteEnable = true;
    VkColorComponentFlags       colorWriteMask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
};

[[nodiscard]] auto FormatHasStencil(VkFormat format) noexcept -> bool;
[[nodiscard]] auto ValidShader(const ShaderDesc& shader) noexcept -> bool;
[[nodiscard]] auto ResolveComputeEntryPoint(const ShaderDesc& shader, std::span<char, 64> name) noexcept -> const char*;

[[nodiscard]] auto CreateGraphicsPipeline(VkDevice device, const VkGraphicsPipelineCreateInfo& createInfo,
                                          std::span<const VkPipelineBinaryKHR> binaries) noexcept -> std::expected<VkPipeline, ErrorCode>;
[[nodiscard]] auto CreateComputePipeline(VkDevice device, const VkComputePipelineCreateInfo& createInfo,
                                         std::span<const VkPipelineBinaryKHR> binaries) noexcept -> std::expected<VkPipeline, ErrorCode>;

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

    auto Binaries(PipelineBinaryStore* binaries) noexcept -> PipelineBuilder& {
        _cfg.binaries = binaries;
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

    auto DynamicCullMode(bool enabled = true) noexcept -> PipelineBuilder& {
        _cfg.dynamicCullMode = enabled;
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

    auto DepthCompare(VkCompareOp op) noexcept -> PipelineBuilder& {
        _cfg.depthCompare = op;
        return *this;
    }

    auto AlphaBlend() noexcept -> PipelineBuilder& {
        _cfg.blendEnable = true;
        return *this;
    }

    // RGB-only radiance overlay. The destination alpha is not radiance.
    // Protect it with both blend factors AND the color write mask.
    auto AdditiveRGBPreserveAlpha() noexcept -> PipelineBuilder& {
        _cfg.blendEnable = true;
        _cfg.additiveBlend = true;
        _cfg.preserveBlendAlpha = true;
        _cfg.accumulateBlendAlpha = false;
        _cfg.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        return *this;
    }

    // Explicit alpha accumulation for attachments where alpha is radiance.
    auto AdditiveRGBA() noexcept -> PipelineBuilder& {
        _cfg.blendEnable = true;
        _cfg.additiveBlend = true;
        _cfg.preserveBlendAlpha = false;
        _cfg.accumulateBlendAlpha = true;
        _cfg.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        return *this;
    }

    // Historical API: RGB adds; alpha REPLACES. Avoid on semantic attachments.
    auto AdditiveBlend() noexcept -> PipelineBuilder& {
        _cfg.blendEnable   = true;
        _cfg.additiveBlend = true;
        _cfg.preserveBlendAlpha = false;
        _cfg.accumulateBlendAlpha = false;
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

    [[nodiscard]] static auto WithConfig(PipelineConfig cfg) noexcept -> PipelineBuilder {
        return PipelineBuilder {std::move(cfg)};
    }

    template <typename Func>
    [[nodiscard]] auto Assemble(Func&& callback) const& noexcept -> std::invoke_result_t<Func, const VkGraphicsPipelineCreateInfo&> {
        const PipelineConfig& config = _cfg;
        if (!config.stages) {
            return std::unexpected(PipelineBuilderError::MissingShaders);
        }
        if (config.colorFormats.size() > UINT32_MAX) {
            return std::unexpected(PipelineBuilderError::TooManyColorAttachments);
        }

        const ShaderStages& shaders       = *config.stages->Get();
        const bool          mesh_pipeline = shaders.mesh.code != nullptr;
        if (!mesh_pipeline && shaders.vert.code == nullptr) {
            return std::unexpected(PipelineBuilderError::MissingVertexOrMeshShader);
        }
        if (config.stencil.has_value() && !FormatHasStencil(config.depthFormat)) {
            return std::unexpected(PipelineBuilderError::StencilFormatRequired);
        }
        if (!mesh_pipeline && ((config.bindingCount != 0 && config.bindings == nullptr) || (config.attributeCount != 0 && config.attributes == nullptr))) {
            return std::unexpected(PipelineBuilderError::InvalidVertexInput);
        }

        constexpr uint32_t                                        k_max_stages = 3;
        std::array<VkPipelineShaderStageCreateInfo, k_max_stages> stage_infos {};
        std::array<VkShaderModuleCreateInfo, k_max_stages>        module_infos {};
        uint32_t                                                  stage_count = 0;

        const auto append_stage = [&](const ShaderStageData& shader, const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) {
            if (shader.code == nullptr) {
                return;
            }
            if (shader.size == 0 || shader.size % sizeof(uint32_t) != 0 || stage_count >= k_max_stages) {
                return;
            }

            module_infos[stage_count] = VkShaderModuleCreateInfo {
                .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext    = mapping,
                .codeSize = shader.size,
                .pCode    = shader.code,
            };
            stage_infos[stage_count] = VkPipelineShaderStageCreateInfo {
                .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext               = &module_infos[stage_count],
                .stage               = shader.stage,
                .module              = VK_NULL_HANDLE,
                .pName               = shader.entryPoint,
                .pSpecializationInfo = config.specializationInfo,
            };
            ++stage_count;
        };

        if (mesh_pipeline) {
            append_stage(shaders.task, config.vsMapping);
            append_stage(shaders.mesh, config.vsMapping);
        } else {
            append_stage(shaders.vert, config.vsMapping);
        }
        append_stage(shaders.frag, config.psMapping);
        if (stage_count == 0) {
            return std::unexpected(PipelineBuilderError::NoUsableShaderStages);
        }

        const VkPipelineVertexInputStateCreateInfo vertex_input {
            .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount   = config.bindingCount,
            .pVertexBindingDescriptions      = config.bindingCount > 0 ? config.bindings : nullptr,
            .vertexAttributeDescriptionCount = config.attributeCount,
            .pVertexAttributeDescriptions    = config.attributeCount > 0 ? config.attributes : nullptr,
        };
        const VkPipelineInputAssemblyStateCreateInfo input_assembly {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology               = config.topology,
            .primitiveRestartEnable = VK_FALSE,
        };
        const VkPipelineViewportStateCreateInfo viewport_state {
            .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount  = 1,
        };
        const VkPipelineRasterizationStateCreateInfo rasterizer {
            .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = config.polygonMode,
            .cullMode    = config.cullMode,
            .frontFace   = config.frontFace,
            .lineWidth   = 1.0F,
        };
        const VkPipelineMultisampleStateCreateInfo multisampling {
            .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        const VkStencilOpState                      no_stencil {};
        const VkPipelineDepthStencilStateCreateInfo depth_stencil {
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable   = config.depthTest ? VK_TRUE : VK_FALSE,
            .depthWriteEnable  = config.depthWrite ? VK_TRUE : VK_FALSE,
            .depthCompareOp    = config.depthCompare,
            .stencilTestEnable = config.stencil.has_value() ? VK_TRUE : VK_FALSE,
            .front             = config.stencil.has_value() ? config.stencil->front : no_stencil,
            .back              = config.stencil.has_value() ? config.stencil->back : no_stencil,
        };

        const VkColorComponentFlags write_mask = config.colorWriteEnable ? config.colorWriteMask : 0;
        const VkPipelineColorBlendAttachmentState blend_state {
            .blendEnable         = config.blendEnable ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = config.additiveBlend ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = config.additiveBlend ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp        = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = config.preserveBlendAlpha ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = (config.preserveBlendAlpha || config.accumulateBlendAlpha) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp        = VK_BLEND_OP_ADD,
            .colorWriteMask      = write_mask,
        };
        std::vector<VkPipelineColorBlendAttachmentState>  blend_attachments(config.colorFormats.size(), blend_state);
        const VkPipelineColorBlendStateCreateInfo         color_blend {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = static_cast<uint32_t>(blend_attachments.size()),
            .pAttachments    = blend_attachments.empty() ? nullptr : blend_attachments.data(),
        };
        constexpr std::array<VkDynamicState, 3> dynamic_states {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_CULL_MODE,
        };
        const VkPipelineDynamicStateCreateInfo dynamic_state {
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = config.dynamicCullMode ? 3U : 2U,
            .pDynamicStates    = dynamic_states.data(),
        };
        const VkPipelineRenderingCreateInfo rendering_info {
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .viewMask                = config.viewMask,
            .colorAttachmentCount    = static_cast<uint32_t>(config.colorFormats.size()),
            .pColorAttachmentFormats = config.colorFormats.empty() ? nullptr : config.colorFormats.data(),
            .depthAttachmentFormat   = config.depthFormat,
            .stencilAttachmentFormat = FormatHasStencil(config.depthFormat) ? config.depthFormat : VK_FORMAT_UNDEFINED,
        };
        const VkPipelineCreateFlags2CreateInfoKHR heap_flags {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
            .pNext = &rendering_info,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        const VkGraphicsPipelineCreateInfo pipeline_info {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &heap_flags,
            .stageCount          = stage_count,
            .pStages             = stage_infos.data(),
            .pVertexInputState   = mesh_pipeline ? nullptr : &vertex_input,
            .pInputAssemblyState = mesh_pipeline ? nullptr : &input_assembly,
            .pViewportState      = &viewport_state,
            .pRasterizationState = &rasterizer,
            .pMultisampleState   = &multisampling,
            .pDepthStencilState  = &depth_stencil,
            .pColorBlendState    = &color_blend,
            .pDynamicState       = &dynamic_state,
            .layout              = VK_NULL_HANDLE,
            .renderPass          = VK_NULL_HANDLE,
            .subpass             = 0,
            .basePipelineHandle  = VK_NULL_HANDLE,
            .basePipelineIndex   = -1,
        };

        return callback(pipeline_info);
    }

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    auto Build(VkDevice device) const& noexcept -> std::expected<Pipeline, ErrorCode> {
        return Validate().and_then([&]() -> std::expected<Pipeline, ErrorCode> {
            auto pipeline = CreatePipeline(device);
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

    // Per-attachment mask; the render graph contract can supply this value.
    auto ColorWriteMask(VkColorComponentFlags mask) noexcept -> PipelineBuilder& {
        _cfg.colorWriteMask = mask;
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
            auto pipeline = CreatePipeline(device);
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

    [[nodiscard]] auto CreatePipeline(VkDevice device) const noexcept -> std::expected<VkPipeline, ErrorCode> {
        if (device == VK_NULL_HANDLE) {
            return std::unexpected(PipelineBuilderError::InvalidDevice);
        }
        return Assemble([&](const VkGraphicsPipelineCreateInfo& info) -> std::expected<VkPipeline, ErrorCode> {
            auto session = PipelineBinarySession::Begin(device, _cfg.binaries, info);
            auto created = CreateGraphicsPipeline(device, info, session.Binaries());
            if (!created && session.Replayed()) {
                session.Invalidate();
                created = CreateGraphicsPipeline(device, info, {});
            }
            if (created) {
                session.Commit();
            }
            return created;
        });
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

    auto Binaries(PipelineBinaryStore* binaries) noexcept -> ComputePipelineBuilder&;

    auto HeapMappings(const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) noexcept -> ComputePipelineBuilder&;

    template <typename Func>
    [[nodiscard]] auto Assemble(Func&& callback) const noexcept -> std::invoke_result_t<Func, const VkComputePipelineCreateInfo&> {
        if (!ValidShader(_shader)) {
            return std::unexpected(PipelineBuilderError::InvalidComputeShader);
        }

        std::array<char, 64>           entry_name {};
        const char*                    entry_point = ResolveComputeEntryPoint(_shader, entry_name);
        const VkShaderModuleCreateInfo module_info {
            .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext    = _mapping,
            .codeSize = _shader.size,
            .pCode    = _shader.code,
        };
        const VkPipelineShaderStageCreateInfo stage_info {
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext               = &module_info,
            .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
            .module              = VK_NULL_HANDLE,
            .pName               = entry_point,
            .pSpecializationInfo = _specialization_info,
        };
        const VkPipelineCreateFlags2CreateInfoKHR heap_flags {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        const VkComputePipelineCreateInfo pipeline_info {
            .sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext              = &heap_flags,
            .stage              = stage_info,
            .layout             = VK_NULL_HANDLE,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex  = -1,
        };

        return callback(pipeline_info);
    }

    [[nodiscard]] auto Build(VkDevice device) const noexcept -> std::expected<Pipeline, ErrorCode> {
        if (auto valid = Validate(); !valid) {
            return std::unexpected(valid.error());
        }
        if (device == VK_NULL_HANDLE) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        return Assemble([&](const VkComputePipelineCreateInfo& info) -> std::expected<Pipeline, ErrorCode> {
            auto session = PipelineBinarySession::Begin(device, _binaries, info);
            auto created = CreateComputePipeline(device, info, session.Binaries());
            if (!created && session.Replayed()) {
                session.Invalidate();
                created = CreateComputePipeline(device, info, {});
            }
            if (!created) {
                return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
            }
            session.Commit();
            return Pipeline(device, *created);
        });
    }

  private:
    [[nodiscard]] auto Validate() const noexcept -> std::expected<void, ErrorCode>;

    ShaderDesc                                           _shader {};
    PipelineBinaryStore*                                   _binaries            = nullptr;
    const VkSpecializationInfo*                          _specialization_info = nullptr;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* _mapping             = nullptr;
};

} // namespace ZHLN::Vk
