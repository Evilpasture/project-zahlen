// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {

// ============================================================================
// Centralized Layout State Translation Engine
// ============================================================================

template <VkImageLayout Layout>
struct LayoutTraits {
  private:
    struct LayoutSyncInfo {
        VkPipelineStageFlags2 stage;
        VkAccessFlags2        access;
    };
    static constexpr LayoutSyncInfo GetSyncInfo(bool isSource) {
        switch (Layout) {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                return {.stage = VK_PIPELINE_STAGE_2_NONE, .access = 0};

            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return {
                    .stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .access = isSource ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT :
                                         (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)
                };

            case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return {
                    .stage  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .access = isSource ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT :
                                         (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                };

            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .access = VK_ACCESS_2_SHADER_READ_BIT};

            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                return {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = 0};

            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_WRITE_BIT};

            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return {.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .access = VK_ACCESS_2_TRANSFER_READ_BIT};

            case VK_IMAGE_LAYOUT_GENERAL:
                return {
                    .stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .access = isSource ? (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT) :
                                         (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)
                };

            default:
                return {.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .access = isSource ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT};
        }
    }

  public:
    static constexpr auto kInfo = GetSyncInfo(false);

    static constexpr VkPipelineStageFlags2 kStage = []() constexpr {
        if constexpr (Layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
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
inline void TransitionLayout(const VkCommandBuffer cmd, const VkImage image, const VkImageAspectFlags aspect) noexcept {
    using Src = LayoutTraits<OldLayout>;
    using Dst = LayoutTraits<NewLayout>;

    const ZHLN_ImageBarrierDesc barrier = {
        .image      = image,
        .src_access = Src::kAccess,
        .dst_access = Dst::kAccess,
        .src_layout = OldLayout,
        .dst_layout = NewLayout,
        .src_stage  = Src::kStage,
        .dst_stage  = Dst::kStage,
        .aspect     = aspect,
        .base_mip   = 0,
        .mip_count  = VK_REMAINING_MIP_LEVELS
    };

    ZHLN_CmdImageBarrier(cmd, &barrier);
}

// ============================================================================
// Scoped RAII Layout Transition Implementations
// ============================================================================

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
        auto state = resource.State();
        src_image  = {state.handle, state.view, state.extent, state.aspect, state.format};
    } else {
        src_image = {resource.handle, resource.view, resource.extent, resource.aspect, resource.format};
    }

    return std::make_pair(transitioned_image, ScopedBarrierGuard<SrcState, DstState>(cmd, src_image, aspectOverride));
}

template <typename T>
struct TargetFormat;

template <typename InState, typename OutState, typename T>
inline auto IssueBarrier(VkCommandBuffer cmd, const T& resource, VkImageAspectFlags aspectOverride) {
    constexpr VkImageLayout in_layout  = LayoutMap<InState>::value;
    constexpr VkImageLayout out_layout = LayoutMap<OutState>::value;

    VkImage            image = VK_NULL_HANDLE;
    VkImageView        view  = VK_NULL_HANDLE;
    VkExtent2D         extent {};
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormat           format = VK_FORMAT_UNDEFINED;

    if constexpr (requires { resource.handle; }) {
        image  = resource.handle;
        view   = resource.view;
        extent = resource.extent;
        aspect = resource.aspect;
        format = resource.format;
    } else if constexpr (requires { resource.image.Handle(); }) {
        image      = resource.image.Handle();
        view       = resource.view.Get();
        extent     = resource.extent;
        aspect     = resource.State().aspect;
        using RawT = std::decay_t<T>;
        if constexpr (requires { TargetFormat<RawT>::value; }) {
            format = TargetFormat<RawT>::value;
        }
    }

    if (aspectOverride != VK_IMAGE_ASPECT_NONE) {
        aspect = aspectOverride;
    }

    TransitionLayout<in_layout, out_layout>(cmd, image, aspect);

    return TypedImage<out_layout> {.handle = image, .view = view, .extent = extent, .aspect = aspect, .format = format};
}

template <VkImageLayout NewLayout, VkImageLayout OldLayout>
inline auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img, VkImageAspectFlags overrideAspect) noexcept -> TypedImage<NewLayout> {
    VkImageAspectFlags aspect = (overrideAspect != VK_IMAGE_ASPECT_NONE) ? overrideAspect : img.aspect;
    TransitionLayout<OldLayout, NewLayout>(cmd, img.handle, aspect);
    return TypedImage<NewLayout> {.handle = img.handle, .view = img.view, .extent = img.extent, .aspect = img.aspect, .format = img.format};
}

template <VkImageLayout TargetLayout, VkImageLayout OldLayout>
constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout>& img, Tag<TargetLayout> /*unused*/) noexcept {
    return Transition<TargetLayout>(cmd, img);
}

// ============================================================================
// DynamicPass Implementation
// ============================================================================

template <size_t ColorCount, bool HasDepth>
template <VkImageLayout Layout>
constexpr auto DynamicPass<ColorCount, HasDepth>::AddColor(
    const TypedImage<Layout>& img,
    VkAttachmentLoadOp        loadOp,
    VkAttachmentStoreOp       storeOp,
    const ZHLN::Color4&       clearColor
) && noexcept -> DynamicPass<ColorCount + 1, HasDepth> {
    static_assert(ColorCount < kMaxColorAttachments, "ZHLN Error: DynamicPass exceeded maximum color attachments (8).");
    static_assert(Layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || Layout == VK_IMAGE_LAYOUT_GENERAL);

    _colors[ColorCount] = {
        .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext              = nullptr,
        .imageView          = img.view,
        .imageLayout        = Layout,
        .resolveMode        = VK_RESOLVE_MODE_NONE,
        .resolveImageView   = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp             = loadOp,
        .storeOp            = storeOp,
        .clearValue         = {.color = {.float32 = {clearColor.r, clearColor.g, clearColor.b, clearColor.a}}}
    };

    return DynamicPass<ColorCount + 1, HasDepth>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth>
template <typename... TypedImages>
constexpr auto DynamicPass<ColorCount, HasDepth>::AddColorGroup(
    const std::tuple<TypedImages...>& imageTuple,
    VkAttachmentLoadOp                loadOp,
    VkAttachmentStoreOp               storeOp,
    const ZHLN::Color4&               clearColor
) && noexcept -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth> {
    constexpr size_t added_count = sizeof...(TypedImages);
    static_assert(ColorCount + added_count <= kMaxColorAttachments, "ZHLN Error: DynamicPass exceeded maximum color attachments (8).");

    std::apply(
        [&](const auto&... img) {
            size_t offset = ColorCount;
            ((_colors[offset++] =
                  {.sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                   .pNext              = nullptr,
                   .imageView          = img.view,
                   .imageLayout        = std::remove_cvref_t<decltype(img)>::layout,
                   .resolveMode        = VK_RESOLVE_MODE_NONE,
                   .resolveImageView   = VK_NULL_HANDLE,
                   .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                   .loadOp             = loadOp,
                   .storeOp            = storeOp,
                   .clearValue         = {.color = {.float32 = {clearColor.r, clearColor.g, clearColor.b, clearColor.a}}}}),
             ...);
        },
        imageTuple
    );

    return DynamicPass<ColorCount + added_count, HasDepth>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth>
template <VkImageLayout Layout>
constexpr auto DynamicPass<ColorCount, HasDepth>::AddDepth(
    const TypedImage<Layout>& img,
    VkAttachmentLoadOp        loadOp,
    VkAttachmentStoreOp       storeOp,
    float                     clearVal
) && noexcept -> DynamicPass<ColorCount, true> {
    static_assert(!HasDepth, "ZHLN Execution Error: Depth target already bound to this pass.");
    static_assert(Layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL || Layout == VK_IMAGE_LAYOUT_GENERAL);

    _depth = {
        .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext              = nullptr,
        .imageView          = img.view,
        .imageLayout        = Layout,
        .resolveMode        = VK_RESOLVE_MODE_NONE,
        .resolveImageView   = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp             = loadOp,
        .storeOp            = storeOp,
        .clearValue         = {.depthStencil = {.depth = clearVal, .stencil = 0}}
    };

    return DynamicPass<ColorCount, true>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth>
constexpr auto DynamicPass<ColorCount, HasDepth>::Flags(VkRenderingFlags flags) && noexcept -> DynamicPass<ColorCount, HasDepth>&& {
    _flags = flags;
    return std::move(*this);
}

template <size_t ColorCount, bool HasDepth>
template <typename Func>
void DynamicPass<ColorCount, HasDepth>::Execute(VkCommandBuffer cmd, Func&& func) const {
    VkRenderingInfo rendering_info = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext                = nullptr,
        .flags                = _flags,
        .renderArea           = {.offset = {0, 0}, .extent = _extent},
        .layerCount           = 1,
        .viewMask             = _viewMask,
        .colorAttachmentCount = ColorCount,
        .pColorAttachments    = ColorCount > 0 ? _colors.data() : nullptr,
        .pDepthAttachment     = GetDepthPtr(),
        .pStencilAttachment   = nullptr,
    };

    vkCmdBeginRendering(cmd, &rendering_info);

    const VkViewport viewport = {
        .x        = 0.0F,
        .y        = 0.0F,
        .width    = (float) _extent.width,
        .height   = (float) _extent.height,
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = _extent};

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    std::forward<Func>(func)();

    vkCmdEndRendering(cmd);
}

template <size_t ColorCount, bool HasDepth>
constexpr auto DynamicPass<ColorCount, HasDepth>::ViewMask(uint32_t mask) && noexcept -> DynamicPass<ColorCount, HasDepth>&& {
    _viewMask = mask;
    return std::move(*this);
}

template <size_t ColorCount, bool HasDepth>
constexpr auto DynamicPass<ColorCount, HasDepth>::GetDepthPtr() const noexcept -> const VkRenderingAttachmentInfo* {
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
                const auto& res = pass.transitions[i];
                p_barriers[i]   = {
                    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .pNext               = nullptr,
                    .srcStageMask        = res.barrier.src_stage,
                    .srcAccessMask       = res.barrier.src_access,
                    .dstStageMask        = res.barrier.dst_stage,
                    .dstAccessMask       = res.barrier.dst_access,
                    .oldLayout           = res.barrier.src_layout,
                    .newLayout           = res.barrier.dst_layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image               = res.barrier.image,
                    .subresourceRange    = {
                        .aspectMask     = res.barrier.aspect,
                        .baseMipLevel   = 0,
                        .levelCount     = VK_REMAINING_MIP_LEVELS,
                        .baseArrayLayer = 0,
                        .layerCount     = VK_REMAINING_ARRAY_LAYERS,
                    },
                };
            }

            const VkDependencyInfo dep_info = {
                .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext                    = nullptr,
                .dependencyFlags          = {},
                .memoryBarrierCount       = {},
                .pMemoryBarriers          = {},
                .bufferMemoryBarrierCount = {},
                .pBufferMemoryBarriers    = {},
                .imageMemoryBarrierCount  = transition_count,
                .pImageMemoryBarriers     = p_barriers,
            };
            vkCmdPipelineBarrier2(cmd, &dep_info);

            if (heap_allocated) [[unlikely]] {
                delete[] heap_allocated;
            }
        }

        if (pass.record) {
            pass.record(cmd, pass.pUserData);
        }
    }
}

} // namespace ZHLN::Vk
