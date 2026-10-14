// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "PipelineBuilder.hpp"
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

inline constexpr size_t k_material_flag_variant_count = 5;

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
    if ((translucent && additive) || (depth_write && !translucent && !additive)) {
        return k_material_flag_variant_count;
    }

    if (translucent) {
        return depth_write ? 2U : 1U;
    }
    if (additive) {
        return depth_write ? 4U : 3U;
    }
    return 0;
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
        return Usage::layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
        return Usage::layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL || Usage::layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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
consteval void AppendColorFormat(std::array<VkFormat, N>& formats, size_t& outIndex) noexcept {
    using Usage = typename UsageList::template type<Index>;
    if constexpr (IsColorAttachmentUsage<Usage>()) {
        using Resource      = typename Usage::Resource;
        formats[outIndex++] = Resource::format;
    }
}

template <typename UsageList, size_t... Index>
[[nodiscard]] consteval auto MakeColorFormats(std::index_sequence<Index...>) noexcept {
    constexpr size_t            count = CountColorAttachments<UsageList>(std::index_sequence<Index...> {});
    std::array<VkFormat, count> formats {};
    size_t                      out_index = 0;
    (AppendColorFormat<UsageList, Index>(formats, out_index), ...);
    return formats;
}

template <typename UsageList, size_t... Index>
[[nodiscard]] consteval auto FindDepthFormat(std::index_sequence<Index...>) noexcept -> VkFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
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
[[nodiscard]] consteval auto PassTopology() noexcept -> VkPrimitiveTopology {
    if constexpr (requires { PassContract::topology; }) {
        return PassContract::topology;
    } else {
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
[[nodiscard]] consteval auto PassCullMode() noexcept -> VkCullModeFlags {
    if constexpr (requires { PassContract::cull_mode; }) {
        return PassContract::cull_mode;
    } else {
        return VK_CULL_MODE_BACK_BIT;
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
[[nodiscard]] consteval auto PassPolygonMode() noexcept -> VkPolygonMode {
    if constexpr (requires { PassContract::polygon_mode; }) {
        return PassContract::polygon_mode;
    } else {
        return VK_POLYGON_MODE_FILL;
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
    static constexpr VkFormat depth_format  = PipelineDetail::FindDepthFormat<UsageList>(k_usage_indices);
    static constexpr bool     has_depth     = depth_count != 0;

    static_assert(depth_count <= 1, "A graphics pass contract may declare at most one depth attachment.");
    static_assert(
        [] {
            for (VkFormat format: color_formats) {
                if (format == VK_FORMAT_UNDEFINED) {
                    return false;
                }
            }
            return !has_depth || depth_format != VK_FORMAT_UNDEFINED;
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

    static_assert(StageOf<Vertex>() == VK_SHADER_STAGE_VERTEX_BIT, "GraphicsShaderModules<VS, PS> requires a vertex shader first.");
    static_assert(StageOf<Fragment>() == VK_SHADER_STAGE_FRAGMENT_BIT, "GraphicsShaderModules<VS, PS> requires a fragment shader second.");

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

    static_assert(StageOf<Task>() == VK_SHADER_STAGE_TASK_BIT_EXT, "GraphicsShaderModules<TS, MS, PS> requires a task shader first.");
    static_assert(StageOf<Mesh>() == VK_SHADER_STAGE_MESH_BIT_EXT, "GraphicsShaderModules<TS, MS, PS> requires a mesh shader second.");
    static_assert(StageOf<Fragment>() == VK_SHADER_STAGE_FRAGMENT_BIT, "GraphicsShaderModules<TS, MS, PS> requires a fragment shader third.");

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
    static_assert(StageOf<ShaderModule>() == VK_SHADER_STAGE_COMPUTE_BIT, "ComputePipeline requires a compute ShaderProgram.");

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
        const PipelineConfig config {
            .stages          = stages,
            .vsMapping       = bindings.vertexMapping,
            .psMapping       = bindings.fragmentMapping,
            .colorFormats    = {Attachments::color_formats.begin(), Attachments::color_formats.end()},
            .depthFormat     = Attachments::depth_format,
            .topology        = PipelineDetail::PassTopology<PassContract>(),
            .polygonMode     = PipelineDetail::PassPolygonMode<PassContract>(),
            .cullMode        = HasFlag(Flags, MaterialFlags::DoubleSided) ? VK_CULL_MODE_NONE : PipelineDetail::PassCullMode<PassContract>(),
            .dynamicCullMode = true,
            .depthTest       = Attachments::has_depth && PipelineDetail::PassDepthTest<PassContract>(),
            .depthWrite = HasFlag(Flags, MaterialFlags::DepthWrite) ? true :
                                                                      (!is_blended && Attachments::has_depth && PipelineDetail::PassDepthWrite<PassContract>()),
            .blendEnable        = is_blended,
            .additiveBlend      = is_additive,
            .viewMask           = PipelineDetail::PassViewMask<PassContract>(),
            .specializationInfo = bindings.specialization,
            .colorWriteEnable   = PipelineDetail::PassColorWriteEnable<PassContract>(),
        };

        return PipelineBuilder<>::WithConfig(config).Binaries(binaries).Build(device);
    }
};

} // namespace ZHLN::Vk
