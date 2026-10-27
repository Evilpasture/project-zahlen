// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresentationSurface.hpp"
#include "RenderInternal.hpp"
#include <Zahlen/Log.hpp>
#include <memory>
#include <utility>

namespace ZHLN {

namespace {

[[nodiscard]] auto AcquireDestinationImage(
    FrameDestinations& destinations, uint64_t& nextAcquisitionSerial, FrameDestinations::Window& dest, Vk::InstanceView instance
) noexcept
    -> std::expected<ZHLN::Optional<FrameDestinations::Acquired&>, ErrorCode> {
    if (dest.acquired.has_value()) {
        return *dest.acquired;
    }

    if (dest.target == nullptr) {
        return std::nullopt;
    }

    Vk::SwapchainPresenter& destPresenter = dest.Presenter();
    const Extent2D          size          = dest.target->GetFramebufferExtent();
    auto                    acquired      = destPresenter.AcquireNext(VkExtent2D {.width = size.width, .height = size.height}, !dest.IsPrimary());

    if (!acquired) {
        if (Vk::IsDeviceLost(acquired.error())) {
            instance.IncrementDeviceLost();
        }
        destinations.AbortRecording(dest.id);
        dest.cachedGeneration = destPresenter.resourceGeneration;
        return std::unexpected(AsFrameError(acquired.error()));
    }

    if (!acquired->has_value()) {
        destinations.AbortRecording(dest.id);
        dest.cachedGeneration = destPresenter.resourceGeneration;
        return std::nullopt;
    }

    const Vk::SwapchainTarget& image = **acquired;
    if (dest.cachedGeneration != 0 && dest.cachedGeneration != image.generation) {
        ZHLN::Log("[Render] Window presentation resources rebuilt (generation {} -> {}).", dest.cachedGeneration, image.generation);
    }
    dest.cachedGeneration = image.generation;

    return dest.acquired.emplace(
        FrameDestinations::Acquired {
            .image      = image.image,
            .imageIndex = image.imageIndex,
            .serial     = nextAcquisitionSerial++,
        }
    );
}

} // namespace

auto RenderContext::Impl::EnsureDepthTarget(FrameDestinations::Window& destination) noexcept -> std::expected<void, ErrorCode> {
    const VkExtent2D extent = destination.Presenter().Extent();
    if (extent.width == 0 || extent.height == 0) {
        return std::unexpected(DestinationError::SurfaceUnusable);
    }

    auto& target = destination.IsPrimary() ? primaryDepthTarget : destination.depthTarget;
    if (target.Valid() && target.extent.width == extent.width && target.extent.height == extent.height) {
        return {};
    }

    auto created = Vk::FormatRenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
        allocator, ctx.Device(), extent, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled}
    );
    if (!created) {
        return std::unexpected(created.error());
    }

    target.Destroy(allocator);
    target = std::move(*created);

    if (destination.IsPrimary()) {
        if (auto immediate = Vk::ExecuteImmediate(
                ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
                    target.DiscardTo<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(cmd);
                    target.Transition<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
                }
            );
            !immediate) {
            target.Destroy(allocator);
            return std::unexpected(immediate.error());
        }
        if (decalDepthSlot.Valid()) {
            heapManager.BindImage(decalDepthSlot, target.image.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        Vk::Debug::SetImageName(ctx, target.image.Handle(), "DepthTarget");
    } else {
        Vk::Debug::SetImageName(ctx, target.image.Handle(), "WindowDepthTarget");
    }

    return {};
}

auto RenderContext::Impl::FindOrCreateDestination(const PresentationTarget& aux, bool primary) noexcept -> std::expected<DestinationVend, ErrorCode> {
    if (auto existing = destinations.Find(aux)) {
        return DestinationVend {.entry = &*existing, .created = false};
    }
    if (destinations.Full()) {
        return std::unexpected(DestinationError::TooManyWindows);
    }

    FrameDestinations::Window dest {};
    dest.target = &aux;

    if (!primary) {
        if (presentationMode != PresentationMode::NativeSwapchain) {
            return std::unexpected(DestinationError::NativeSwapchainRequired);
        }
        if (ctx.Device() == VK_NULL_HANDLE) {
            return std::unexpected(DestinationError::DeviceUnavailable);
        }

        const Extent2D extent     = aux.GetFramebufferExtent();
        auto           surfaceRes = CreateSurfaceFromNative(ctx.Instance().Handle(), aux.GetNativeSurface());
        if (!surfaceRes) {
            return std::unexpected(ErrorCode {surfaceRes.error()});
        }

        auto owned     = std::make_unique<Vk::SwapchainPresenter>();
        owned->surface = Vk::Surface(ctx.Instance().Handle(), surfaceRes->Release());
        if (owned->surface.Get() == VK_NULL_HANDLE || extent.width == 0 || extent.height == 0) {
            return std::unexpected(DestinationError::SurfaceUnusable);
        }
        if (auto initRes = owned->Init(ctx, allocator, extent.width, extent.height, ctx.PhysicalInfo().graphicsFamily, true); !initRes) {
            return std::unexpected(initRes.error());
        }
        if (owned->GetPresentFormat() != presenter.GetPresentFormat()) {
            return std::unexpected(DestinationError::PresentFormatMismatch);
        }
        dest.presenter      = owned.get();
        dest.ownedPresenter = std::move(owned);
    } else {
        dest.presenter = &presenter;
    }

    if (auto entry = destinations.Attach(std::move(dest))) {
        return DestinationVend {.entry = &*entry, .created = true};
    }
    return std::unexpected(DestinationError::TooManyWindows);
}

auto RenderContext::Impl::TargetAttachment(const PresentationTarget& aux) const noexcept -> std::optional<FrameTarget> {
    if (!frameOpen) {
        return std::nullopt;
    }
    const auto dest = destinations.Find(aux);
    if (!dest || !dest->acquired) {
        return std::nullopt;
    }
    const auto recording = destinations.FindRecording(dest->id);
    if (!recording || !recording->recorder.IsRecording()) {
        return std::nullopt;
    }
    return FrameTarget {rendererId, frameSerial, dest->id, dest->acquired->serial};
}

auto RenderContext::Impl::AcquireTarget(const PresentationTarget& aux) noexcept -> FrameOutcome<FrameTarget> {
    if (!frameOpen) {
        return std::unexpected(DestinationError::NoActiveFrame);
    }

    auto found = FindOrCreateDestination(aux, &aux == &presentationTarget);
    if (!found) {
        return std::unexpected(found.error());
    }
    FrameDestinations::Window& dest = *found->entry;
    if (found->created) {
        ZHLN::Log("[Render] Window presenter created for {:p} (primary={}).", static_cast<const void*>(dest.target), dest.IsPrimary() ? 1 : 0);
    }

    // Rebuild an auxiliary swapchain before the renderer allocates its matching
    // scene-depth image. AcquireNext can still handle out-of-date images below.
    if (!dest.acquired && !dest.IsPrimary() && dest.Presenter().HasSwapchain()) {
        const Extent2D requested = aux.GetFramebufferExtent();
        const VkExtent2D current = dest.Presenter().Extent();
        if (requested.width != 0 && requested.height != 0 &&
            (requested.width != current.width || requested.height != current.height)) {
            if (auto rebuilt = dest.Presenter().Rebuild(requested.width, requested.height); !rebuilt) {
                return std::unexpected(rebuilt.error());
            }
        }
    }

    if (!dest.acquired) {
        if (auto depth = EnsureDepthTarget(dest); !depth) {
            return std::unexpected(depth.error());
        }
    }

    const auto acquired = AcquireDestinationImage(destinations, nextAcquisition, dest, ctx.Instance());
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    if (!acquired->has_value()) {
        return std::nullopt;
    }

    if (const auto existing = destinations.FindRecording(dest.id)) {
        if (!existing->recorder.IsRecording()) {
            return std::unexpected(DestinationError::ExpiredFrameTarget);
        }
    } else {
        Vk::SwapchainPresenter& destPresenter = dest.Presenter();

        // This is where a window's graphics command stream begins -- one stream
        // carrying the scene graph, its fork secondaries' setup, the Blit pass
        // and the UI pass -- so this is where BeginStream establishes its heap
        // state: both segments bound and the frame address block pushed, before
        // the first command. The state survives until the buffer is reset,
        // because HeapManager writes into one pair of device-addressable heap
        // buffers that are created once and never rebound with other contents.
        auto recording = BeginStream(destPresenter.SlotCommand(destPresenter.frameIndex));
        if (!recording) {
            dest.acquired.reset();
            return std::unexpected(recording.error());
        }

        // From here on the stream is the destination table's, and it only needs
        // the handle: the heap state lives on the command buffer, not in the type.
        destinations.AddRecording(dest.id, std::move(*recording).Release());
    }
    destinations.SetActive(dest.id);
    return FrameTarget {rendererId, frameSerial, dest.id, dest.acquired->serial};
}

auto RenderContext::Impl::ResolveTarget(const FrameTarget& target) noexcept -> std::expected<ResolvedTarget, ErrorCode> {
    if (!frameOpen) {
        return std::unexpected(DestinationError::NoActiveFrame);
    }
    if (target._renderer != rendererId || target._frame != frameSerial || !target.Valid()) {
        return std::unexpected(DestinationError::ExpiredFrameTarget);
    }
    auto dest = destinations.Find(target._window);
    if (!dest || !dest->acquired || dest->acquired->serial != target._acquisition) {
        return std::unexpected(DestinationError::ExpiredFrameTarget);
    }
    auto recording = destinations.FindRecording(dest->id);
    if (!recording || !recording->recorder.IsRecording()) {
        return std::unexpected(DestinationError::ExpiredFrameTarget);
    }
    if (target._texture != TextureHandle::Invalid) {
        const auto it = renderTextures.find(target._texture);
        if (it == renderTextures.end() || !it->second.image.Valid()) {
            return std::unexpected(DestinationError::RenderTextureUnavailable);
        }
        auto& texture = it->second;
        return ResolvedTarget {.window = *dest, .recorder = recording->recorder, .image = texture.image, .layout = texture.layout, .drawn = texture.drawn};
    }
    auto& image = *dest->acquired;
    return ResolvedTarget {.window = *dest, .recorder = recording->recorder, .image = image.image, .layout = image.layout, .drawn = image.drawn};
}

auto RenderContext::Impl::FrameCommand() const noexcept -> VkCommandBuffer {
    const auto active = destinations.Active();
    if (!active) {
        return VK_NULL_HANDLE;
    }
    const auto recording = destinations.FindRecording(active->id);
    return recording ? recording->recorder.Handle() : VK_NULL_HANDLE;
}

void RenderContext::Impl::ReleaseTarget(const PresentationTarget& aux) noexcept {
    auto entry = destinations.Find(aux);
    if (!entry || entry->IsPrimary()) {
        return;
    }
    if (ctx.Device() != VK_NULL_HANDLE) {
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && Vk::IsDeviceLost(waited.error())) {
            ctx.Instance().IncrementDeviceLost();
        }
    }
    destinations.AbortRecording(entry->id);
    entry->depthTarget.Destroy(allocator);
    destinations.Detach(aux);
}

void RenderContext::Impl::DestroyDestinations() noexcept {
    if (ctx.Device() != VK_NULL_HANDLE) {
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && Vk::IsDeviceLost(waited.error())) {
            ctx.Instance().IncrementDeviceLost();
        }
    }
    destinations.AbortRecordings();
    for (auto& destination: destinations.Windows()) {
        if (!destination.IsPrimary()) {
            destination.depthTarget.Destroy(allocator);
        }
    }
    destinations.Clear();
    renderTextures.clear();
}

} // namespace ZHLN
