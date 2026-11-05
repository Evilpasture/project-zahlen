// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "PipelineBuilder.hpp" // PipelineBuilderError, CreateGraphicsPipeline, ComputePipelineBuilder
#include "PipelineBinaries.hpp"  // PipelineBinarySession: Create() below runs the binary cache itself
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

enum class MaterialFlags : uint8_t {
    None             = 0,
    Opaque           = None,
    DoubleSided      = 1U << 0U,
    TranslucentBlend = 1U << 1U,
    AdditiveBlend    = 1U << 2U,
    DepthWrite       = 1U << 3U,
};

[[nodiscard]] constexpr auto operator|(MaterialFlags left, MaterialFlags right) noexcept -> MaterialFlags {
    return static_cast<MaterialFlags>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

[[nodiscard]] constexpr auto operator&(MaterialFlags left, MaterialFlags right) noexcept -> MaterialFlags {
    return static_cast<MaterialFlags>(static_cast<uint8_t>(left) & static_cast<uint8_t>(right));
}

constexpr auto operator|=(MaterialFlags& left, MaterialFlags right) noexcept -> MaterialFlags& {
    left = left | right;
    return left;
}

[[nodiscard]] constexpr auto HasFlag(MaterialFlags flags, MaterialFlags flag) noexcept -> bool {
    return (flags & flag) != MaterialFlags::None;
}

// None, translucent, translucent+depth, additive, additive+depth, opaque+depth.
// DoubleSided only changes culling and deliberately shares these state variants.
inline constexpr size_t k_material_flag_variant_count = 6;

[[nodiscard]] constexpr auto MaterialFlagVariantIndex(MaterialFlags flags) noexcept -> size_t {
    constexpr uint8_t k_known_mask = static_cast<uint8_t>(MaterialFlags::DoubleSided) | static_cast<uint8_t>(MaterialFlags::TranslucentBlend) |
                                     static_cast<uint8_t>(MaterialFlags::AdditiveBlend) | static_cast<uint8_t>(MaterialFlags::DepthWrite);
    const auto        bits         = static_cast<uint8_t>(flags);
    if ((bits & static_cast<uint8_t>(~k_known_mask)) != 0) {
        return k_material_flag_variant_count;
    }

    const bool translucent = HasFlag(flags, MaterialFlags::TranslucentBlend);
    const bool additive    = HasFlag(flags, MaterialFlags::AdditiveBlend);
    const bool depth_write = HasFlag(flags, MaterialFlags::DepthWrite);
    if (translucent && additive) {
        return k_material_flag_variant_count;
    }

    if (translucent) {
        return depth_write ? 2U : 1U;
    }
    if (additive) {
        return depth_write ? 4U : 3U;
    }
    return depth_write ? 5U : 0U;
}

struct PipelineCreateBindings {
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* vertexMapping   = nullptr;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* fragmentMapping = nullptr;
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* computeMapping  = nullptr;
    const VkSpecializationInfo*                          specialization  = nullptr;
};

namespace PipelineDetail {

template <typename Usage>
[[nodiscard]] consteval auto IsColorAttachmentUsage() noexcept -> bool {
    if constexpr (requires {
                      typename Usage::Resource;
                      Usage::layout;
                  }) {
        return Usage::layout == Vk::ImageLayout::ColorAttachmentOptimal;
    } else {
        return false;
    }
}

template <typename Usage>
[[nodiscard]] consteval auto IsDepthAttachmentUsage() noexcept -> bool {
    if constexpr (requires {
                      typename Usage::Resource;
                      Usage::layout;
                  }) {
        return Usage::layout == Vk::ImageLayout::DepthAttachmentOptimal || Usage::layout == Vk::ImageLayout::DepthStencilAttachmentOptimal;
    } else {
        return false;
    }
}

template <typename UsageList, size_t... Index>
[[nodiscard]] consteval auto CountColorAttachments(std::index_sequence<Index...>) noexcept -> size_t {
    return (static_cast<size_t>(IsColorAttachmentUsage<typename UsageList::template type<Index>>()) + ... + 0U);
}

template <typename UsageList, size_t... Index>
[[nodiscard]] consteval auto CountDepthAttachments(std::index_sequence<Index...>) noexcept -> size_t {
    return (static_cast<size_t>(IsDepthAttachmentUsage<typename UsageList::template type<Index>>()) + ... + 0U);
}

template <typename UsageList, size_t Index, size_t N>
consteval void AppendColorFormat(std::array<Vk::Format, N>& formats, size_t& outIndex) noexcept {
    using Usage = typename UsageList::template type<Index>;
    if constexpr (IsColorAttachmentUsage<Usage>()) {
        using Resource      = typename Usage::Resource;
        formats[outIndex++] = Resource::format;
    }
}

template <typename UsageList, size_t Index, size_t N>
consteval void AppendColorMask(std::array<VkColorComponentFlags, N>& masks, size_t& outIndex) noexcept {
    using Usage = typename UsageList::template type<Index>;
    if constexpr (IsColorAttachmentUsage<Usage>()) {
        masks[outIndex++] = Usage::channel_mask;
    }
}

template <typename UsageList, size_t... Index>
[[nodiscard]] consteval auto MakeColorMasks(std::index_sequence<Index...>) noexcept {
    constexpr size_t count = CountColorAttachments<UsageList>(std::index_sequence<Index...> {});
    std::array<VkColorComponentFlags, count> masks {};
    size_t outIndex = 0;
    (AppendColorMask<UsageList, Index>(masks, outIndex), ...);
    return masks;
}

template <typename UsageList, size_t... Index>
[[nodiscard]] consteval auto MakeColorFormats(std::index_sequence<Index...>) noexcept {
    constexpr size_t            count = CountColorAttachments<UsageList>(std::index_sequence<Index...> {});
    std::array<Vk::Format, count> formats {};
    size_t                      out_index = 0;
    (AppendColorFormat<UsageList, Index>(formats, out_index), ...);
    return formats;
}

template <typename UsageList, size_t... Index>
[[nodiscard]] consteval auto FindDepthFormat(std::index_sequence<Index...>) noexcept -> Vk::Format {
    Vk::Format format = Vk::Format::Undefined;
    (
        [&] {
            using Usage = typename UsageList::template type<Index>;
            if constexpr (IsDepthAttachmentUsage<Usage>()) {
                using Resource = typename Usage::Resource;
                format         = Resource::format;
            }
        }(),
        ...);
    return format;
}

template <typename PassContract, typename = void>
struct PassPushConstants {
    using type = void;
};

template <typename PassContract>
struct PassPushConstants<PassContract, std::void_t<typename PassContract::PushConstants>> {
    using type = typename PassContract::PushConstants;
};

template <typename PassContract>
[[nodiscard]] consteval auto PassTopology() noexcept -> Vk::PrimitiveTopology {
    if constexpr (requires { PassContract::topology; }) {
        return PassContract::topology;
    } else {
        return Vk::PrimitiveTopology::TriangleList;
    }
}

template <typename PassContract>
[[nodiscard]] consteval auto PassViewMask() noexcept -> uint32_t {
    if constexpr (requires { PassContract::view_mask; }) {
        return PassContract::view_mask;
    } else {
        return 0;
    }
}

template <typename PassContract>
[[nodiscard]] consteval auto PassCullMode() noexcept -> Vk::CullMode {
    if constexpr (requires { PassContract::cull_mode; }) {
        return PassContract::cull_mode;
    } else {
        return Vk::CullMode::Back;
    }
}

template <typename PassContract>
[[nodiscard]] consteval auto PassDepthTest() noexcept -> bool {
    if constexpr (requires { PassContract::depth_test; }) {
        return PassContract::depth_test;
    } else {
        return true;
    }
}

template <typename PassContract>
[[nodiscard]] consteval auto PassDepthWrite() noexcept -> bool {
    if constexpr (requires { PassContract::depth_write; }) {
        return PassContract::depth_write;
    } else {
        return true;
    }
}

template <typename PassContract>
[[nodiscard]] consteval auto PassPolygonMode() noexcept -> Vk::PolygonMode {
    if constexpr (requires { PassContract::polygon_mode; }) {
        return PassContract::polygon_mode;
    } else {
        return Vk::PolygonMode::Fill;
    }
}

template <typename PassContract>
[[nodiscard]] consteval auto PassColorWriteEnable() noexcept -> bool {
    if constexpr (requires { PassContract::color_write_enable; }) {
        return PassContract::color_write_enable;
    } else {
        return true;
    }
}

} // namespace PipelineDetail

template <typename PassContract>
concept GraphicsPassContract = requires { typename PassContract::Usages; };

template <GraphicsPassContract PassContract>
struct PassAttachmentFormats {
    using UsageList = typename PassContract::Usages;

    static constexpr size_t k_usage_count   = UsageList::size;
    static constexpr auto   k_usage_indices = std::make_index_sequence<k_usage_count> {};

    static constexpr size_t   color_count   = PipelineDetail::CountColorAttachments<UsageList>(k_usage_indices);
    static constexpr size_t   depth_count   = PipelineDetail::CountDepthAttachments<UsageList>(k_usage_indices);
    static constexpr auto     color_formats = PipelineDetail::MakeColorFormats<UsageList>(k_usage_indices);
    static constexpr auto     color_masks   = PipelineDetail::MakeColorMasks<UsageList>(k_usage_indices);
    static constexpr Vk::Format depth_format  = PipelineDetail::FindDepthFormat<UsageList>(k_usage_indices);
    static constexpr bool     has_depth     = depth_count != 0;

    static_assert(depth_count <= 1, "A graphics pass contract may declare at most one depth attachment.");
    static_assert(
        [] {
            for (Vk::Format format: color_formats) {
                if (format == Vk::Format::Undefined) {
                    return false;
                }
            }
            return !has_depth || depth_format != Vk::Format::Undefined;
        }(),
        "Pass attachment formats must be statically known Vulkan formats."
    );
};

template <ShaderProgram... Modules>
struct GraphicsShaderModules;

template <ShaderProgram Vertex, ShaderProgram Fragment>
struct GraphicsShaderModules<Vertex, Fragment> {
    using Programs = ShaderSet<Vertex, Fragment>;

    static constexpr bool is_mesh_pipeline = false;

    static_assert(StageOf<Vertex>() == Vk::ShaderStage::Vertex, "GraphicsShaderModules<VS, PS> requires a vertex shader first.");
    static_assert(StageOf<Fragment>() == Vk::ShaderStage::Fragment, "GraphicsShaderModules<VS, PS> requires a fragment shader second.");

    template <typename Push>
    [[nodiscard]] static consteval auto PushLayoutMatches() noexcept -> bool {
        constexpr bool declares_push = DeclaresPushBlock<Vertex> || DeclaresPushBlock<Fragment>;
        if constexpr (!declares_push) {
            return std::is_void_v<Push>;
        } else if constexpr (std::is_void_v<Push>) {
            return false;
        } else {
            return PushConstantLayoutMatchesAll<Push, Vertex, Fragment>();
        }
    }

    template <typename Visitor>
    static void ForEachModule(Visitor&& visitor) {
        visitor(std::type_identity<Vertex> {});
        visitor(std::type_identity<Fragment> {});
    }

    [[nodiscard("Shader creation may fail; verify validity before pipeline creation")]]
    static auto CreateStages() -> std::expected<ShaderStagesView, ErrorCode> {
        return ShaderStagesView::Create<Vertex, Fragment>();
    }
};

template <ShaderProgram Task, ShaderProgram Mesh, ShaderProgram Fragment>
struct GraphicsShaderModules<Task, Mesh, Fragment> {
    using Programs = ShaderSet<Task, Mesh, Fragment>;

    static constexpr bool is_mesh_pipeline = true;

    static_assert(StageOf<Task>() == Vk::ShaderStage::Task, "GraphicsShaderModules<TS, MS, PS> requires a task shader first.");
    static_assert(StageOf<Mesh>() == Vk::ShaderStage::Mesh, "GraphicsShaderModules<TS, MS, PS> requires a mesh shader second.");
    static_assert(StageOf<Fragment>() == Vk::ShaderStage::Fragment, "GraphicsShaderModules<TS, MS, PS> requires a fragment shader third.");

    template <typename Push>
    [[nodiscard]] static consteval auto PushLayoutMatches() noexcept -> bool {
        constexpr bool declares_push = DeclaresPushBlock<Task> || DeclaresPushBlock<Mesh> || DeclaresPushBlock<Fragment>;
        if constexpr (!declares_push) {
            return std::is_void_v<Push>;
        } else if constexpr (std::is_void_v<Push>) {
            return false;
        } else {
            return PushConstantLayoutMatchesAll<Push, Task, Mesh, Fragment>();
        }
    }

    template <typename Visitor>
    static void ForEachModule(Visitor&& visitor) {
        visitor(std::type_identity<Task> {});
        visitor(std::type_identity<Mesh> {});
        visitor(std::type_identity<Fragment> {});
    }

    [[nodiscard("Shader creation may fail; verify validity before pipeline creation")]]
    static auto CreateStages() -> std::expected<ShaderStagesView, ErrorCode> {
        return ShaderStagesView::CreateMesh<Task, Mesh, Fragment>();
    }
};

template <typename ShaderModule>
concept GraphicsShaderModule = requires {
    { ShaderModule::is_mesh_pipeline } -> std::convertible_to<bool>;
    { ShaderModule::template PushLayoutMatches<void>() } -> std::same_as<bool>;
    { ShaderModule::CreateStages() } -> std::same_as<std::expected<ShaderStagesView, ErrorCode>>;
};

template <ShaderProgram ShaderModule, typename PushConstants = void>
struct ComputePipeline {
    static_assert(StageOf<ShaderModule>() == Vk::ShaderStage::Compute, "ComputePipeline requires a compute ShaderProgram.");

    static_assert(
        [] {
            constexpr bool declares_push = DeclaresPushBlock<ShaderModule>;
            if constexpr (!declares_push) {
                return std::is_void_v<PushConstants>;
            } else if constexpr (std::is_void_v<PushConstants>) {
                return false;
            } else {
                return PushConstantLayoutMatchesAll<PushConstants, ShaderModule>();
            }
        }(),
        "ComputePipeline push constants must match the shader module's reflected push block."
    );

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    static auto Create(VkDevice device, const PipelineCreateBindings& bindings, PipelineBinaryStore* binaries = nullptr) noexcept
        -> std::expected<Pipeline, ErrorCode> {
        return Create(device, CreateShaderDesc<ShaderModule>(), bindings, binaries);
    }

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    static auto Create(VkDevice device, PipelineBinaryStore* binaries = nullptr) noexcept -> std::expected<Pipeline, ErrorCode> {
        return Create(device, PipelineCreateBindings {}, binaries);
    }

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    static auto
        Create(VkDevice device, const ShaderDesc& shader, const PipelineCreateBindings& bindings, PipelineBinaryStore* binaries = nullptr) noexcept
        -> std::expected<Pipeline, ErrorCode> {
        if (device == VK_NULL_HANDLE) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        return ComputePipelineBuilder()
            .Shader(shader)
            .Specialization(bindings.specialization)
            .HeapMappings(bindings.computeMapping)
            .Binaries(binaries)
            .Build(device);
    }
};

template <GraphicsShaderModule ShaderModule, GraphicsPassContract PassContract, MaterialFlags Flags = MaterialFlags::None>
struct GraphicsPipeline {
    using Attachments   = PassAttachmentFormats<PassContract>;
    using PushConstants = typename PipelineDetail::PassPushConstants<PassContract>::type;

    static constexpr bool   is_translucent         = HasFlag(Flags, MaterialFlags::TranslucentBlend);
    static constexpr bool   is_additive            = HasFlag(Flags, MaterialFlags::AdditiveBlend);
    static constexpr bool   is_blended             = is_translucent || is_additive;
    static constexpr size_t material_variant_index = MaterialFlagVariantIndex(Flags);

    static_assert(ShaderModule::template PushLayoutMatches<PushConstants>(), "Graphics pass push constants must match shader reflection.");
    static_assert(material_variant_index < k_material_flag_variant_count, "Invalid MaterialFlags combination.");
    static_assert(!is_blended || Attachments::color_count == 1, "Blend material variants require exactly one color attachment.");
    // PipelineBuilder currently has a single colorWriteMask shared by every
    // attachment. Refuse mixed-mask MRT instead of silently violating a graph
    // usage contract; add per-attachment builder state before enabling it.
    static_assert(Attachments::color_count <= 1 || [] {
        for (auto mask: Attachments::color_masks)
            if (mask != (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)) return false;
        return true;
    }(), "Mixed channel masks need per-attachment pipeline state");
    static_assert(!HasFlag(Flags, MaterialFlags::DepthWrite) || Attachments::has_depth, "DepthWrite requires a pass depth attachment.");

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    static auto Create(VkDevice device, const PipelineCreateBindings& bindings, PipelineBinaryStore* binaries = nullptr) noexcept
        -> std::expected<Pipeline, ErrorCode> {
        auto stages = ShaderModule::CreateStages();
        if (!stages) {
            return std::unexpected(stages.error());
        }
        return Create(device, *stages, bindings, binaries);
    }

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    static auto Create(VkDevice device, PipelineBinaryStore* binaries = nullptr) noexcept -> std::expected<Pipeline, ErrorCode> {
        if constexpr (requires {
                          { PassContract::PipelineBindings(device) } -> std::same_as<PipelineCreateBindings>;
                      }) {
            return Create(device, PassContract::PipelineBindings(device), binaries);
        } else {
            return Create(device, PipelineCreateBindings {}, binaries);
        }
    }

    [[nodiscard("Pipeline creation may fail; verify validity before use")]]
    static auto Create(VkDevice device, ShaderStagesView stages, const PipelineCreateBindings& bindings, PipelineBinaryStore* binaries = nullptr) noexcept
        -> std::expected<Pipeline, ErrorCode> {
        if (device == VK_NULL_HANDLE) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        if (!stages.Valid() || stages.IsMeshPipeline() != ShaderModule::is_mesh_pipeline) {
            return std::unexpected(PipelineBuilderError::MissingShaders);
        }
        // The pipeline is a ShaderModule + this PassContract + these
        // MaterialFlags, so the create-info is synthesized here, in one
        // scope, from those three concepts -- there is no config object to
        // unpack. Everything below is native Vulkan; the contract's static
        // formats and state readers convert once, at the field they fill.
        const ShaderStages& shaders       = *stages.Get();
        const bool          mesh_pipeline = ShaderModule::is_mesh_pipeline;

        std::array<VkPipelineShaderStageCreateInfo, 3> stage_infos {};
        std::array<VkShaderModuleCreateInfo, 3>        module_infos {};
        uint32_t                                       stage_count = 0;
        const auto append_stage = [&](const ShaderStageData& shader, const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping) {
            if (shader.code == nullptr || shader.size == 0 || shader.size % sizeof(uint32_t) != 0 || stage_count >= stage_infos.size()) {
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
                .stage               = ToVk(shader.stage),
                .module              = VK_NULL_HANDLE,
                .pName               = shader.entryPoint,
                .pSpecializationInfo = bindings.specialization,
            };
            ++stage_count;
        };
        if (mesh_pipeline) {
            append_stage(shaders.task, bindings.vertexMapping);
            append_stage(shaders.mesh, bindings.vertexMapping);
        } else {
            append_stage(shaders.vert, bindings.vertexMapping);
        }
        append_stage(shaders.frag, bindings.fragmentMapping);
        if (stage_count == 0) {
            return std::unexpected(PipelineBuilderError::NoUsableShaderStages);
        }

        std::array<VkFormat, Attachments::color_count> raw_color_formats {};
        for (size_t i = 0; i < Attachments::color_count; ++i) {
            raw_color_formats[i] = ToVk(Attachments::color_formats[i]);
        }
        const VkFormat raw_depth_format = ToVk(Attachments::depth_format);

        const VkPipelineVertexInputStateCreateInfo vertex_input {
            .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount   = 0,
            .pVertexBindingDescriptions      = nullptr,
            .vertexAttributeDescriptionCount = 0,
            .pVertexAttributeDescriptions    = nullptr,
        };
        const VkPipelineInputAssemblyStateCreateInfo input_assembly {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology               = ToVk(PipelineDetail::PassTopology<PassContract>()),
            .primitiveRestartEnable = VK_FALSE,
        };
        const VkPipelineViewportStateCreateInfo viewport_state {
            .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount  = 1,
        };
        const VkPipelineRasterizationStateCreateInfo rasterizer {
            .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = ToVk(PipelineDetail::PassPolygonMode<PassContract>()),
            .cullMode    = HasFlag(Flags, MaterialFlags::DoubleSided) ? VK_CULL_MODE_NONE : ToVk(PipelineDetail::PassCullMode<PassContract>()),
            .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth   = 1.0F,
        };
        const VkPipelineMultisampleStateCreateInfo multisampling {
            .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        const bool depth_write = HasFlag(Flags, MaterialFlags::DepthWrite) ||
                                 (!is_blended && Attachments::has_depth && PipelineDetail::PassDepthWrite<PassContract>());
        const VkPipelineDepthStencilStateCreateInfo depth_stencil {
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable   = (Attachments::has_depth && PipelineDetail::PassDepthTest<PassContract>()) ? VK_TRUE : VK_FALSE,
            .depthWriteEnable  = depth_write ? VK_TRUE : VK_FALSE,
            .depthCompareOp    = VK_COMPARE_OP_LESS,
            .stencilTestEnable = VK_FALSE,
            .front             = {},
            .back              = {},
        };
        const VkColorComponentFlags write_mask = PipelineDetail::PassColorWriteEnable<PassContract>()
                                                     ? (Attachments::color_count == 1
                                                            ? Attachments::color_masks[0]
                                                            : VkColorComponentFlags {VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT})
                                                     : VkColorComponentFlags {0};
        const VkPipelineColorBlendAttachmentState blend_state {
            .blendEnable         = is_blended ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = is_additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = is_additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp        = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp        = VK_BLEND_OP_ADD,
            .colorWriteMask      = write_mask,
        };
        const std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(Attachments::color_count, blend_state);
        const VkPipelineColorBlendStateCreateInfo              color_blend {
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
            .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
            .pDynamicStates    = dynamic_states.data(),
        };
        const VkPipelineRenderingCreateInfo rendering_info {
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .viewMask                = PipelineDetail::PassViewMask<PassContract>(),
            .colorAttachmentCount    = static_cast<uint32_t>(raw_color_formats.size()),
            .pColorAttachmentFormats = raw_color_formats.empty() ? nullptr : raw_color_formats.data(),
            .depthAttachmentFormat   = raw_depth_format,
            .stencilAttachmentFormat = FormatHasStencil(Attachments::depth_format) ? raw_depth_format : VK_FORMAT_UNDEFINED,
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

        auto session = PipelineBinarySession::Begin(device, binaries, pipeline_info);
        auto created = CreateGraphicsPipeline(device, pipeline_info, session.Binaries());
        if (!created && session.Replayed()) {
            session.Invalidate();
            created = CreateGraphicsPipeline(device, pipeline_info, {});
        }
        if (!created) {
            return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
        }
        session.Commit();
        return Pipeline(device, *created);
    }
};

} // namespace ZHLN::Vk
