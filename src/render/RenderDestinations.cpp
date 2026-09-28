// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresentationSurface.hpp"
#include "RenderInternal.hpp"
#include <Zahlen/Log.hpp>
#include <memory>
#include <utility>

namespace ZHLN {

auto RenderContext::Impl::FindOrCreateDestination(const PresentationTarget& aux, bool primary) noexcept -> std::expected<DestinationVend, ErrorCode> {
    if (auto* existing = destinations.Find(aux); existing != nullptr) {
        return DestinationVend {.entry = existing, .created = false};
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

        const Extent2D extent = aux.GetFramebufferExtent();
        auto surfaceRes = CreateSurfaceFromNative(ctx.Instance(), aux.GetNativeSurface());
        if (!surfaceRes) {
            return std::unexpected(ErrorCode {surfaceRes.error()});
        }

        auto owned = std::make_unique<Vk::SwapchainPresenter>();
        owned->surface = Vk::Surface(ctx.Instance(), surfaceRes->Release());
        if (owned->surface.Get() == VK_NULL_HANDLE || extent.width == 0 || extent.height == 0) {
            return std::unexpected(DestinationError::SurfaceUnusable);
        }
        if (auto initRes = owned->Init(ctx, allocator, extent.width, extent.height, ctx.PhysicalInfo().graphics_family, true); !initRes) {
            return std::unexpected(initRes.error());
        }
        if (owned->GetPresentFormat() != presenter.GetPresentFormat()) {
            return std::unexpected(DestinationError::PresentFormatMismatch);
        }
        dest.presenter = owned.get();
        dest.ownedPresenter = std::move(owned);
    } else {
        dest.presenter = &presenter;
    }

    if (auto* entry = destinations.Attach(std::move(dest)); entry != nullptr) {
        return DestinationVend {.entry = entry, .created = true};
    }
    return std::unexpected(DestinationError::TooManyWindows);
}

auto RenderContext::Impl::AcquireDestinationImage(FrameDestinations::Window& dest) noexcept -> std::expected<bool, ErrorCode> {
    if (dest.acquired.has_value()) {
        return true;
    }
    if (dest.target == nullptr) {
        return false;
    }

    Vk::SwapchainPresenter& destPresenter = dest.Presenter();
    const Extent2D size = dest.target->GetFramebufferExtent();
    auto acquired = destPresenter.AcquireNext(VkExtent2D {.width = size.width, .height = size.height}, !dest.IsPrimary());
    if (!acquired) {
        if (acquired.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::IncrementNumericalDeviceLoss();
        }
        dest.recording.Discard();
        dest.cachedGeneration = destPresenter.resourceGeneration;
        return std::unexpected(acquired.error());
    }
    if (!acquired->has_value()) {
        dest.recording.Discard();
        dest.cachedGeneration = destPresenter.resourceGeneration;
        return false;
    }

    const Vk::SwapchainTarget& image = **acquired;
    if (dest.cachedGeneration != 0 && dest.cachedGeneration != image.generation) {
        ZHLN::Log("[Render] Window presentation resources rebuilt (generation {} -> {}).", dest.cachedGeneration, image.generation);
    }
    dest.cachedGeneration = image.generation;
    dest.acquired.emplace(FrameDestinations::Acquired {
        .image = image.image,
        .imageIndex = image.imageIndex,
        .serial = nextAcquisition++,
    });
    return true;
}

auto RenderContext::Impl::TargetAttachment(const PresentationTarget& aux) const noexcept -> std::optional<FrameTarget> {
    if (!activeQueueGuard.has_value()) {
        return std::nullopt;
    }
    const auto* dest = destinations.Find(aux);
    if (dest == nullptr || !dest->acquired || !dest->recording.IsOpen()) {
        return std::nullopt;
    }
    return FrameTarget {rendererId, frameSerial, dest->id, dest->acquired->serial};
}

auto RenderContext::Impl::AcquireTarget(const PresentationTarget& aux) noexcept -> FrameOutcome<FrameTarget> {
    if (!activeQueueGuard.has_value()) {
        return std::unexpected(DestinationError::NoActiveFrame);
    }

    auto found = FindOrCreateDestination(aux, &aux == &presentationTarget);
    if (!found) {
        return std::unexpected(found.error());
    }
    FrameDestinations::Window& dest = *found->entry;
    if (found->created) {
        ZHLN::Log(
            "[Render] Window presenter created for {:p} (primary={}).", static_cast<const void*>(dest.target), dest.IsPrimary() ? 1 : 0
        );
    }
    const auto acquired = AcquireDestinationImage(dest);
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    if (!*acquired) {
        return std::nullopt;
    }

    Vk::SwapchainPresenter& destPresenter = dest.Presenter();
    dest.recording.Open(destPresenter.SlotCommand(destPresenter.frameIndex));
    destinations.SetActive(dest.id);
    return FrameTarget {rendererId, frameSerial, dest.id, dest.acquired->serial};
}

auto RenderContext::Impl::ResolveTarget(const FrameTarget& target) noexcept -> std::expected<ResolvedTarget, ErrorCode> {
    if (!activeQueueGuard.has_value()) {
        return std::unexpected(DestinationError::NoActiveFrame);
    }
    if (target._renderer != rendererId || target._frame != frameSerial || !target.Valid()) {
        return std::unexpected(DestinationError::ExpiredFrameTarget);
    }
    auto* dest = destinations.Find(target._window);
    if (dest == nullptr || !dest->acquired || dest->acquired->serial != target._acquisition || !dest->recording.IsOpen()) {
        return std::unexpected(DestinationError::ExpiredFrameTarget);
    }
    if (target._texture != RenderTextureHandle::Invalid) {
        const auto it = renderTextures.find(target._texture);
        if (it == renderTextures.end() || !it->second.image.Valid()) {
            return std::unexpected(DestinationError::RenderTextureUnavailable);
        }
        auto& texture = it->second;
        return ResolvedTarget {.window = *dest, .image = texture.image, .layout = texture.layout, .drawn = texture.drawn};
    }
    auto& image = *dest->acquired;
    return ResolvedTarget {.window = *dest, .image = image.image, .layout = image.layout, .drawn = image.drawn};
}

auto RenderContext::Impl::FrameCommand() const noexcept -> VkCommandBuffer {
    const auto* active = destinations.Active();
    return active != nullptr ? active->recording.Command() : VK_NULL_HANDLE;
}

void RenderContext::Impl::ReleaseTarget(const PresentationTarget& aux) noexcept {
    auto* entry = destinations.Find(aux);
    if (entry == nullptr || entry->IsPrimary()) {
        return;
    }
    if (ctx.Device() != VK_NULL_HANDLE) {
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && waited.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::IncrementNumericalDeviceLoss();
        }
    }
    destinations.Detach(aux);
}

void RenderContext::Impl::DestroyDestinations() noexcept {
    if (ctx.Device() != VK_NULL_HANDLE) {
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && waited.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::IncrementNumericalDeviceLoss();
        }
    }
    destinations.Clear();
    renderTextures.clear();
}

}
