// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "PresentationSurface.hpp"
#include "RenderInternal.hpp"
#include <Zahlen/Log.hpp>
#include <cstdint>
#include <memory>
#include <utility>

namespace ZHLN {



DestinationRegistry::DestinationRecording::~DestinationRecording() noexcept {
    Close();
}

DestinationRegistry::DestinationRecording::DestinationRecording(DestinationRecording&& other) noexcept: cmd(other.cmd), open(other.open) {
    other.cmd  = VK_NULL_HANDLE;
    other.open = false;
}

auto DestinationRegistry::DestinationRecording::operator=(DestinationRecording&& other) noexcept -> DestinationRecording& {
    if (this != &other) {
        Close();
        cmd       = other.cmd;
        open      = other.open;
        other.cmd  = VK_NULL_HANDLE;
        other.open = false;
    }
    return *this;
}

auto DestinationRegistry::DestinationRecording::Open(VkCommandBuffer slot) noexcept -> VkCommandBuffer {
    if (!open) {
        cmd  = slot;
        open = cmd != VK_NULL_HANDLE;
        if (open) {
            ZHLN_BeginCommandBuffer(cmd);
        }
    }
    return cmd;
}

void DestinationRegistry::DestinationRecording::Close() noexcept {
    if (open) {
        if (cmd != VK_NULL_HANDLE) {
            ZHLN_EndCommandBuffer(cmd);
        }
        open = false;
    }
}

void DestinationRegistry::DestinationRecording::Discard() noexcept {
    cmd  = VK_NULL_HANDLE;
    open = false;
}


auto RenderContext::Impl::FindOrCreateDestination(PresentationTarget& aux, bool primary) noexcept -> std::expected<DestinationVend, ErrorCode> {
    if (auto* existing = destinations.Find(aux); existing != nullptr) {
        return DestinationVend {.entry = existing, .created = false};
    }
    if (destinations.Full()) {
        return std::unexpected(DestinationError::RegistryFull);
    }

    DestinationRegistry::WindowEntry dest {};
    dest.target = &aux;

    if (!primary) {
        if (presentationMode != PresentationMode::NativeSwapchain) {
            return std::unexpected(DestinationError::NativeSwapchainRequired);
        }
        if (ctx.Device() == VK_NULL_HANDLE) {
            return std::unexpected(DestinationError::DeviceUnavailable);
        }

        const Extent2D extent = aux.GetFramebufferExtent();
        auto           surfaceRes = CreateSurfaceFromNative(ctx.Instance(), aux.GetNativeSurface());
        if (!surfaceRes) {
            return std::unexpected(ErrorCode {surfaceRes.error()});
        }

        auto owned     = std::make_unique<Vk::SwapchainPresenter>();
        owned->surface = Vk::Surface(ctx.Instance(), surfaceRes->Release());
        if (owned->surface.Get() == VK_NULL_HANDLE || extent.width == 0 || extent.height == 0) {
            return std::unexpected(DestinationError::SurfaceUnusable);
        }
        if (auto initRes = owned->Init(ctx, allocator, extent.width, extent.height, ctx.PhysicalInfo().graphics_family, true);
            !initRes) {
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

    if (auto* entry = destinations.Attach(std::move(dest)); entry != nullptr) {
        return DestinationVend {.entry = entry, .created = true};
    }
    return std::unexpected(DestinationError::RegistryFull);
}


auto RenderContext::Impl::AcquireDestinationImage(DestinationRegistry::WindowEntry& dest) noexcept
    -> std::expected<std::optional<DestinationRegistry::Handle>, ErrorCode> {
    if (dest.imageAcquired) {
        if (dest.imageIndex >= dest.recordHandles.size() || !dest.recordHandles[dest.imageIndex].Valid()) {
            return std::nullopt;
        }
        return dest.recordHandles[dest.imageIndex];
    }
    if (dest.target == nullptr) {
        return std::nullopt;
    }

    Vk::SwapchainPresenter& destPresenter = dest.Presenter();

    const Extent2D size     = dest.target->GetFramebufferExtent();
    auto           acquired = destPresenter.AcquireNext(VkExtent2D {.width = size.width, .height = size.height}, !dest.IsPrimary());
    if (!acquired) {
        const ErrorCode error = acquired.error();
        if (!error.Is(FrameResult::DeviceLost)) {
            return std::unexpected(error);
        }
        Vk::Instance::IncrementNumericalDeviceLoss();
        destinations.Retire(dest.target);
        dest.recording.Discard();
        dest.recordHandles.clear();
        dest.cachedGeneration = destPresenter.resourceGeneration;
        return std::unexpected(error);
    }
    if (!acquired->has_value()) {
        destinations.Retire(dest.target);
        dest.recording.Discard();
        dest.recordHandles.clear();
        dest.cachedGeneration = destPresenter.resourceGeneration;
        return std::nullopt;
    }

    const Vk::SwapchainTarget& target = **acquired;

    if (dest.cachedGeneration != target.generation) {
        if (dest.cachedGeneration != 0) {
            ZHLN::Log(
                "[Render] Destination resources rebuilt (generation {} -> {}); re-vending the window's image.", dest.cachedGeneration,
                target.generation
            );
            destinations.Retire(dest.target);
            dest.recording.Discard();
            dest.recordHandles.clear();
        }
        dest.cachedGeneration = target.generation;
    }

    dest.imageIndex = target.imageIndex;
    if (dest.recordHandles.size() <= target.imageIndex) {
        dest.recordHandles.resize(target.imageIndex + 1);
    }
    if (!dest.recordHandles[target.imageIndex].Valid()) {
        dest.recordHandles[target.imageIndex] = destinations.Register(DestinationRegistry::Record {
            .bindlessIndex = 0,
            .image         = target.image,
            .presentable   = target.presentable,
            .generation    = target.generation,
            .target        = dest.target,
        });
    }

    const DestinationRegistry::Handle handle = dest.recordHandles[target.imageIndex];
    DestinationRegistry::Record&      record = destinations.Records()[handle.Index()];
    record.trackedLayout = Vk::AttachmentLayout::Undefined;
    record.content.reset();

    dest.imageAcquired = true;
    return handle;
}


namespace {

[[nodiscard]] constexpr auto AttachmentFor(DestinationRegistry::Handle handle) noexcept -> RenderAttachment {
    return RenderAttachment {.texture = handle.AsTexture(), .mipLevel = 0, .arrayLayer = 0};
}

[[nodiscard]] auto VendedAttachmentOf(const DestinationRegistry::WindowEntry& dest) noexcept -> std::optional<RenderAttachment> {
    if (!dest.imageAcquired || dest.imageIndex >= dest.recordHandles.size()) {
        return std::nullopt;
    }
    const DestinationRegistry::Handle handle = dest.recordHandles[dest.imageIndex];
    if (!handle.Valid()) {
        return std::nullopt;
    }
    return AttachmentFor(handle);
}

}

auto RenderContext::Impl::TargetAttachment(const PresentationTarget& aux) noexcept -> std::optional<RenderAttachment> {
    const DestinationRegistry::WindowEntry* dest = destinations.Find(aux);
    if (dest == nullptr) {
        return std::nullopt;
    }
    return VendedAttachmentOf(*dest);
}

auto RenderContext::Impl::AcquireTarget(const PresentationTarget& aux) noexcept -> FrameOutcome<RenderAttachment> {
    if (!activeQueueGuard.has_value()) {
        return std::unexpected(DestinationError::NoActiveFrame);
    }

    auto found = FindOrCreateDestination(const_cast<PresentationTarget&>(aux), &aux == &presentationTarget);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (found->created) {
        const DestinationRegistry::WindowEntry* entry = found->entry;
        ZHLN::Log(
            "[Render] Destination created for window {:p} (primary={}); {}", static_cast<const void*>(entry->target), entry->IsPrimary() ? 1 : 0,
            entry->IsPrimary() ? "borrowing the renderer's presenter" : "owning its own presenter"
        );
    }
    DestinationRegistry::WindowEntry* dest = found->entry;
    destinations.SetActive(dest->target);

    const auto acquired = AcquireDestinationImage(*dest);
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    if (!acquired->has_value()) {
        return std::nullopt;
    }

    Vk::SwapchainPresenter& destPresenter = dest->Presenter();
    dest->recording.Open(destPresenter.SlotCommand(destPresenter.frameIndex));

    return AttachmentFor(**acquired);
}

auto RenderContext::Impl::RecordingFor(const DestinationRegistry::Record& record) const noexcept -> VkCommandBuffer {
    const DestinationRegistry::WindowEntry* dest = destinations.DestinationOf(record);
    return dest != nullptr ? dest->recording.Command() : VK_NULL_HANDLE;
}

auto RenderContext::Impl::FrameCommand() const noexcept -> VkCommandBuffer {
    const DestinationRegistry::WindowEntry* active = destinations.ActiveDestination();
    return active != nullptr ? active->recording.Command() : VK_NULL_HANDLE;
}


void RenderContext::Impl::ReleaseTarget(const PresentationTarget& aux) noexcept {
    DestinationRegistry::WindowEntry* entry = destinations.Find(aux);
    if (entry == nullptr || entry->IsPrimary()) {
        return;
    }

    const PresentationTarget* released = entry->target;
    if (ctx.Device() != VK_NULL_HANDLE) {
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && waited.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::IncrementNumericalDeviceLoss();
        }
    }
    destinations.Detach(aux);
    destinations.Retire(released);
}

void RenderContext::Impl::DestroyDestinations() noexcept {
    if (ctx.Device() != VK_NULL_HANDLE) {
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && waited.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::IncrementNumericalDeviceLoss();
        }
    }
    destinations.Clear();
}

}
