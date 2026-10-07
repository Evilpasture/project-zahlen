// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {

// Centralized Layout State Translation Engine

template <VkImageLayout Layout>
struct LayoutTraits {
  private:
    struct LayoutSyncInfo {
        VkPipelineStageFlags2 stage;
        VkAccessFlags2        access;
        std::string_view      name;
    };
    static constexpr LayoutSyncInfo GetSyncInfo(bool isSource) {
        switch (Layout) {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                return {.stage = VK_PIPELINE_STAGE_2_NONE, .access = 0, .name = "UNDEFINED"};

            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return {
                    .stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .access = isSource ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT :
                                         (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
                    .name   = "COLOR_ATTACHMENT_OPTIMAL"
                };

            case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
                return {
                    .stage  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .access = isSource ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT :
                                         (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
                    .name   = "DEPTH_ATTACHMENT_OPTIMAL"
                };

            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return {
                    .stage  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .access = isSource ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT :
                                         (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
                    .name   = "DEPTH_STENCIL_ATTACHMENT_OPTIMAL"
                };

            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return {
                    .stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .access = VK_ACCESS_2_SHADER_READ_BIT,
                    .name   = "SHADER_READ_ONLY_OPTIMAL"
                };

            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                return {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = 0, .name = "PRESENT_SRC_KHR"};

            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT, .name = "TRANSFER_DST_OPTIMAL"};

            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT, .name = "TRANSFER_SRC_OPTIMAL"};

            case VK_IMAGE_LAYOUT_GENERAL:
                // GENERAL backs storage images: shaders both read and write it, so the
                // source and destination access masks are the same set of bits and the
                // direction makes no difference here.
                return {
                    .stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                    .name   = "GENERAL"
                };

            default:
                return {
                    .stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .access = isSource ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT,
                    .name   = "UNKNOWN_OR_CUSTOM_LAYOUT"
                };
        }
    }

  public:
    static constexpr auto             kInfo = GetSyncInfo(false);
    static constexpr std::string_view kName = kInfo.name;

    static constexpr VkPipelineStageFlags2 kStage = []() constexpr {
        if constexpr (Layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            return VK_PIPELINE_STAGE_2_NONE;
        } else if constexpr (Layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        } else if constexpr (Layout == VK_IMAGE_LAYOUT_GENERAL) {
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        } else {
            return kInfo.stage;
        }
    }();

    static constexpr VkAccessFlags2 kAccess = kInfo.access;
};

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
constexpr auto MakeLayoutBarrierDesc(
    VkImage            image,
    VkImageAspectFlags aspect,
    uint32_t           baseMip,
    uint32_t           mipCount
) noexcept -> ImageBarrierDesc {
    using Src = LayoutTraits<OldLayout>;
    using Dst = LayoutTraits<NewLayout>;
    return {
        .image      = image,
        .src_access = Src::kAccess,
        .dst_access = Dst::kAccess,
        .src_layout = OldLayout,
        .dst_layout = NewLayout,
        .src_stage  = Src::kStage,
        .dst_stage  = Dst::kStage,
        .aspect     = aspect,
        .base_mip   = baseMip,
        .mip_count  = mipCount
    };
}

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
inline void TransitionLayout(
    const VkCommandBuffer    cmd,
    const VkImage            image,
    const VkImageAspectFlags aspect,
    const uint32_t           baseMip,
    const uint32_t           mipCount
) noexcept {
    ImageBarrier(cmd, MakeLayoutBarrierDesc<OldLayout, NewLayout>(image, aspect, baseMip, mipCount));
}

template <VkImageLayout OldLayout, VkImageLayout NewLayout>
inline void TransitionLayout(VkCommandBuffer cmd, VkImage image, const VkImageSubresourceRange& range) noexcept {
    // MakeImageBarrier's ordinary form covers all layers from zero. A partial
    // clear needs both transitions to match the exact range passed to Vulkan.
    VkImageMemoryBarrier2 barrier = MakeImageBarrier(
        MakeLayoutBarrierDesc<OldLayout, NewLayout>(image, range.aspectMask, range.baseMipLevel, range.levelCount)
    );
    barrier.subresourceRange = range;
    PipelineBarrier(cmd, {}, std::span<const VkImageMemoryBarrier2> {&barrier, 1});
}

template <ColorClearTarget T>
constexpr auto GetVkImage(const T& target) noexcept -> VkImage {
    if constexpr (std::same_as<std::remove_cvref_t<T>, VkImage>) {
        return target;
    } else if constexpr (requires { { target.image.Handle() } -> std::same_as<VkImage>; }) {
        return target.image.Handle();
    } else if constexpr (requires { { target.Handle() } -> std::same_as<VkImage>; }) {
        return target.Handle();
    } else {
        return target.image;
    }
}

constexpr auto ToVkClearColor(const Color4& color) noexcept -> VkClearColorValue {
    return {.float32 = {color.r, color.g, color.b, color.a}};
}

constexpr auto ToVkClearColor(float scalar) noexcept -> VkClearColorValue {
    return {.float32 = {scalar, scalar, scalar, scalar}};
}

constexpr auto ToVkClearColor(const VkClearColorValue& color) noexcept -> VkClearColorValue {
    return color;
}

template <VkImageLayout InitialLayout, VkImageLayout FinalLayout, ColorClearTarget Target, ClearColorSource Color>
inline void ClearColorAndTransition(VkCommandBuffer cmd, const Target& target, const Color& color,
                                    const VkImageSubresourceRange& range) noexcept {
    static_assert(FinalLayout != VK_IMAGE_LAYOUT_UNDEFINED, "A cleared image cannot finish in UNDEFINED layout.");
    const VkImage image = GetVkImage(target);
    if (image == VK_NULL_HANDLE) return;

    const VkClearColorValue clear = ToVkClearColor(color);
    TransitionLayout<InitialLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, image, range);
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, FinalLayout>(cmd, image, range);
}

template <VkImageLayout InitialLayout, VkImageLayout FinalLayout, ClearColorSource Color, ColorClearTarget... Targets>
    requires (sizeof...(Targets) > 1)
inline void ClearColorAndTransition(VkCommandBuffer cmd, const Color& color, const Targets&... targets) noexcept {
    (ClearColorAndTransition<InitialLayout, FinalLayout>(cmd, targets, color), ...);
}

static_assert(ColorClearTarget<VkImage> && ColorClearTarget<ImageSlice> && !ColorClearTarget<Color4>);
// 32-bit Vulkan headers may typedef all non-dispatchable handles to uint64_t.
static_assert(std::same_as<VkImage, VkImageView> || !ColorClearTarget<VkImageView>);
static_assert(std::same_as<VkImage, VkBuffer> || !ColorClearTarget<VkBuffer>);
static_assert(ToVkClearColor(Color4 {0.25f, 0.5f, 0.75f, 1.0f}).float32[2] == 0.75f);
static_assert(ToVkClearColor(1.0f).float32[3] == 1.0f);
static_assert(ToVkClearColor(VkClearColorValue {.float32 = {0.1f, 0.2f, 0.3f, 0.4f}}).float32[1] == 0.2f);

inline void ClearColorImage(const VkCommandBuffer cmd, const VkImage image, const VkClearColorValue& color, const uint32_t layerCount) noexcept {
    // Retain the legacy mip-0/color-attachment contract; both barriers now
    // cover exactly the layers being cleared, not unrelated array layers.
    const VkImageSubresourceRange range {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = layerCount,
    };
    ClearColorAndTransition<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, image, color, range);
}

// Scoped RAII Layout Transition Implementations

template <typename SrcState, typename DstState>
ScopedBarrierGuard<SrcState, DstState>::ScopedBarrierGuard(
    VkCommandBuffer                               c,
    const TypedImage<LayoutMap<SrcState>::value>& res,
    VkImageAspectFlags                            aspect
) noexcept: cmd(c), resource(res), aspectOverride(aspect) {
}

template <typename SrcState, typename DstState>
ScopedBarrierGuard<SrcState, DstState>::~ScopedBarrierGuard() noexcept {
    if (active) {
        IssueBarrier<DstState, SrcState>(cmd, resource, aspectOverride);
    }
}

template <typename SrcState, typename DstState>
ScopedBarrierGuard<SrcState, DstState>::ScopedBarrierGuard(ScopedBarrierGuard&& other) noexcept:
    cmd(other.cmd), resource(other.resource), aspectOverride(other.aspectOverride), active(other.active) {
    other.active = false;
}

template <typename SrcState, typename DstState>
auto ScopedBarrierGuard<SrcState, DstState>::operator=(ScopedBarrierGuard&& other) noexcept -> ScopedBarrierGuard& {
    if (this != &other) {
        if (active) {
            IssueBarrier<DstState, SrcState>(cmd, resource, aspectOverride);
        }
        cmd            = other.cmd;
        resource       = other.resource;
        aspectOverride = other.aspectOverride;
        active         = other.active;
        other.active   = false;
    }
    return *this;
}

template <typename SrcState, typename DstState, typename T>
auto ScopedBarrier(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride) noexcept {
    auto transitioned_image = IssueBarrier<SrcState, DstState>(cmd, resource, aspectOverride);

    constexpr VkImageLayout src_layout = LayoutMap<SrcState>::value;
    TypedImage<src_layout>  src_image;
    if constexpr (requires { resource.State(); }) {
        src_image = TypedImage<src_layout> {resource.State().Raw()};
    } else if constexpr (requires { resource.Raw(); }) {
        src_image = TypedImage<src_layout> {resource.Raw()};
    } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, ImageSlice>) {
        src_image = TypedImage<src_layout> {resource};
    } else {
        src_image = TypedImage<src_layout> {ImageSlice {resource.image.Handle(), resource.view, resource.extent, resource.view.Format()}};
    }

    return std::make_pair(transitioned_image, ScopedBarrierGuard<SrcState, DstState>(cmd, src_image, aspectOverride));
}

template <typename InState, typename OutState, typename T>
inline auto IssueBarrier(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride) {
    constexpr VkImageLayout in_layout  = LayoutMap<InState>::value;
    constexpr VkImageLayout out_layout = LayoutMap<OutState>::value;

    ImageSlice slice;
    if constexpr (requires { resource.Raw(); }) {
        slice = resource.Raw();
    } else if constexpr (requires { resource.AsSlice(); }) {
        slice = resource.AsSlice();
    } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, ImageSlice>) {
        slice = resource;
    } else if constexpr (requires { resource.image.Handle(); resource.view.Format(); }) {
        slice = ImageSlice {resource.image.Handle(), resource.view, resource.extent, resource.view.Format()};
    } else {
        static_assert(sizeof(T) == 0, "IssueBarrier requires an image slice or an owning image resource");
    }

    const VkImageAspectFlags aspect = aspectOverride != VK_IMAGE_ASPECT_NONE ? aspectOverride : slice.aspect;
    TransitionLayout<in_layout, out_layout>(cmd, slice.image, aspect);
    slice.aspect = aspect;
    return TypedImage<out_layout> {slice};
}

template <VkImageLayout NewLayout, VkImageLayout OldLayout, VkFormat Format>
inline auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout, Format>& img, VkImageAspectFlags overrideAspect) noexcept
    -> TypedImage<NewLayout, Format> {
    const VkImageAspectFlags aspect = overrideAspect != VK_IMAGE_ASPECT_NONE ? overrideAspect : img.Aspect();
    TransitionLayout<OldLayout, NewLayout>(cmd, img.Handle(), aspect);
    return img.template WithLayout<NewLayout>(aspect);
}

template <VkImageLayout TargetLayout, VkImageLayout OldLayout, VkFormat Format>
constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout, Format>& img, Tag<TargetLayout> /*unused*/) noexcept {
    return Transition<TargetLayout>(cmd, img);
}

// DynamicPass Implementation

template <size_t ColorCount, bool HasDepth, typename Formats>
template <VkImageLayout Layout, VkFormat Format>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddColor(
    const TypedImage<Layout, Format>& img,
    VkAttachmentLoadOp                loadOp,
    VkAttachmentStoreOp               storeOp,
    const ZHLN::Color4&               clearColor
) && noexcept -> DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type> {
    static_assert(ColorCount < kMaxColorAttachments, "ZHLN Error: DynamicPass exceeded maximum color attachments (8).");
    static_assert(Layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || Layout == VK_IMAGE_LAYOUT_GENERAL);

    _colors[ColorCount] = {
        .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext              = nullptr,
        .imageView          = img.View(),
        .imageLayout        = Layout,
        .resolveMode        = VK_RESOLVE_MODE_NONE,
        .resolveImageView   = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp             = loadOp,
        .storeOp            = storeOp,
        .clearValue         = {.color = {.float32 = {clearColor.r, clearColor.g, clearColor.b, clearColor.a}}}
    };
    _colorFormats[ColorCount] = img.GetFormat();

    return DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <typename... TypedImages>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddColorGroup(
    const std::tuple<TypedImages...>& imageTuple,
    VkAttachmentLoadOp                loadOp,
    VkAttachmentStoreOp               storeOp,
    const ZHLN::Color4&               clearColor
) && noexcept -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth,
                          typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type> {
    constexpr size_t added_count = sizeof...(TypedImages);
    static_assert(ColorCount + added_count <= kMaxColorAttachments, "ZHLN Error: DynamicPass exceeded maximum color attachments (8).");

    std::apply(
        [&](const auto&... img) {
            size_t offset = ColorCount;
            const auto add = [&](const auto& image) {
                const size_t index = offset++;
                _colorFormats[index] = image.GetFormat();
                _colors[index] = {
                    .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .pNext              = nullptr,
                    .imageView          = image.View(),
                    .imageLayout        = std::remove_cvref_t<decltype(image)>::layout,
                    .resolveMode        = VK_RESOLVE_MODE_NONE,
                    .resolveImageView   = VK_NULL_HANDLE,
                    .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .loadOp             = loadOp,
                    .storeOp            = storeOp,
                    .clearValue         = {.color = {.float32 = {clearColor.r, clearColor.g, clearColor.b, clearColor.a}}}
                };
            };
            (add(img), ...);
        },
        imageTuple
    );

    return DynamicPass<ColorCount + added_count, HasDepth,
                       typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <VkImageLayout Layout, VkFormat Format>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddDepth(
    const TypedImage<Layout, Format>& img,
    VkAttachmentLoadOp                loadOp,
    VkAttachmentStoreOp               storeOp,
    float                             clearVal
) && noexcept -> DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type> {
    static_assert(!HasDepth, "ZHLN Execution Error: Depth target already bound.");
    // Allow both depth-only and combined depth-stencil layouts
    static_assert(
        Layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL || Layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || Layout == VK_IMAGE_LAYOUT_GENERAL
    );

    _depthFormat = img.GetFormat();
    _hasStencil = StencilFormatForDepth(img.GetFormat()) != VK_FORMAT_UNDEFINED;

    _depth = {
        .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext              = nullptr,
        .imageView          = img.View(),
        .imageLayout        = Layout,
        .resolveMode        = VK_RESOLVE_MODE_NONE,
        .resolveImageView   = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp             = loadOp,
        .storeOp            = storeOp,
        .clearValue         = {.depthStencil = {.depth = clearVal, .stencil = 0}}
    };

    return DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth, typename Formats>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::Flags(VkRenderingFlags flags) && noexcept -> DynamicPass<ColorCount, HasDepth, Formats>&& {
    _flags = flags;
    return std::move(*this);
}

template <size_t ColorCount, bool HasDepth, typename Formats>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::GetSecondaryInheritance(
    const VkBindHeapInfoEXT* samplerHeap, const VkBindHeapInfoEXT* resourceHeap, std::span<const uint32_t> pushOffsets,
    std::span<const VkDeviceAddress> pushAddresses
) const noexcept -> SecondaryInheritance {
    std::span<const VkFormat> colors;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    if constexpr (std::same_as<Formats, RuntimeAttachmentFormats>) {
        // AssumeLayout yields runtime-format images even for a RenderTarget<F>.
        colors = std::span<const VkFormat> {_colorFormats.data(), ColorCount};
        if constexpr (HasDepth) {
            depth = _depthFormat;
        }
    } else {
        static_assert(Formats::color_formats.size() == ColorCount);
        static_assert((Formats::depth_format != VK_FORMAT_UNDEFINED) == HasDepth);
        colors = std::span<const VkFormat> {Formats::color_formats};
        if constexpr (HasDepth) {
            depth = Formats::depth_format;
        }
    }

    // Execute binds stencil only when AddDepth marked it active; use that
    // same decision rather than assuming a combined view always binds it.
    const VkFormat stencil = (HasDepth && _hasStencil) ? StencilFormatForDepth(depth) : VK_FORMAT_UNDEFINED;
    return SecondaryInheritance {colors, depth, stencil, _viewMask, samplerHeap, resourceHeap, pushOffsets, pushAddresses, EffectiveViewport()};
}

template <size_t ColorCount, bool HasDepth, typename Formats>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::EffectiveViewport() const noexcept -> VkViewport {
    const bool useVp = _vpW > 1.0F && _vpH > 1.0F;
    return {
        .x        = useVp ? _vpX : 0.0F,
        .y        = useVp ? _vpY : 0.0F,
        .width    = useVp ? _vpW : static_cast<float>(_extent.width),
        .height   = useVp ? _vpH : static_cast<float>(_extent.height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <typename Func>
void DynamicPass<ColorCount, HasDepth, Formats>::Execute(VkCommandBuffer cmd, Func&& func) const {
    VkRenderingInfo rendering_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext                = nullptr,
        .flags                = _flags,
        .renderArea           = {.offset = {0, 0}, .extent = {_extent.width, _extent.height}},
        .layerCount           = 1,
        .viewMask             = _viewMask,
        .colorAttachmentCount = ColorCount,
        .pColorAttachments    = ColorCount > 0 ? _colors.data() : nullptr,
        .pDepthAttachment     = GetDepthPtr(),
        .pStencilAttachment   = (HasDepth && _hasStencil) ? &_depth : nullptr,
    };

    vkCmdBeginRendering(cmd, &rendering_info);

    const VkViewport viewport = EffectiveViewport();
    const VkRect2D scissor = {
        .offset = {.x = static_cast<int32_t>(viewport.x), .y = static_cast<int32_t>(viewport.y)},
        .extent = {.width = static_cast<uint32_t>(viewport.width), .height = static_cast<uint32_t>(viewport.height)}
    };

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    std::forward<Func>(func)();

    vkCmdEndRendering(cmd);
}

template <size_t ColorCount, bool HasDepth, typename Formats>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::ViewMask(uint32_t mask) && noexcept -> DynamicPass<ColorCount, HasDepth, Formats>&& {
    _viewMask = mask;
    return std::move(*this);
}

template <size_t ColorCount, bool HasDepth, typename Formats>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::GetDepthPtr() const noexcept -> const VkRenderingAttachmentInfo* {
    if constexpr (HasDepth) {
        return &_depth;
    } else {
        return nullptr;
    }
}

template <size_t MaxStackBarriers>
inline void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept {
    std::array<VkImageMemoryBarrier2, MaxStackBarriers> stack_barriers;

    for (const auto& pass: passes) {
        const auto transition_count = static_cast<uint32_t>(pass.transitions.size());

        if (transition_count > 0) {
            VkImageMemoryBarrier2* p_barriers     = stack_barriers.data();
            VkImageMemoryBarrier2* heap_allocated = nullptr;

            if (transition_count > MaxStackBarriers) [[unlikely]] {
                heap_allocated = new (std::nothrow) VkImageMemoryBarrier2[transition_count];
                p_barriers     = heap_allocated;
            }

            for (uint32_t i = 0; i < transition_count; ++i) {
                p_barriers[i] = MakeImageBarrier(pass.transitions[i].barrier);
            }

            PipelineBarrier(cmd, {}, std::span<const VkImageMemoryBarrier2>(p_barriers, transition_count));

            if (heap_allocated) [[unlikely]] {
                delete[] heap_allocated;
            }
        }

        if (pass.record) {
            pass.record(cmd, pass.pUserData);
        }
    }
}

// Attachment Clear Helpers (wraps vkCmdClearAttachments for in-pass clears)

// Clears one attachment region inside the current render pass instance.
inline void ClearAttachment(
    VkCommandBuffer     cmd,
    VkImageAspectFlags  aspectMask,
    VkExtent2D          extent,
    const VkClearValue& value,
    uint32_t            baseArrayLayer = 0,
    uint32_t            layerCount     = 1
) noexcept {
    const VkClearAttachment attachment = {
        .aspectMask      = aspectMask,
        .colorAttachment = 0,
        .clearValue      = value,
    };
    const VkClearRect rect = {
        .rect           = {.offset = {0, 0}, .extent = extent},
        .baseArrayLayer = baseArrayLayer,
        .layerCount     = layerCount,
    };
    vkCmdClearAttachments(cmd, 1, &attachment, 1, &rect);
}

// Clears the stencil aspect of the bound depth/stencil attachment (CSG passes).
inline void ClearStencilAttachment(VkCommandBuffer cmd, VkExtent2D extent, uint32_t stencil = 0) noexcept {
    ClearAttachment(cmd, VK_IMAGE_ASPECT_STENCIL_BIT, extent, {.depthStencil = {.depth = 1.0f, .stencil = stencil}});
}

} // namespace ZHLN::Vk
