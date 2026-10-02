// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
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

        [[nodiscard]] auto IsPrimary() const noexcept -> bool { return ownedPresenter == nullptr; }
        [[nodiscard]] auto Presenter() const noexcept -> Vk::SwapchainPresenter& {
            return ownedPresenter != nullptr ? *ownedPresenter : *presenter;
        }
    };

    // List nodes are constructed once and never assigned or moved while a
    // window's command buffer is recording. Window metadata remains separately
    // movable so removing a presenter cannot overwrite another recording.
    struct Recording {
        uint64_t windowId;
        Vk::CommandRecorder recorder;

        Recording(uint64_t id, Vk::CommandRecorder&& active) noexcept: windowId(id), recorder(std::move(active)) {}
        Recording(const Recording&) = delete;
        auto operator=(const Recording&) -> Recording& = delete;
        Recording(Recording&&) = delete;
        auto operator=(Recording&&) -> Recording& = delete;
    };

    static constexpr size_t kMaxWindows = 8;

    FrameDestinations() noexcept = default;
    ~FrameDestinations() noexcept;
    FrameDestinations(FrameDestinations&&) = delete;
    auto operator=(FrameDestinations&&) -> FrameDestinations& = delete;
    FrameDestinations(const FrameDestinations&) = delete;
    auto operator=(const FrameDestinations&) -> FrameDestinations& = delete;

    [[nodiscard]] auto Find(const PresentationTarget& target) noexcept -> ZHLN::Optional<Window&>;
    [[nodiscard]] auto Find(const PresentationTarget& target) const noexcept -> ZHLN::Optional<const Window&>;
    [[nodiscard]] auto Find(uint64_t id) noexcept -> ZHLN::Optional<Window&>;
    [[nodiscard]] auto Find(uint64_t id) const noexcept -> ZHLN::Optional<const Window&>;
    [[nodiscard]] auto Windows() noexcept -> std::span<Window>;
    [[nodiscard]] auto FindRecording(uint64_t windowId) noexcept -> ZHLN::Optional<Recording&>;
    [[nodiscard]] auto FindRecording(uint64_t windowId) const noexcept -> ZHLN::Optional<const Recording&>;
    void AddRecording(uint64_t windowId, Vk::CommandRecorder&& recorder) noexcept;
    void AbortRecording(uint64_t windowId) noexcept;
    [[nodiscard]] auto Full() const noexcept -> bool;
    auto Attach(Window entry) noexcept -> ZHLN::Optional<Window&>;
    void Detach(const PresentationTarget& target) noexcept;
    void Clear() noexcept;
    void BeginFrame() noexcept;
    void AbortRecordings() noexcept;

    void SetActive(uint64_t id) noexcept { activeWindow = id; }
    [[nodiscard]] auto Active() const noexcept -> ZHLN::Optional<const Window&> { return Find(activeWindow); }

  private:
    uint64_t activeWindow = 0;
    uint64_t nextWindowId = 1;
    std::vector<Window> windows;
    std::list<Recording> recordings;
};

static_assert(std::is_move_constructible_v<FrameDestinations::Window> && std::is_move_assignable_v<FrameDestinations::Window>);
static_assert(!std::is_default_constructible_v<FrameDestinations::Recording>);
static_assert(!std::is_move_constructible_v<FrameDestinations::Recording> && !std::is_move_assignable_v<FrameDestinations::Recording>);
static_assert(!std::is_move_assignable_v<FrameDestinations>);

}
