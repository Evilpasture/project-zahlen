// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "Rendering.hpp"

#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
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
    ~TextureManager()                                        = default;
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
    [[nodiscard]] auto RegisterUploaded(std::string_view identifier, uint32_t bindlessIndex, VkFormat format) -> TextureHandle;
    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;
    void Unload(TextureHandle handle);
    void Clear();
    void OnDeviceLost();


    [[nodiscard]] auto Upload2D(const void* data, uint32_t width, uint32_t height, VkFormat format) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] auto UploadCube(const void* const* faceData, uint32_t size) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] auto Adopt(Vk::Image&& image, Vk::ImageView&& view) -> std::expected<uint32_t, ErrorCode>;
    void BeginFrame(uint32_t frameIndex) noexcept;
    void ReleaseSlot(uint32_t bindlessIndex) noexcept;

    [[nodiscard]] auto                 SlotCount() const noexcept -> size_t { return _slotImages.size(); }
    [[nodiscard]] const Vk::Image&     Image(uint32_t slot) const noexcept { return _slotImages[slot]; }
    [[nodiscard]] const Vk::ImageView& View(uint32_t slot) const noexcept { return _slotViews[slot]; }
    void NameSlots() noexcept;

  private:
    struct TextureRecord {
        TextureHandle handle = TextureHandle::Invalid;
        String256     identifier;
        VkFormat      format = VK_FORMAT_UNDEFINED;
        uint32_t      width  = 0;
        uint32_t      height = 0;
        uint64_t pixelHash       = 0;
        uint32_t gpuBindlessIndex = kFallbackWhiteTextureIndex;
    };

    struct ReleasedSlot {
        uint32_t      index = 0;
        Vk::Image     image;
        Vk::ImageView view;
    };

    void WriteSlotToHeap(uint32_t bindlessIndex, const Vk::ImageView& view) noexcept;

    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::StagingRingBuffer&                       _staging;
    Vk::CommandRing<Vk::QueueType::Graphics, 8>& _cmdRing;
    Vk::HeapManager&                             _heaps;

    uint32_t _bindlessBaseSlot = 0;
    uint32_t _frameIndex = 0;

    ZHLN::Array<Vk::Image>     _slotImages;
    ZHLN::Array<Vk::ImageView> _slotViews;
    uint32_t                                 _nextSlotIndex = 0;
    ZHLN::Array<uint32_t>                    _freeSlots;
    std::array<ZHLN::Array<ReleasedSlot>, Vk::kFramesInFlight> _pendingFrees;

    HashMap<uint64_t, TextureRecord> _textures;
    mutable Mutex                    _mutex {};
};

}
