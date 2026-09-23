// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/TextureManager.hpp
//
// The renderer's bindless texture table. One object, two layers:
//
//   * the GPU layer -- the globalTextures[] region of the descriptor heap, the
//     image and view behind each slot, and the free list that recycles a slot
//     at the frame boundary after the frames that could still sample it have
//     retired;
//   * the naming layer -- the handle -> record table that gives an uploaded
//     image a stable identity and remembers which slot it landed in.
//
// What this is NOT: an asset loader. It knows no file format, opens no path and
// holds no CPU copy of a texture. Pixels arrive already decoded, exactly as
// meshes arrive as a vertex span and fonts as a BakedFontAsset -- the VFS read,
// the container unpack (CookedTextureHeader) and the stb_image decode all live
// above this layer, in the engine. Everything here converts a pixel buffer into
// a VkImage and a globalTextures[] slot, which is the renderer's whole job.
//
// Every dependency arrives at construction (device context, allocator, staging
// ring, command ring, heap manager). The manager uploads through
// Vk::TextureUploader itself and writes its own descriptors through the heap
// manager it was handed, so it never reaches back into RenderContext: there is
// no cycle to break, RenderContext::Impl owns this and this owns the slots.
//
// The one thing it does not own is the frame clock. Releasing a slot parks the
// image and view until the parity that may still be sampling it has retired, so
// the parity is handed in once per frame by BeginFrame -- the same call that
// reclaims the slots the previous frame of that parity parked.

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

// globalTextures[] is a single offset-addressed region of the resource heap
// (reserved in RenderInitHeaps.cpp). The budget and the three permanent
// fallback slots live here, next to the table they index, rather than in
// RenderInternal.hpp -- the heap only needs the budget to size itself.
static constexpr uint32_t kGlobalTextureSlots = 32768; // bindless globalTextures[] region

// Uploaded first by InitializeSystemTextures, in this order, and used as the
// fallback whenever a texture cannot be created or looked up. An index into
// globalTextures[], not a handle.
static constexpr uint32_t kFallbackBlackTextureIndex  = 0;
static constexpr uint32_t kFallbackWhiteTextureIndex  = 1;
static constexpr uint32_t kFallbackNormalTextureIndex = 2;

// The one pixel-format decision this layer makes: every texture that arrives as
// host pixels is 8-bit RGBA, and the only choice the caller has is whether the
// sampler reads it as sRGB. Spelled once, here, so the rest of the renderer
// speaks VkFormat.
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

    // --- Heap region -------------------------------------------------------

    // Reserves globalTextures[] in the resource heap and remembers where it
    // landed. Runs once, after HeapManager::Init and before any pass's heap
    // mapping is baked, because a mapping names the base slot.
    [[nodiscard]] auto     ReserveBindlessRegion() -> std::expected<void, ErrorCode>;
    [[nodiscard]] uint32_t BindlessBaseSlot() const noexcept {
        return _bindlessBaseSlot;
    }

    // --- Named uploads -----------------------------------------------------

    // Decoded 8-bit RGBA host pixels -> a named, bindless-backed texture.
    // `pixels` is read width*height*4 bytes and is not retained: the caller
    // owns the source and is what regenerates it after a device loss, the same
    // way the engine re-bakes the font atlas rather than the renderer keeping
    // the coverage bytes.
    //
    // Deduplicated by identifier and content: re-uploading the same name with
    // byte-identical pixels answers with the handle and slot already in the
    // table instead of burning another index and orphaning the old image. A
    // genuine content change replaces the record and hands the old slot back.
    [[nodiscard]] auto Upload(std::string_view identifier, const void* pixels, uint32_t width, uint32_t height, VkFormat format)
        -> std::expected<TextureHandle, ErrorCode>;
    // Records a slot the caller uploaded itself. No GPU work, no ownership
    // transfer of anything but the record.
    [[nodiscard]] auto RegisterUploaded(std::string_view identifier, uint32_t bindlessIndex, VkFormat format) -> TextureHandle;
    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;
    // Drops the record and parks its slot for reclamation at the next frame
    // boundary of this parity. Later GetBindlessIndex calls resolve to the
    // white fallback immediately.
    void Unload(TextureHandle handle);
    // Teardown: parks every recorded slot and drops the records. Self-contained
    // -- the caller does not collect the indices and release them, because the
    // table that knows which slots are in use is the one that gives them back.
    // Includes the fallback index recorded for failed uploads, which
    // ReleaseSlot ignores.
    void Clear();
    // The device is gone, so every image, view and slot went with it. Abandons
    // the GPU layer and points every surviving record at the white fallback, so
    // a lookup between the loss and the re-upload resolves to white rather than
    // to a slot that no longer exists. The records themselves stay: they are
    // what the engine re-uploads against.
    void OnDeviceLost();

    // --- Unnamed GPU layer -------------------------------------------------
    // What the renderer's own resources use: system fallbacks, render targets
    // published for sampling, baked compute output. No identity, just a slot.

    [[nodiscard]] auto Upload2D(const void* data, uint32_t width, uint32_t height, VkFormat format) -> std::expected<uint32_t, ErrorCode>;
    // Six square faces in +X, -X, +Y, -Y, +Z, -Z order, 8-bit RGBA.
    [[nodiscard]] auto UploadCube(const void* const* faceData, uint32_t size) -> std::expected<uint32_t, ErrorCode>;
    // Takes ownership of an already-uploaded image and publishes it in
    // globalTextures[]. Reuses a slot released by ReleaseSlot before advancing
    // the high-water mark, and fails with
    // DescriptorHeapError::ResourceSlotsExhausted rather than writing past the
    // region when every slot is occupied.
    [[nodiscard]] auto Adopt(Vk::Image&& image, Vk::ImageView&& view, VkFormat format, uint32_t mipLevels = 1, bool cube = false)
        -> std::expected<uint32_t, ErrorCode>;
    // Frame boundary. Records the parity every release this frame parks into,
    // then reclaims the slots the previous frame of that parity released:
    // points each at the white fallback and returns its index to the pool.
    // Called from BeginFrame after the fence wait, so no submission can be
    // reading those descriptors.
    void BeginFrame(uint32_t frameIndex) noexcept;
    // Hands a slot back. It keeps its descriptor -- in-flight frames may still
    // sample it -- until BeginFrame neutralizes it at the frame boundary of the
    // parity that released it. Releasing an unoccupied slot or one of the three
    // fallbacks is a no-op.
    void ReleaseSlot(uint32_t bindlessIndex) noexcept;

    [[nodiscard]] auto                 SlotCount() const noexcept -> size_t { return _slotImages.size(); }
    [[nodiscard]] const Vk::Image&     Image(uint32_t slot) const noexcept { return _slotImages[slot]; }
    [[nodiscard]] const Vk::ImageView& View(uint32_t slot) const noexcept { return _slotViews[slot]; }
    // Debug-utils labels for every occupied slot. Slots released and awaiting
    // reclamation hold no image, so naming them is skipped.
    void NameSlots() noexcept;

  private:
    struct TextureRecord {
        TextureHandle handle = TextureHandle::Invalid;
        String256     identifier;
        VkFormat      format = VK_FORMAT_UNDEFINED;
        uint32_t      width  = 0;
        uint32_t      height = 0;
        // Hash of the pixels the slot was uploaded from, kept instead of the
        // pixels: it is what makes a byte-identical re-upload answer from the
        // table, and it costs 8 bytes where a CPU copy would cost the whole
        // image. Nothing here can reconstruct a texture from it, by design.
        uint64_t pixelHash       = 0;
        uint32_t gpuBindlessIndex = kFallbackWhiteTextureIndex;
    };

    // A slot handed back but not yet reclaimed. The descriptor keeps pointing
    // at these until BeginFrame runs, so ownership lives here rather than dying
    // in ReleaseSlot.
    struct ReleasedSlot {
        uint32_t      index = 0;
        Vk::Image     image;
        Vk::ImageView view;
    };

    // globalTextures[] is addressed by raw offset (base + index), not through
    // the slot allocator, because a bindless array is one contiguous region.
    void WriteSlotToHeap(uint32_t bindlessIndex, VkImage image, VkFormat format, uint32_t mipLevels, bool cube) noexcept;

    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::StagingRingBuffer&                       _staging;
    Vk::CommandRing<Vk::QueueType::Graphics, 8>& _cmdRing;
    Vk::HeapManager&                             _heaps;

    // First slot of the globalTextures[] region; 0 until ReserveBindlessRegion.
    uint32_t _bindlessBaseSlot = 0;
    // The parity BeginFrame last recorded, which is the parity a release parks
    // into. 0 before the first frame, matching the presenter's initial index.
    uint32_t _frameIndex = 0;

    // Slot-indexed, not append-only: a recycled index is not the highest one.
    ZHLN::Array<Vk::Image>     _slotImages;
    ZHLN::Array<Vk::ImageView> _slotViews;
    // A high-water mark, not a live count: a released slot is recycled only
    // once the frames that could still read its descriptor have retired.
    uint32_t                                 _nextSlotIndex = 0;
    ZHLN::Array<uint32_t>                    _freeSlots;
    std::array<ZHLN::Array<ReleasedSlot>, 2> _pendingFrees;

    HashMap<uint64_t, TextureRecord> _textures;
    mutable Mutex                    _mutex {};
};

} // namespace ZHLN
