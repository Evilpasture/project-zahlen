// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <optional>

namespace ZHLN::Vk {

static constexpr VkCommandBufferInheritanceInfo NullInheritanceInfo = {
    .sType                = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
    .pNext                = nullptr,
    .renderPass           = VK_NULL_HANDLE,
    .subpass              = 0,
    .framebuffer          = VK_NULL_HANDLE,
    .occlusionQueryEnable = VK_FALSE,
    .queryFlags           = 0,
    .pipelineStatistics   = 0
};

struct ImageSlice;

// Keep the layout-first, runtime-format form for existing graph resources;
// the optional second template argument carries a checked attachment format.
template <VkImageLayout Layout, VkFormat Format = VK_FORMAT_UNDEFINED>
struct TypedImage {
    static constexpr VkImageLayout layout       = Layout;
    static constexpr VkFormat      known_format = Format;
    VkImage               handle = VK_NULL_HANDLE;
    VkImageView           view   = VK_NULL_HANDLE;
    VkExtent3D            extent {};
    VkImageAspectFlags    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormat              format = Format;
    VkImageViewCreateInfo info {};
};

// A known-format image is immutable and cannot be assembled from unchecked
// handles. ImageSlice::MatchFormat is the only runtime-to-typed entry point.
template <VkImageLayout Layout, VkFormat Format>
    requires (Format != VK_FORMAT_UNDEFINED)
struct TypedImage<Layout, Format> {
    static constexpr VkImageLayout layout       = Layout;
    static constexpr VkFormat      known_format = Format;
    static constexpr VkFormat      format       = Format;
    const VkImage               handle;
    const VkImageView           view;
    const VkExtent3D            extent;
    const VkImageAspectFlags    aspect;
    const VkImageViewCreateInfo info;

  private:
    friend struct ImageSlice;
    constexpr TypedImage(VkImage image, VkImageView imageView, VkExtent3D size, VkImageAspectFlags imageAspect,
                         const VkImageViewCreateInfo& createInfo) noexcept:
        handle(image), view(imageView), extent(size), aspect(imageAspect), info(createInfo) {
    }
};

struct ImageSlice {
    VkImage     handle = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkExtent3D  extent {};
    VkFormat    format = VK_FORMAT_UNDEFINED;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return handle != VK_NULL_HANDLE && view != VK_NULL_HANDLE;
    }

    [[nodiscard]] constexpr auto Extent2D() const noexcept -> VkExtent2D {
        return {.width = extent.width, .height = extent.height};
    }

    // Raw slices (swapchain, headless and render textures) use single-mip 2D views.
    // Engine-owned cube, array, 3D and mip views instead carry ImageView::Info().
    template <VkImageLayout Layout>
    [[nodiscard]] constexpr auto Assume(VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) const noexcept -> TypedImage<Layout> {
        return {.handle = handle, .view = view, .extent = extent, .aspect = aspect, .format = format,
                .info = Valid() ? MakeViewCreateInfo2D(handle, format, 1, aspect) : VkImageViewCreateInfo {}};
    }

    // Runtime Vulkan images cross into the format-typed API only after their
    // view format has been checked. Assume<Layout>() intentionally stays
    // format-agnostic; it must not manufacture a format proof.
    template <VkFormat Format, VkImageLayout Layout>
    [[nodiscard]] constexpr auto MatchFormat(VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) const noexcept
        -> std::optional<TypedImage<Layout, Format>> {
        static_assert(Format != VK_FORMAT_UNDEFINED, "MatchFormat requires a concrete VkFormat.");
        if (!Valid() || format != Format) {
            return std::nullopt;
        }
        return TypedImage<Layout, Format> {handle, view, extent, aspect, MakeViewCreateInfo2D(handle, Format, 1, aspect)};
    }
};

[[nodiscard]] constexpr auto MakeSlice(VkImage handle, VkImageView view, VkExtent2D extent, VkFormat format) noexcept -> ImageSlice {
    return ImageSlice {
        .handle = handle,
        .view   = view,
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
        .format = format,
    };
}

enum class AttachmentLayout : uint8_t {
    Undefined = 0,
    ColorAttachment,
    ShaderReadOnly,
    DepthStencilAttachment,
    TransferSrc,
    TransferDst,
};

[[nodiscard]] constexpr auto ToVkImageLayout(AttachmentLayout layout) noexcept -> VkImageLayout {
    switch (layout) {
        case AttachmentLayout::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case AttachmentLayout::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case AttachmentLayout::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case AttachmentLayout::DepthStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case AttachmentLayout::TransferSrc:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case AttachmentLayout::TransferDst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

static_assert(
    []() consteval {
        constexpr AttachmentLayout kEveryLayout[] = {
            AttachmentLayout::Undefined,
            AttachmentLayout::ColorAttachment,
            AttachmentLayout::ShaderReadOnly,
            AttachmentLayout::DepthStencilAttachment,
            AttachmentLayout::TransferSrc,
            AttachmentLayout::TransferDst,
        };
        for (const AttachmentLayout layout: kEveryLayout) {
            if (ToVkImageLayout(layout) == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                return false;
            }
        }
        return true;
    }(),
    "AttachmentLayout is the set of layouts a render target may be left in by a frame, and no pass may declare an image "
    "presentable: the presenter decides that from the swapchain, not from what a pass knows about its target."
);


struct UndefinedState {};
struct ColorAttachmentState {};
struct DepthAttachmentState {};
struct DepthStencilAttachmentState {};
struct ShaderReadState {};
struct PresentState {};

template <typename State>
struct LayoutMap;

template <>
struct LayoutMap<UndefinedState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_UNDEFINED;
};
template <>
struct LayoutMap<ColorAttachmentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
};
template <>
struct LayoutMap<DepthAttachmentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
};
template <>
struct LayoutMap<DepthStencilAttachmentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
};
template <>
struct LayoutMap<ShaderReadState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};
template <>
struct LayoutMap<PresentState> {
    static constexpr VkImageLayout value = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
};

template <VkImageLayout Layout>
struct LayoutTraits;

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
[[nodiscard]] constexpr auto MakeLayoutBarrierDesc(
    VkImage            image,
    VkImageAspectFlags aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
    uint32_t           baseMip  = 0,
    uint32_t           mipCount = VK_REMAINING_MIP_LEVELS
) noexcept -> ZHLN_ImageBarrierDesc;

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
void TransitionLayout(
    VkCommandBuffer    cmd,
    VkImage            image,
    VkImageAspectFlags aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
    uint32_t           baseMip  = 0,
    uint32_t           mipCount = VK_REMAINING_MIP_LEVELS
) noexcept;

void ClearColorImage(
    VkCommandBuffer     cmd,
    VkImage             image,
    const VkClearColorValue& color,
    uint32_t            layerCount = 1
) noexcept;

template <typename InState, typename OutState, typename T>
auto IssueBarrier(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE);

template <VkImageLayout NewLayout, VkImageLayout OldLayout>
[[nodiscard]] auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img, VkImageAspectFlags overrideAspect = VK_IMAGE_ASPECT_NONE) noexcept
    -> TypedImage<NewLayout>;


template <typename SrcState, typename DstState>
class ScopedBarrierGuard {
  public:
    VkCommandBuffer                        cmd;
    TypedImage<LayoutMap<SrcState>::value> resource;
    VkImageAspectFlags                     aspectOverride;
    bool                                   active = true;

    ScopedBarrierGuard(VkCommandBuffer c, const TypedImage<LayoutMap<SrcState>::value>& res, VkImageAspectFlags aspect) noexcept;
    ~ScopedBarrierGuard() noexcept;

    ScopedBarrierGuard(const ScopedBarrierGuard&)                    = delete;
    auto operator=(const ScopedBarrierGuard&) -> ScopedBarrierGuard& = delete;

    ScopedBarrierGuard(ScopedBarrierGuard&& other) noexcept;
    auto operator=(ScopedBarrierGuard&& other) noexcept -> ScopedBarrierGuard&;
};

template <typename SrcState, typename DstState, typename T>
[[nodiscard]] auto ScopedBarrier(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE) noexcept;


template <typename SrcState, typename DstState>
struct ScopedBarrierTrans {
    template <typename T>
    [[nodiscard]] auto operator()(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride = VK_IMAGE_ASPECT_NONE) const noexcept {
        return ScopedBarrier<SrcState, DstState, T>(cmd, resource, aspectOverride);
    }
};

using ReadToColorTrans = ScopedBarrierTrans<Vk::ShaderReadState, Vk::ColorAttachmentState>;
using ColorToReadTrans = ScopedBarrierTrans<Vk::ColorAttachmentState, Vk::ShaderReadState>;

inline constexpr ReadToColorTrans ReadToColor {};
inline constexpr ColorToReadTrans ColorToRead {};


static constexpr size_t kMaxColorAttachments = 8;

// DynamicPass binds the stencil attachment only for these combined formats.
// Secondary command buffers must inherit the same format (or UNDEFINED when
// the primary has no stencil attachment).
[[nodiscard]] constexpr auto StencilFormatForDepth(VkFormat depthFormat) noexcept -> VkFormat {
    return (depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || depthFormat == VK_FORMAT_D24_UNORM_S8_UINT) ? depthFormat : VK_FORMAT_UNDEFINED;
}

template <size_t ColorCount, bool HasDepth, typename Formats>
class DynamicPass;

// A snapshot of one DynamicPass's attachment formats and view state. Its
// color formats are owned, not borrowed from a temporary pass. Only the pass
// can construct it; the recording caller cannot supply or change formats.
class SecondaryInheritance {
    template <size_t, bool, typename>
    friend class DynamicPass;

    std::array<VkFormat, kMaxColorAttachments> _colorFormats {};
    uint32_t                                   _colorFormatCount;

    constexpr SecondaryInheritance(
        std::span<const VkFormat> colorFormats, VkFormat depth, VkFormat stencil, uint32_t mask, const VkBindHeapInfoEXT* samplerHeap,
        const VkBindHeapInfoEXT* resourceHeap, std::span<const uint32_t> pushOffsets, std::span<const VkDeviceAddress> pushAddresses,
        VkViewport effectiveViewport
    ) noexcept:
        _colorFormatCount(static_cast<uint32_t>(colorFormats.size())), depthFormat(depth), stencilFormat(stencil), viewMask(mask),
        samplerHeapBindInfo(samplerHeap), resourceHeapBindInfo(resourceHeap), pushDataFrameOffsets(pushOffsets),
        pushDataFrameAddresses(pushAddresses), viewport(effectiveViewport) {
        for (size_t i = 0; i < colorFormats.size(); ++i) {
            _colorFormats[i] = colorFormats[i];
        }
    }

  public:
    [[nodiscard]] constexpr auto ColorFormats() const noexcept -> std::span<const VkFormat> {
        return {_colorFormats.data(), _colorFormatCount};
    }

    const VkFormat                         depthFormat;
    const VkFormat                         stencilFormat;
    const uint32_t                         viewMask;
    const VkBindHeapInfoEXT* const         samplerHeapBindInfo;
    const VkBindHeapInfoEXT* const         resourceHeapBindInfo;
    const std::span<const uint32_t>        pushDataFrameOffsets;
    const std::span<const VkDeviceAddress> pushDataFrameAddresses;
    const VkViewport                       viewport;
};

static_assert(!std::is_aggregate_v<SecondaryInheritance> && !std::is_default_constructible_v<SecondaryInheritance>);

template <VkImageLayout Layout>
struct Tag {};

inline constexpr Tag<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> AsColorAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> AsReadOnly;
inline constexpr Tag<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> AsDepthAttachment;
inline constexpr Tag<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>          AsPresent;

template <VkImageLayout TargetLayout, VkImageLayout OldLayout>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img, Tag<TargetLayout> ) noexcept;

template <typename ImageT, VkImageLayout Final>
class ScopedTransition {
  public:
    ScopedTransition(VkCommandBuffer cmd, ImageT& image, Vk::Tag<Final> transitionTag): cmd_(cmd), image_(Transition(cmd, image, transitionTag)) {
    }
    ~ScopedTransition() {
        [[maybe_unused]] auto _ = Transition(cmd_, image_, Vk::AsReadOnly);
    }
    ScopedTransition(const ScopedTransition&)            = delete;
    ScopedTransition& operator=(const ScopedTransition&) = delete;
    ScopedTransition(ScopedTransition&&)                 = delete;
    ScopedTransition& operator=(ScopedTransition&&)      = delete;
    auto&             Get() {
        return image_;
    }

  private:
    VkCommandBuffer       cmd_;
    Vk::TypedImage<Final> image_;
};

template <typename ImageT, VkImageLayout Final>
ScopedTransition(VkCommandBuffer, ImageT&, Vk::Tag<Final>) -> ScopedTransition<ImageT, Final>;

template <size_t ColorCount = 0, bool HasDepth = false, typename Formats = AttachmentFormats<VK_FORMAT_UNDEFINED>>
class DynamicPass {
  public:
    constexpr explicit DynamicPass(VkExtent2D extent) noexcept
        requires (ColorCount == 0 && !HasDepth && std::same_as<Formats, AttachmentFormats<VK_FORMAT_UNDEFINED>>): _extent(extent) {
    }

    constexpr explicit DynamicPass(VkExtent3D extent) noexcept
        requires (ColorCount == 0 && !HasDepth && std::same_as<Formats, AttachmentFormats<VK_FORMAT_UNDEFINED>>):
        _extent({.width = extent.width, .height = extent.height}) {
    }

    constexpr auto Viewport(float x, float y, float width, float height) && -> DynamicPass<ColorCount, HasDepth, Formats>&& {
        _vpX = x;
        _vpY = y;
        _vpW = width;
        _vpH = height;
        return std::move(*this);
    }

    constexpr auto Viewport(const VkViewport& viewport) && -> DynamicPass<ColorCount, HasDepth, Formats>&& {
        return std::move(*this).Viewport(viewport.x, viewport.y, viewport.width, viewport.height);
    }

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto AddColor(
        const TypedImage<Layout, Format>& img,
        VkAttachmentLoadOp                loadOp     = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp               storeOp    = VK_ATTACHMENT_STORE_OP_STORE,
        const ZHLN::Color4&               clearColor = {}
    ) && noexcept -> DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type>;

    template <typename... TypedImages>
    constexpr auto AddColorGroup(
        const std::tuple<TypedImages...>& imageTuple,
        VkAttachmentLoadOp                loadOp     = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp               storeOp    = VK_ATTACHMENT_STORE_OP_STORE,
        const ZHLN::Color4&               clearColor = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}
    ) && noexcept -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth,
                              typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type>;

    template <VkImageLayout Layout, VkFormat Format>
    constexpr auto AddDepth(
        const TypedImage<Layout, Format>& img,
        VkAttachmentLoadOp                loadOp   = VK_ATTACHMENT_LOAD_OP_LOAD,
        VkAttachmentStoreOp               storeOp  = VK_ATTACHMENT_STORE_OP_STORE,
        float                             clearVal = 1.0F
    ) && noexcept -> DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type>;

    constexpr auto Flags(VkRenderingFlags flags) && noexcept -> DynamicPass<ColorCount, HasDepth, Formats>&&;

    // For a pass begun with VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT.
    // Uses checked formats when available, otherwise the formats recorded by
    // AddColor/AddDepth for runtime-format attachments.
    [[nodiscard]] constexpr auto GetSecondaryInheritance(
        const VkBindHeapInfoEXT* samplerHeap = nullptr, const VkBindHeapInfoEXT* resourceHeap = nullptr,
        std::span<const uint32_t> pushOffsets = {}, std::span<const VkDeviceAddress> pushAddresses = {}
    ) const noexcept -> SecondaryInheritance;

    template <typename Func>
    void Execute(VkCommandBuffer cmd, Func&& func) const;

    constexpr auto ViewMask(uint32_t mask) && noexcept -> DynamicPass<ColorCount, HasDepth, Formats>&&;

    // No raw VkPipeline overload: a checked pass can bind only a pipeline
    // constructed with precisely these attachment formats (and this order).
    void Bind(VkCommandBuffer cmd, const TypedPipeline<ColorCount, HasDepth, Formats>& pipeline) const noexcept
        requires (!std::same_as<Formats, RuntimeAttachmentFormats>) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    }

    void Bind(VkCommandBuffer cmd, const TypedPipeline<ColorCount, HasDepth>& pipeline) const noexcept
        requires std::same_as<Formats, RuntimeAttachmentFormats> {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
    }

  private:
    template <size_t C, bool D, typename F>
    friend class DynamicPass;

    // Only AddColor/AddDepth/AddColorGroup may change the pass's format type.
    template <size_t InsideCount, bool InsideDepth, typename InsideFormats>
    constexpr explicit DynamicPass(DynamicPass<InsideCount, InsideDepth, InsideFormats>&& other) noexcept:
        _extent(other._extent), _flags(other._flags), _colors(std::move(other)._colors), _colorFormats(other._colorFormats),
        _depth(other._depth), _depthFormat(other._depthFormat), _viewMask(other._viewMask), _hasStencil(other._hasStencil),
        _vpX(other._vpX), _vpY(other._vpY), _vpW(other._vpW), _vpH(other._vpH) {
    }

    [[nodiscard]] constexpr auto GetDepthPtr() const noexcept -> const VkRenderingAttachmentInfo*;
    [[nodiscard]] constexpr auto EffectiveViewport() const noexcept -> VkViewport;

    VkExtent2D                                                  _extent {};
    VkRenderingFlags                                            _flags = 0;
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> _colors {};
    std::array<VkFormat, kMaxColorAttachments>                  _colorFormats {};
    VkRenderingAttachmentInfo                                   _depth {};
    VkFormat                                                    _depthFormat = VK_FORMAT_UNDEFINED;
    uint32_t                                                    _viewMask   = 0;
    bool                                                        _hasStencil = false;
    float                                                       _vpX        = 0.0f;
    float                                                       _vpY        = 0.0f;
    float                                                       _vpW        = 0.0f;
    float                                                       _vpH        = 0.0f;
};

DynamicPass(VkExtent2D) -> DynamicPass<0, false>;


struct PassResource {
    ZHLN_ImageBarrierDesc barrier;
};

using PassRecordFn = void (*)(VkCommandBuffer, const void* userData);

struct PassDesc {
    const char*                   name = "Unnamed Pass";
    std::span<const PassResource> transitions;
    PassRecordFn                  record    = nullptr;
    const void*                   pUserData = nullptr;
};

template <size_t MaxStackBarriers = 16>
void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept;

}

#include "DynamicRendering.inl"
