// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ZHLN {

class PresentationTarget;

enum class DestinationError : uint8_t {
    TooManyWindows ZHLN_ANNOTATION(ZHLN::Description<"No room for another window presenter">{}) = 1,
    NativeSwapchainRequired ZHLN_ANNOTATION(ZHLN::Description<"A second window needs native swapchain presentation">{}),
    DeviceUnavailable ZHLN_ANNOTATION(ZHLN::Description<"There is no device to build a window presenter on">{}),
    SurfaceUnusable ZHLN_ANNOTATION(ZHLN::Description<"The window's surface is null, or its extent is empty">{}),
    PresentFormatMismatch ZHLN_ANNOTATION(ZHLN::Description<"The window's present format does not match the primary swapchain">{}),
    NoActiveFrame ZHLN_ANNOTATION(ZHLN::Description<"An image was requested outside BeginFrame/EndFrame">{}),
    ExpiredFrameTarget ZHLN_ANNOTATION(ZHLN::Description<"The frame target belongs to another renderer, frame or acquisition">{}),
    RenderTextureUnavailable ZHLN_ANNOTATION(ZHLN::Description<"The render texture was destroyed or belongs to another renderer">{}),
    UnsupportedColorFormat ZHLN_ANNOTATION(ZHLN::Description<"No UI pipeline was built for this target's color format">{}),
};

// Window presenters persist; their acquired images, layout and written state do
// not. A frame target names one acquisition, not a slot in a reusable table of
// presentation images. Offscreen textures have their own persistent ownership.
class FrameDestinations {
  public:
    struct Acquired {
        Vk::ImageSlice image {};
        uint32_t       imageIndex = 0;
        uint64_t       serial = 0;
        Vk::AttachmentLayout layout = Vk::AttachmentLayout::Undefined;
        bool drawn = false;
    };

    struct Window {
        const PresentationTarget* target = nullptr;
        uint64_t id = 0;
        Vk::SwapchainPresenter* presenter = nullptr;
        std::unique_ptr<Vk::SwapchainPresenter> ownedPresenter;
        std::optional<Acquired> acquired;
        uint64_t cachedGeneration = 0;
        Vk::CommandRecorder recorder;

        [[nodiscard]] auto IsPrimary() const noexcept -> bool { return ownedPresenter == nullptr; }
        [[nodiscard]] auto Presenter() const noexcept -> Vk::SwapchainPresenter& {
            return ownedPresenter != nullptr ? *ownedPresenter : *presenter;
        }
    };

    static constexpr size_t kMaxWindows = 8;

    FrameDestinations() noexcept = default;
    ~FrameDestinations() noexcept;
    FrameDestinations(FrameDestinations&&) noexcept;
    auto operator=(FrameDestinations&&) noexcept -> FrameDestinations&;
    FrameDestinations(const FrameDestinations&)                    = delete;
    auto operator=(const FrameDestinations&) -> FrameDestinations& = delete;

    [[nodiscard]] auto Find(const PresentationTarget& target) noexcept -> Window*;
    [[nodiscard]] auto Find(const PresentationTarget& target) const noexcept -> const Window*;
    [[nodiscard]] auto Find(uint64_t id) noexcept -> Window*;
    [[nodiscard]] auto Find(uint64_t id) const noexcept -> const Window*;
    [[nodiscard]] auto Windows() noexcept -> std::span<Window>;
    [[nodiscard]] auto Full() const noexcept -> bool;
    auto Attach(Window entry) noexcept -> Window*;
    void Detach(const PresentationTarget& target) noexcept;
    void Clear() noexcept;
    void BeginFrame() noexcept;
    void AbortRecordings() noexcept;

    void SetActive(uint64_t id) noexcept { activeWindow = id; }
    [[nodiscard]] auto Active() const noexcept -> const Window* { return Find(activeWindow); }

  private:
    uint64_t activeWindow = 0;
    uint64_t nextWindowId = 1;
    std::vector<Window> windows;
};

}
