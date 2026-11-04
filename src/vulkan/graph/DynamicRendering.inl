// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {

template <ColorClearTarget T>
constexpr auto GetVkImage(const T& target) noexcept -> VkImage {
    if constexpr (std::same_as<std::remove_cvref_t<T>, VkImage>) {
        return target;
    } else if constexpr (requires {
                             { target.image.Handle() } -> std::same_as<VkImage>;
                         }) {
        return target.image.Handle();
    } else if constexpr (requires {
                             { target.Handle() } -> std::same_as<VkImage>;
                         }) {
        return target.Handle();
    } else {
        return target.image;
    }
}

constexpr auto ToVkClearColor(ZHLN::Vk::Color4 color) noexcept -> VkClearColorValue {
    return {.float32 = {color.r, color.g, color.b, color.a}};
}

constexpr auto ToVkClearColor(float scalar) noexcept -> VkClearColorValue {
    return {.float32 = {scalar, scalar, scalar, scalar}};
}

constexpr auto ToVkClearColor(VkClearColorValue color) noexcept -> VkClearColorValue {
    return color;
}

template <Vk::ImageLayout InitialLayout, Vk::ImageLayout FinalLayout, ColorClearTarget Target, ClearColorSource Color>
inline void ClearColorAndTransition(VkCommandBuffer cmd, const Target& target, const Color& color, const VkImageSubresourceRange range) noexcept {
    static_assert(FinalLayout != Vk::ImageLayout::Undefined, "A cleared image cannot finish in UNDEFINED layout.");
    const VkImage image = GetVkImage(target);
    if (image == VK_NULL_HANDLE) {
        return;
    }

    const VkClearColorValue clear = ToVkClearColor(color);
    ImageBarrier::Transition<InitialLayout, Vk::ImageLayout::TransferDstOptimal>(image, range).Record(cmd);
    vkCmdClearColorImage(cmd, image, ToVk(Vk::ImageLayout::TransferDstOptimal), &clear, 1, &range);
    ImageBarrier::Transition<Vk::ImageLayout::TransferDstOptimal, FinalLayout>(image, range).Record(cmd);
}

template <Vk::ImageLayout InitialLayout, Vk::ImageLayout FinalLayout, ClearColorSource Color, ColorClearTarget... Targets>
    requires(sizeof...(Targets) > 1)
inline void ClearColorAndTransition(VkCommandBuffer cmd, const Color& color, const Targets&... targets) noexcept {
    (ClearColorAndTransition<InitialLayout, FinalLayout>(cmd, targets, color), ...);
}

static_assert(ColorClearTarget<VkImage> && ColorClearTarget<ImageSlice> && !ColorClearTarget<Color4>);

static_assert(std::same_as<VkImage, VkImageView> || !ColorClearTarget<VkImageView>);
static_assert(std::same_as<VkImage, VkBuffer> || !ColorClearTarget<VkBuffer>);
static_assert(ToVkClearColor(Color4 {.r = 0.25F, .g = 0.5F, .b = 0.75F, .a = 1.0F}).float32[2] == 0.75F);
static_assert(ToVkClearColor(1.0F).float32[3] == 1.0F);
static_assert(ToVkClearColor(VkClearColorValue {.float32 = {0.1F, 0.2F, 0.3F, 0.4F}}).float32[1] == 0.2F);

inline void ClearColorImage(const VkCommandBuffer cmd, const VkImage image, VkClearColorValue color, const uint32_t layerCount) noexcept {
    const VkImageSubresourceRange range {
        .aspectMask     = ToVk(Vk::ImageAspect::Color),
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = layerCount,
    };
    ClearColorAndTransition<Vk::ImageLayout::Undefined, Vk::ImageLayout::ColorAttachmentOptimal>(cmd, image, color, range);
}

template <typename SrcState, typename DstState>
ScopedBarrierGuard<SrcState, DstState>::ScopedBarrierGuard(
    VkCommandBuffer                               c,
    const TypedImage<LayoutMap<SrcState>::value>& res,
    Vk::ImageAspect                            aspect
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
auto ScopedBarrier(VkCommandBuffer cmd, const T& resource, Vk::ImageAspect aspectOverride) noexcept {
    auto transitioned_image = IssueBarrier<SrcState, DstState>(cmd, resource, aspectOverride);

    constexpr Vk::ImageLayout src_layout = LayoutMap<SrcState>::value;
    TypedImage<src_layout>  src_image;
    if constexpr (requires { resource.State(); }) {
        src_image = TypedImage<src_layout> {resource.State().Raw()};
    } else if constexpr (requires { resource.Raw(); }) {
        src_image = TypedImage<src_layout> {resource.Raw()};
    } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, ImageSlice>) {
        src_image = TypedImage<src_layout> {resource};
    } else {
        src_image = TypedImage<src_layout> {resource.AsSlice()};
    }

    return std::make_pair(transitioned_image, ScopedBarrierGuard<SrcState, DstState>(cmd, src_image, aspectOverride));
}

template <typename InState, typename OutState, typename T>
inline auto IssueBarrier(VkCommandBuffer cmd, const T& resource, Vk::ImageAspect aspectOverride) {
    constexpr Vk::ImageLayout in_layout  = LayoutMap<InState>::value;
    constexpr Vk::ImageLayout out_layout = LayoutMap<OutState>::value;

    ImageSlice slice;
    if constexpr (requires { resource.Raw(); }) {
        slice = resource.Raw();
    } else if constexpr (requires { resource.AsSlice(); }) {
        slice = resource.AsSlice();
    } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, ImageSlice>) {
        slice = resource;
    } else {
        static_assert(sizeof(T) == 0, "IssueBarrier requires an image slice or an owning image resource");
    }

    slice.aspect = aspectOverride != Vk::ImageAspect::None ? aspectOverride : slice.aspect;
    slice.template Transition<in_layout, out_layout>(cmd);
    return TypedImage<out_layout> {slice};
}

template <Vk::ImageLayout NewLayout, Vk::ImageLayout OldLayout, Vk::Format Format>
inline auto
    Transition(VkCommandBuffer cmd, const TypedImage<OldLayout, Format>& img, Vk::ImageAspect overrideAspect) noexcept -> TypedImage<NewLayout, Format> {
    const Vk::ImageAspect aspect = overrideAspect != Vk::ImageAspect::None ? overrideAspect : img.Aspect();
    auto                     next   = img.template WithLayout<NewLayout>(aspect);
    if constexpr (OldLayout != NewLayout) {
        next.slice.template Transition<OldLayout, NewLayout>(cmd);
    }
    return next;
}

template <Vk::ImageLayout TargetLayout, Vk::ImageLayout OldLayout, Vk::Format Format>
constexpr auto Transition(VkCommandBuffer cmd, const TypedImage<OldLayout, Format>& img, Tag<TargetLayout> /*unused*/) noexcept {
    return Transition<TargetLayout>(cmd, img);
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <Vk::ImageLayout Layout, Vk::Format Format>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddColor(
    const TypedImage<Layout, Format>& img,
    Vk::AttachmentLoadOp                loadOp,
    Vk::AttachmentStoreOp               storeOp,
    ZHLN::Vk::Color4                  clearColor
) && noexcept -> DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type> {
    static_assert(ColorCount < kMaxColorAttachments, "ZHLN Error: DynamicPass exceeded maximum color attachments (8).");
    static_assert(Layout == Vk::ImageLayout::ColorAttachmentOptimal || Layout == Vk::ImageLayout::General);

    _colors[ColorCount] = {
        .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext              = nullptr,
        .imageView          = img.AttachmentView(),
        .imageLayout        = ToVk(Layout),
        .resolveMode        = VK_RESOLVE_MODE_NONE,
        .resolveImageView   = VK_NULL_HANDLE,
        .resolveImageLayout = ToVk(Vk::ImageLayout::Undefined),
        .loadOp             = ToVk(loadOp),
        .storeOp            = ToVk(storeOp),
        .clearValue         = {.color = {.float32 = {clearColor.r, clearColor.g, clearColor.b, clearColor.a}}}
    };
    _colorFormats[ColorCount] = img.GetFormat();

    return DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <typename... TypedImages>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddColorGroup(
    const std::tuple<TypedImages...>& imageTuple,
    Vk::AttachmentLoadOp                loadOp,
    Vk::AttachmentStoreOp               storeOp,
    ZHLN::Vk::Color4                  clearColor
) && noexcept -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth, typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type> {
    constexpr size_t added_count = sizeof...(TypedImages);
    static_assert(ColorCount + added_count <= kMaxColorAttachments, "ZHLN Error: DynamicPass exceeded maximum color attachments (8).");

    std::apply(
        [&](const auto&... img) {
            size_t     offset = ColorCount;
            const auto add    = [&](const auto& image) {
                const size_t index   = offset++;
                _colorFormats[index] = image.GetFormat();
                _colors[index]       = {
                    .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .pNext              = nullptr,
                    .imageView          = image.AttachmentView(),
                    .imageLayout        = ToVk(std::remove_cvref_t<decltype(image)>::layout),
                    .resolveMode        = VK_RESOLVE_MODE_NONE,
                    .resolveImageView   = VK_NULL_HANDLE,
                    .resolveImageLayout = ToVk(Vk::ImageLayout::Undefined),
                    .loadOp             = ToVk(loadOp),
                    .storeOp            = ToVk(storeOp),
                    .clearValue         = {.color = {.float32 = {clearColor.r, clearColor.g, clearColor.b, clearColor.a}}}
                };
            };
            (add(img), ...);
        },
        imageTuple
    );

    return DynamicPass<ColorCount + added_count, HasDepth, typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <Vk::ImageLayout Layout, Vk::Format Format>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddDepth(
    const TypedImage<Layout, Format>& img,
    Vk::AttachmentLoadOp                loadOp,
    Vk::AttachmentStoreOp               storeOp,
    float                             clearVal
) && noexcept -> DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type> {
    static_assert(!HasDepth, "ZHLN Execution Error: Depth target already bound.");

    static_assert(
        Layout == Vk::ImageLayout::DepthAttachmentOptimal || Layout == Vk::ImageLayout::DepthStencilAttachmentOptimal || Layout == Vk::ImageLayout::General
    );

    _depthFormat = img.GetFormat();
    _hasStencil  = StencilFormatForDepth(img.GetFormat()) != Vk::Format::Undefined;

    _depth = {
        .sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext              = nullptr,
        .imageView          = img.AttachmentView(),
        .imageLayout        = ToVk(Layout),
        .resolveMode        = VK_RESOLVE_MODE_NONE,
        .resolveImageView   = VK_NULL_HANDLE,
        .resolveImageLayout = ToVk(Vk::ImageLayout::Undefined),
        .loadOp             = ToVk(loadOp),
        .storeOp            = ToVk(storeOp),
        .clearValue         = {.depthStencil = {.depth = clearVal, .stencil = 0}}
    };

    return DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type>(std::move(*this));
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <Vk::ImageLayout Layout, Vk::Format Format>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddColor(const TypedImage<Layout, Format>& img, ColorOp op) && noexcept
    -> DynamicPass<ColorCount + 1, HasDepth, typename AppendAttachmentColors<Formats, Format>::type> {
    return std::move(*this).AddColor(img, op.loadOp, op.storeOp, op.clearColor);
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <typename... TypedImages>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddColorGroup(const std::tuple<TypedImages...>& imageTuple, ColorOp op) && noexcept
    -> DynamicPass<ColorCount + sizeof...(TypedImages), HasDepth, typename AppendAttachmentColors<Formats, TypedImages::known_format...>::type> {
    return std::move(*this).AddColorGroup(imageTuple, op.loadOp, op.storeOp, op.clearColor);
}

template <size_t ColorCount, bool HasDepth, typename Formats>
template <Vk::ImageLayout Layout, Vk::Format Format>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::AddDepth(const TypedImage<Layout, Format>& img, DepthOp op) && noexcept
    -> DynamicPass<ColorCount, true, typename SetAttachmentDepth<Formats, Format>::type> {
    return std::move(*this).AddDepth(img, op.loadOp, op.storeOp, op.clearVal);
}

template <size_t ColorCount, bool HasDepth, typename Formats>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::Flags(VkRenderingFlags flags) && noexcept -> DynamicPass<ColorCount, HasDepth, Formats>&& {
    _flags = flags;
    return std::move(*this);
}

template <size_t ColorCount, bool HasDepth, typename Formats>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::GetInheritance() const noexcept -> RenderingInheritance {
    std::span<const Vk::Format> colors;
    Vk::Format                  depth = Vk::Format::Undefined;
    if constexpr (std::same_as<Formats, RuntimeAttachmentFormats>) {
        colors = std::span<const Vk::Format> {_colorFormats.data(), ColorCount};
        if constexpr (HasDepth) {
            depth = _depthFormat;
        }
    } else {
        static_assert(Formats::color_formats.size() == ColorCount);
        static_assert((Formats::depth_format != Vk::Format::Undefined) == HasDepth);
        colors = std::span<const Vk::Format> {Formats::color_formats};
        if constexpr (HasDepth) {
            depth = Formats::depth_format;
        }
    }

    const Vk::Format stencil = (HasDepth && _hasStencil) ? StencilFormatForDepth(depth) : Vk::Format::Undefined;
    return RenderingInheritance {colors, depth, stencil, _viewMask, EffectiveViewport()};
}

template <size_t ColorCount, bool HasDepth, typename Formats>
constexpr auto DynamicPass<ColorCount, HasDepth, Formats>::EffectiveViewport() const noexcept -> VkViewport {
    const bool use_vp = _vpW > 1.0F && _vpH > 1.0F;
    return {
        .x        = use_vp ? _vpX : 0.0F,
        .y        = use_vp ? _vpY : 0.0F,
        .width    = use_vp ? _vpW : static_cast<float>(_extent.width),
        .height   = use_vp ? _vpH : static_cast<float>(_extent.height),
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

    ApplyViewportAndScissor(cmd, EffectiveViewport());

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

inline void ExecutePasses(VkCommandBuffer cmd, std::span<const PassDesc> passes) noexcept {
    for (const auto& pass: passes) {
        if (!pass.transitions.empty()) {
            ImageBarrier::Record(cmd, pass.transitions);
        }
        if (pass.record) {
            pass.record(cmd, pass.pUserData);
        }
    }
}

inline void ClearAttachment(
    VkCommandBuffer    cmd,
    Vk::ImageAspect aspectMask,
    VkExtent2D         extent,
    VkClearValue       value,
    uint32_t           baseArrayLayer,
    uint32_t           layerCount
) noexcept {
    const VkClearAttachment attachment = {
        .aspectMask      = ToVk(aspectMask),
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

inline void ClearStencilAttachment(VkCommandBuffer cmd, VkExtent2D extent, uint32_t stencil = 0) noexcept {
    ClearAttachment(cmd, Vk::ImageAspect::Stencil, extent, {.depthStencil = {.depth = 1.0F, .stencil = stencil}});
}

} // namespace ZHLN::Vk
