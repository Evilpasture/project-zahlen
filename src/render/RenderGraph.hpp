// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

// ============================================================================
// Compile-Time Resource Identification & Tagging
// ============================================================================

template <size_t N>
struct ResourceName {
    std::array<char, N> value {};

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    constexpr ResourceName(const char (&str)[N]);
};

// Defined early so GraphPass and MakePass can resolve it during compilation
template <typename... Ts>
struct TypeList {
    static constexpr size_t size = sizeof...(Ts);

    // C++26 Pack Indexing on Types: T...[N]
    template <size_t I>
    using type = Ts...[I];
};

// Compile-time frame graph membership predicate
template <typename List, typename Target>
struct IsInList;
template <typename... Ts, typename Target>
struct IsInList<Vk::TypeList<Ts...>, Target> {
    static constexpr bool value = (std::is_same_v<Ts, Target> || ...);
};

template <>
struct TypeList<> {
    static constexpr size_t size = 0;

    template <size_t I>
    using type = void; // Safe fallback, never indexed
};

template <ResourceName Name, VkFormat Format, VkImageAspectFlags Aspect, bool IsSwapchain = false, bool IsPersistent = false>
struct GraphImage {
    static constexpr auto               name          = Name;
    static constexpr VkFormat           format        = Format;
    static constexpr VkImageAspectFlags aspect        = Aspect;
    static constexpr bool               is_swapchain  = IsSwapchain;
    static constexpr bool               is_persistent = IsPersistent;
};

template <typename Image, VkImageLayout Layout, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct Usage {
    using Resource                                = Image;
    static constexpr VkImageLayout         layout = Layout;
    static constexpr VkPipelineStageFlags2 stage  = Stage;
    static constexpr VkAccessFlags2        access = Access;
};

template <typename Image>
using ColorWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT>;

template <typename Image>
using ShaderRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using DepthWrite = Usage<
    Image,
    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT>;

template <typename Image>
using ComputeWrite = Usage<Image, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using ComputeRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <ResourceName Name, typename UsagesList, typename RecordFn_T>
struct GraphPass {
    static constexpr auto name = Name;
    using Usages               = UsagesList;
    using RecordFn             = RecordFn_T;
    RecordFn record;
};

namespace detail {

// Bypass token for authorized framework-level render pass builders
struct BypassGraphicsCheckToken {};

// Type traits to identify rasterization/attachment writes
template <typename U>
struct IsColorAttachment: std::false_type {};

template <typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct IsColorAttachment<Usage<Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Stage, Access>>: std::true_type {};

template <typename U>
struct IsDepthAttachment: std::false_type {};

template <typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct IsDepthAttachment<Usage<Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, Stage, Access>>: std::true_type {};

// Metaprogramming list filter
template <typename InList, typename OutList, template <typename> class Predicate>
struct FilterImpl;

template <typename List, template <typename> class Predicate>
using Filter = typename FilterImpl<List, TypeList<>, Predicate>::type;

template <typename List, typename T>
struct AppendUnique;
template <typename List1, typename List2>
struct MergeLists;
template <typename UsagesList>
struct ExtractResources;
template <typename... Passes>
struct CollectAllResources;

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> /*unused*/) -> size_t;

// Zero-Allocation Compile-Time String Formatter
template <size_t Capacity>
struct ConstexprString {
    std::array<char, Capacity> data_buffer {};
    size_t                     length = 0;

    constexpr void               append(std::string_view sv) noexcept;
    [[nodiscard]] constexpr auto string_view() const noexcept -> std::string_view;
};

template <typename ResourceList, typename Target>
consteval auto GetResourceIndex() -> size_t;

struct ResourceState {
    VkImageLayout         layout            = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage             = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access            = 0;
    size_t                lastWritePass     = 999999;
    bool                  fromPreviousFrame = false;
};

constexpr VkAccessFlags2 WriteMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

template <ResourceState Prev, typename Usage, size_t PassIndex>
struct NeedsBarrier {
    static constexpr bool value = (((Usage::access & WriteMask) != 0) && (Prev.fromPreviousFrame || (Prev.lastWritePass < PassIndex))) ||
                                  (Prev.layout != Usage::layout) || (Prev.stage != Usage::stage) || (Prev.access != Usage::access) ||
                                  (Prev.layout == VK_IMAGE_LAYOUT_UNDEFINED);
};

template <typename ResourceList, typename... Passes>
consteval auto ComputeStateTable();

} // namespace detail

/**
 * @brief SAFE, compile-time verified pass builder.
 * Triggers a static assertion if rasterization attachments are used directly.
 */
template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto MakePass(RecordFn&& record);

/**
 * @brief UNSAFE / Framework-only pass builder.
 * Required for internal wrappers that manually handle render pass boundaries.
 */
template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto Passieren(RecordFn&& record, [[maybe_unused]] detail::BypassGraphicsCheckToken unused = {});

struct GraphResource {
    VkImage     handle = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkExtent3D  extent {}; // Upgraded to 3D to support volumetric targets
};

template <typename ResourceList>
class ResourceBinder {
  public:
    template <typename Image>
    // Upgraded parameter signature to 3D
    constexpr void Bind(VkImage handle, VkImageView view, VkExtent3D extent) noexcept;

    constexpr auto GetBindings() const noexcept -> const std::array<GraphResource, ResourceList::size>&;

  private:
    std::array<GraphResource, ResourceList::size> _resources {};
};

// Forward declare RasterPassContext to satisfy CompileTimeFrameGraph dependencies
template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
class RasterPassContext;

template <typename... Passes>
class CompileTimeFrameGraph {
  public:
    using Resources = typename detail::CollectAllResources<Passes...>::type;
    using Binder    = ResourceBinder<Resources>;

    static constexpr size_t NumPasses    = sizeof...(Passes);
    static constexpr size_t NumResources = Resources::size;

    static constexpr auto StateTable = detail::ComputeStateTable<Resources, Passes...>();

    constexpr explicit CompileTimeFrameGraph(Passes&&... passes);

    void Execute(VkCommandBuffer cmd, const Binder& binder) const;

  private:
    template <size_t PassIndex, typename PassType>
    static consteval size_t CountRequiredBarriers() {
        using Usages = typename PassType::Usages;
        return []<size_t... Is>(std::index_sequence<Is...>) {
            size_t count = 0;
            ((count +=
              []() {
                  using UsageType                            = typename Usages::template type<Is>;
                  using Img                                  = typename UsageType::Resource;
                  constexpr size_t                r_idx      = detail::GetResourceIndex<Resources, Img>();
                  constexpr detail::ResourceState prev_state = StateTable[PassIndex][r_idx];
                  return detail::NeedsBarrier<prev_state, UsageType, PassIndex>::value ? 1 : 0;
              }()),
             ...);
            return count;
        }(std::make_index_sequence<Usages::size> {});
    }

    template <size_t PassIndex, typename PassType, size_t BarrierCount>
    static consteval std::array<size_t, BarrierCount> GetBarrierUsageIndices() {
        std::array<size_t, BarrierCount> indices {};
        using Usages = typename PassType::Usages;

        [&]<size_t... Is>(std::index_sequence<Is...>) {
            size_t write_idx = 0;
            (([&]() {
                 using UsageType                            = typename Usages::template type<Is>;
                 using Img                                  = typename UsageType::Resource;
                 constexpr size_t                r_idx      = detail::GetResourceIndex<Resources, Img>();
                 constexpr detail::ResourceState prev_state = StateTable[PassIndex][r_idx];
                 if constexpr (detail::NeedsBarrier<prev_state, UsageType, PassIndex>::value) {
                     indices[write_idx++] = Is;
                 }
             }()),
             ...);
        }(std::make_index_sequence<Usages::size> {});

        return indices;
    }

    template <size_t PassIndex, typename PassType>
    void ExecutePass(VkCommandBuffer cmd, const std::array<GraphResource, NumResources>& bindings, const PassType& pass) const;

    std::tuple<Passes...> _passes;
};

// ============================================================================
// Automatic RenderPass Execution Context
// ============================================================================

template <typename Tag>
struct ClearColorOf {
    static constexpr Color4 value = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}; // Default: Black
};

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex, typename... Passes>
class RasterPassContext {
  public:
    RasterPassContext(VkCommandBuffer cmd, const std::array<GraphResource, ResourceList::size>& bindings) noexcept;

    ~RasterPassContext() noexcept;

    [[nodiscard]] VkCommandBuffer Cmd() const noexcept;
    [[nodiscard]] VkExtent2D      Extent() const noexcept;

  private:
    VkCommandBuffer                                             m_cmd;
    VkExtent2D                                                  m_extent {};
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> m_colors {};

    template <size_t... Is, size_t... Js>
    void ResolveExtent(
        const std::array<GraphResource, ResourceList::size>& bindings,
        std::index_sequence<Is...> /*unused*/,
        std::index_sequence<Js...> /*unused*/
    ) noexcept;

    template <size_t... Is>
    void BuildColorAttachments(
        const std::array<GraphResource, ResourceList::size>& bindings,
        uint32_t&                                            colorCount,
        std::index_sequence<Is...> /*unused*/
    ) noexcept;

    template <size_t... Js>
    bool BuildDepthAttachment(
        const std::array<GraphResource, ResourceList::size>& bindings,
        VkRenderingAttachmentInfo&                           outDepth,
        std::index_sequence<Js...> /*unused*/
    ) noexcept;
};

/**
 * @brief Pack compile-time resource tags alongside runtime Vulkan handles.
 */
template <typename Tag>
struct GraphImageRef {
    using TagType      = Tag;
    VkImage     handle = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkExtent3D  extent {};
};

template <typename Tag, typename T>
constexpr auto MakeRef(const T& resource) noexcept;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view) noexcept;
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent2D extent) noexcept;

template <typename BinderT, typename... Refs>
constexpr void AutoBind(BinderT& binder, const Refs&... refs) noexcept;

} // namespace ZHLN::Vk

// ============================================================================
// Debug Tools & Compile-Time Inspection API
// ============================================================================

namespace ZHLN::Vk::Debug {

template <size_t Capacity>
struct VisualizerString {
    std::array<char, Capacity> data_buffer {};
    size_t                     length = 0;

    constexpr void               append(std::string_view sv) noexcept;
    constexpr void               append_int(size_t val) noexcept;
    [[nodiscard]] constexpr auto string_view() const noexcept -> std::string_view;
};

template <typename T>
struct GraphVisualizer;

template <typename... Passes>
struct GraphVisualizer<CompileTimeFrameGraph<Passes...>> {
    using GraphT                         = CompileTimeFrameGraph<Passes...>;
    using Resources                      = typename GraphT::Resources;
    static constexpr size_t NumPasses    = GraphT::NumPasses;
    static constexpr size_t NumResources = Resources::size;

    static constexpr std::string_view LayoutToString(VkImageLayout layout);
    static constexpr std::string_view StageToString(VkPipelineStageFlags2 stage);
    static constexpr std::string_view AccessToString(VkAccessFlags2 access);
    static consteval auto             Visualize();
};

template <typename GraphT>
struct ForceGraphVisualization {
    static_assert(sizeof(GraphT) == 0, GraphVisualizer<GraphT>::Visualize().string_view());
};

} // namespace ZHLN::Vk::Debug

#include "RenderGraph.inl"
