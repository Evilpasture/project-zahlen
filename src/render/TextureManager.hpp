// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "Rendering.hpp"

#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace ZHLN {

static constexpr uint32_t kGlobalTextureSlots = 32768;

static constexpr uint32_t kFallbackBlackTextureIndex  = 0;
static constexpr uint32_t kFallbackWhiteTextureIndex  = 1;
static constexpr uint32_t kFallbackNormalTextureIndex = 2;

[[nodiscard]] constexpr VkFormat Rgba8Format(bool isSRGB) noexcept {
    return isSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

class TextureManager {
  public:
    TextureManager(
        Vk::Context&                                 ctx,
        Vk::Allocator&                               allocator,
        Vk::StagingRingBuffer&                       staging,
        Vk::CommandRing<Vk::QueueType::Graphics, 8>& cmdRing,
        Vk::HeapManager&                             heaps
    ) noexcept;
    ~TextureManager();
    TextureManager(const TextureManager&)                    = delete;
    auto operator=(const TextureManager&) -> TextureManager& = delete;
    TextureManager(TextureManager&&)                         = delete;
    auto operator=(TextureManager&&) -> TextureManager&      = delete;


    [[nodiscard]] auto     ReserveBindlessRegion() -> std::expected<void, ErrorCode>;
    [[nodiscard]] uint32_t BindlessBaseSlot() const noexcept {
        return _bindlessBaseSlot;
    }


    [[nodiscard]] auto Upload(std::string_view identifier, const void* pixels, uint32_t width, uint32_t height, VkFormat format)
        -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] auto UploadUnnamed(const void* pixels, uint32_t width, uint32_t height, VkFormat format)
        -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] auto UploadCube(const void* const* faceData, uint32_t size) -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] auto AdoptTexture(Vk::Image image, Vk::ImageView view, uint32_t width, uint32_t height)
        -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;
    void Unload(TextureHandle handle);
    void Clear();
    // Only after an idle wait (e.g. ClearGPUCaches): reclaim every retired slot.
    void RetireAll() noexcept;
    void OnDeviceLost();


    // Raw slots are only for internal fallback, render-target and LUT setup.
    [[nodiscard]] auto Upload2D(const void* data, uint32_t width, uint32_t height, VkFormat format) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] auto Adopt(Vk::Image image, Vk::ImageView view) -> std::expected<uint32_t, ErrorCode>;
    void BeginFrame(uint32_t frameIndex) noexcept;
    void ReleaseSlot(uint32_t bindlessIndex) noexcept;

    [[nodiscard]] auto                 SlotCount() const noexcept -> size_t { return _slotImages.size(); }
    [[nodiscard]] const Vk::Image&     Image(uint32_t slot) const noexcept { return _slotImages[slot]; }
    [[nodiscard]] const Vk::ImageView& View(uint32_t slot) const noexcept { return _slotViews[slot]; }
    // A slice borrows the exact view metadata, including cube/array/3D shape
    // and mip/layer range. Appending slots cannot relocate a deque element;
    // slot release/replacement, device loss, or manager destruction ends the borrow.
    [[nodiscard]] auto Slice(uint32_t slot, VkExtent2D extent) const noexcept -> Vk::ImageSlice {
        const Vk::ImageView& view = _slotViews[slot];
        return Vk::ImageSlice {_slotImages[slot].Handle(), view, extent, view.Info().format};
    }
    void NameSlots() noexcept;

  private:
    struct TextureRecord {
        TextureHandle handle           = TextureHandle::Invalid;
        std::string   identifier {};
        bool          named            = false;
        VkFormat      format           = VK_FORMAT_UNDEFINED;
        uint32_t      width            = 0;
        uint32_t      height           = 0;
        uint64_t      pixelHash        = 0;
        uint32_t      gpuBindlessIndex = kFallbackWhiteTextureIndex;
    };

    struct ReleasedSlot {
        uint32_t      index = 0;
        Vk::Image     image;
        Vk::ImageView view;
    };

    // Assign an opaque, never-reused handle to a newly adopted GPU slot.
    [[nodiscard]] TextureHandle RegisterAnonymous(uint32_t bindlessIndex, VkFormat format, uint32_t width, uint32_t height);
    void WriteSlotToHeap(uint32_t bindlessIndex, const Vk::ImageView& view) noexcept;
    void RetireBatch(ZHLN::Array<ReleasedSlot>& pending) noexcept;
    void DestroyAllSlots() noexcept;

    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::StagingRingBuffer&                       _staging;
    Vk::CommandRing<Vk::QueueType::Graphics, 8>& _cmdRing;
    Vk::HeapManager&                             _heaps;

    uint32_t _bindlessBaseSlot = 0;
    uint32_t _frameIndex = 0;

    ZHLN::Array<Vk::Image> _slotImages;
    std::deque<Vk::ImageView> _slotViews;
    uint32_t _nextSlotIndex = 0;
    ZHLN::Array<uint32_t> _freeSlots;
    std::array<ZHLN::Array<ReleasedSlot>, Vk::kFramesInFlight> _pendingFrees;

    HashMap<uint64_t, TextureRecord> _textures;
    // Kept across Clear() so unnamed uploads do not recycle old IDs.
    uint64_t                         _nextAnonymousHandle = 1ull << 63;
    mutable Mutex                    _mutex {};
};

}
