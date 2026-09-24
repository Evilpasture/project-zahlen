// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/TextureManager.cpp
//
// No stb_image, no AssetManager, no path parsing: pixels reach this file
// already decoded. See the header comment for where the decode lives.

#include "TextureManager.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstddef>
#include <cstring>
#include <format>

namespace ZHLN {

// Private texture errors (Tier 1): produced only inside this translation unit,
// so no header exposes them and callers just log the type-erased ErrorCode.
// Declared at file scope (not in an anonymous namespace) to keep reflected
// category names stable for both native reflection and the AST transpiler.
enum class TextureUploadError : uint8_t {
    NoPixelData ZHLN_ANNOTATION(ZHLN::Description<"Texture upload was handed no pixel data"> {}) = 1,
};

} // namespace ZHLN

namespace ZHLN {

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

TextureManager::TextureManager(
    Vk::Context&                                 ctx,
    Vk::Allocator&                               allocator,
    Vk::StagingRingBuffer&                       staging,
    Vk::CommandRing<Vk::QueueType::Graphics, 8>& cmdRing,
    Vk::HeapManager&                             heaps
) noexcept
    : _ctx(ctx), _allocator(allocator), _staging(staging), _cmdRing(cmdRing), _heaps(heaps) {}

auto TextureManager::ReserveBindlessRegion() -> std::expected<void, ErrorCode> {
    const auto base = _heaps.ReserveOffsetAddressedResourceRegion(kGlobalTextureSlots);
    if (!base) [[unlikely]] {
        return std::unexpected(base.error());
    }
    _bindlessBaseSlot = *base;
    return {};
}

auto TextureManager::Upload2D(const void* data, uint32_t width, uint32_t height, VkFormat format) -> std::expected<uint32_t, ErrorCode> {
    return Vk::TextureUploader(_ctx, _allocator, _staging, _cmdRing)
        .Upload2D({.data = data, .width = width, .height = height, .format = format, .generateMips = true})
        .and_then([&](Vk::TextureResource tex) -> std::expected<uint32_t, ErrorCode> {
            const auto index = Adopt(std::move(tex.image), std::move(tex.view), format, tex.mipLevels, false);
            if (index) {
                // Indexed, not back(): a recycled slot is not the highest one.
                Vk::Debug::SetImageName(_ctx, _slotImages[*index].Handle(), std::format("BindlessTexture{:03}", *index));
            }
            return index;
        });
}

auto TextureManager::UploadCube(const void* const* faceData, uint32_t size) -> std::expected<uint32_t, ErrorCode> {
    const std::span<const void* const, 6> faces {faceData, 6};

    return Vk::TextureUploader(_ctx, _allocator, _staging, _cmdRing)
        .UploadCube({.faceData = faces, .size = size, .format = VK_FORMAT_R8G8B8A8_UNORM})
        .and_then([&](Vk::TextureResource tex) -> std::expected<uint32_t, ErrorCode> {
            const auto index = Adopt(std::move(tex.image), std::move(tex.view), VK_FORMAT_R8G8B8A8_UNORM, 1, true);
            if (index) {
                Vk::Debug::SetImageName(_ctx, _slotImages[*index].Handle(), std::format("BindlessCubeTexture{:03}", *index));
            }
            return index;
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

auto TextureManager::Upload(std::string_view identifier, const void* pixels, uint32_t width, uint32_t height, VkFormat format)
    -> std::expected<TextureHandle, ErrorCode> {
    const uint64_t id     = HashAssetID(identifier);
    const auto     handle = static_cast<TextureHandle>(id);
    // 8-bit RGBA: four bytes per texel, the only layout the host upload path takes.
    const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);

    // The uploader reads width*height*4 bytes out of `pixels` with no way to
    // signal a short read, so a null source or an empty extent is a caller bug
    // rather than a runtime condition to recover from.
    ZHLN::Assert(pixels != nullptr && pixelBytes > 0, "TextureManager::Upload('{}'): no pixel data for a {}x{} texture", identifier, width, height);
    if (pixels == nullptr || pixelBytes == 0) {
        ZHLN::Log("[TextureManager] Refusing upload '{}': {}x{} with {} pixel data.", identifier, width, height, pixels == nullptr ? "no" : "empty");
        return std::unexpected(TextureUploadError::NoPixelData);
    }

    // Hashed rather than copied: the table only ever needs to answer "is this
    // the same content I already have a slot for", and keeping the pixels here
    // would make the renderer the second owner of every texture's CPU image.
    const uint64_t pixelHash = Hash64(static_cast<const char*>(pixels), pixelBytes);

    // Re-creating a named texture used to leak: the old image and its slot
    // stayed resident for the life of the device because nothing handed a
    // bindless index back. A byte-identical re-upload is answered from the
    // record, so a caller that rebuilds the same atlas per scene does not churn
    // slots at all; a genuine content change goes through and releases the slot
    // the old copy held, so repeated regeneration no longer accumulates.
    const bool duplicate = Lock(_mutex, [&] -> bool {
        const auto* existing = _textures.Find(id);
        if (existing == nullptr) {
            return false;
        }
        if (existing->format == format && existing->width == width && existing->height == height && existing->pixelHash == pixelHash) {
            return true;
        }
        ZHLN::Log(
            "[TextureManager] Texture '{}' re-uploaded with different contents ({}x{} -> {}x{}); releasing bindless slot {} and uploading a replacement.",
            identifier, existing->width, existing->height, width, height, existing->gpuBindlessIndex
        );
        return false;
    });

    if (duplicate) {
        return handle;
    }

    // A genuine content change replaces the record below; releasing the slot
    // the old copy held first is what keeps repeated regeneration from eating
    // the index space. The replacement cannot land in the same slot this frame:
    // released slots are handed back out only once reclamation has run, which
    // is also what makes the release safe against frames that still read the
    // old descriptor. Unload is a no-op when the name was never registered,
    // which is the first-upload case.
    Unload(handle);

    auto uploaded = Upload2D(pixels, width, height, format);
    if (!uploaded) {
        return std::unexpected(uploaded.error());
    }

    Lock(_mutex, [&] {
        _textures.Insert(
            id, TextureRecord {
                    .handle           = handle,
                    .identifier       = String256(identifier),
                    .format           = format,
                    .width            = width,
                    .height           = height,
                    .pixelHash        = pixelHash,
                    .gpuBindlessIndex = *uploaded
                }
        );
    });
    return handle;
}

auto TextureManager::RegisterUploaded(std::string_view identifier, uint32_t bindlessIndex, VkFormat format) -> TextureHandle {
    const uint64_t id     = HashAssetID(identifier);
    const auto     handle = static_cast<TextureHandle>(id);

    Lock(_mutex, [&] {
        _textures.Insert(
            id, TextureRecord {
                    .handle           = handle,
                    .identifier       = String256(identifier),
                    .format           = format,
                    .width            = 0,
                    .height           = 0,
                    .pixelHash        = 0,
                    .gpuBindlessIndex = bindlessIndex
                }
        );
    });

    return handle;
}

uint32_t TextureManager::GetBindlessIndex(TextureHandle handle) const noexcept {
    if (handle == TextureHandle::Invalid) {
        return kFallbackWhiteTextureIndex;
    }

    const auto id = static_cast<uint64_t>(handle);
    return Lock(_mutex, [&]() -> uint32_t {
        if (const auto* record = _textures.Find(id)) {
            return record->gpuBindlessIndex;
        }

        // Diagnostic check: Alert if raw integers or unmapped IDs are being passed
        if constexpr (isDev) {
            static uint32_t s_WarnCount = 0;
            if (s_WarnCount++ < 5) {
                ZHLN::Log(
                    "[Warning] TextureHandle {:#X} was not found in registry! "
                    "Did you pass a raw integer instead of a registered asset handle?",
                    id
                );
            }
        }

        return kFallbackWhiteTextureIndex; // Safe fallback
    });
}

void TextureManager::Unload(TextureHandle handle) {
    if (handle == TextureHandle::Invalid) {
        return;
    }

    const uint64_t id = static_cast<uint64_t>(handle);
    const auto     released =
        Lock(_mutex, [&]() -> std::optional<uint32_t> {
            const auto* const record = _textures.Find(id);
            if (record == nullptr) {
                return std::nullopt;
            }
            const uint32_t bindlessIndex = record->gpuBindlessIndex;
            _textures.Erase(id);
            return bindlessIndex;
        });

    // Outside the lock: ReleaseSlot touches the slot arrays, which the record
    // table has no business holding the lock across.
    if (released) {
        ReleaseSlot(*released);
    }
}

void TextureManager::Clear() {
    // The records are the only thing that knows which slots are in use, so the
    // indices are collected under the lock and handed back once it is dropped.
    ZHLN::Array<uint32_t> released;
    Lock(_mutex, [&] {
        released.reserve(_textures.Size());
        _textures.ForEach([&](uint64_t /*id*/, TextureRecord& record) {
            released.push_back(record.gpuBindlessIndex);
        });
        _textures.Clear();
    });
    for (const uint32_t bindlessIndex: released) {
        ReleaseSlot(bindlessIndex);
    }
}

void TextureManager::OnDeviceLost() {
    // Everything on the GPU side died with the device. The images and views are
    // dropped here rather than left to the destructor so the manager is left in
    // a state the next device's uploads can run against, and the slot
    // bookkeeping goes with them: the replacement device reserves its own
    // region and starts its high-water mark at the three fallbacks
    // InitializeSystemTextures uploads.
    for (auto& image: _slotImages) {
        image = Vk::Image {};
    }
    _slotImages.clear();
    _slotViews.clear();
    _nextSlotIndex    = 0;
    _bindlessBaseSlot = 0;
    _freeSlots.clear();
    for (auto& pending: _pendingFrees) {
        pending.clear();
    }

    // The records keep their identity -- they are what the engine re-uploads
    // against -- but their bindless indices are meaningless now, so every one
    // points at the white fallback until the re-upload lands.
    Lock(_mutex, [&] {
        _textures.ForEach([&](uint64_t /*id*/, TextureRecord& record) {
            record.gpuBindlessIndex = kFallbackWhiteTextureIndex;
        });
    });
}

auto TextureManager::Adopt(Vk::Image&& image, Vk::ImageView&& view, VkFormat format, uint32_t mipLevels, bool cube) -> std::expected<uint32_t, ErrorCode> {
    // globalTextures[] is addressed by raw offset (base + index), not through
    // the slot allocator, so nothing else bounds this counter: an overrun would
    // spill into the frame partition that follows the array and quietly rewrite
    // a pass's descriptors.
    //
    // A slot handed back by ReleaseSlot is reused before the counter advances,
    // so exhaustion now means 32768 slots are genuinely occupied at once rather
    // than that a caller has been recreating textures. Recoverable, so it is an
    // error rather than an assertion: every caller already substitutes the white
    // fallback for a texture it could not create.
    uint32_t index = 0;
    if (!_freeSlots.empty()) {
        index = _freeSlots.back();
        _freeSlots.pop_back();
    } else {
        if (_nextSlotIndex >= kGlobalTextureSlots) [[unlikely]] {
            ZHLN::Log("[Bindless] globalTextures[] exhausted: all {} slots are occupied. Refusing the upload.", kGlobalTextureSlots);
            // image and view die with this scope: the refusal costs the GPU
            // allocation that was already made, but leaks nothing.
            return std::unexpected(Vk::DescriptorHeapError::ResourceSlotsExhausted);
        }
        index = _nextSlotIndex++;
    }

    // The arrays are slot-indexed rather than append-only: a recycled index is
    // not necessarily the highest one ever handed out.
    if (_slotImages.size() <= index) {
        _slotImages.resize(static_cast<size_t>(index) + 1);
        _slotViews.resize(static_cast<size_t>(index) + 1);
    }

    WriteSlotToHeap(index, image.Handle(), format, mipLevels, cube);
    _slotImages[index] = std::move(image);
    _slotViews[index]  = std::move(view);
    return index;
}

void TextureManager::BeginFrame(uint32_t frameIndex) noexcept {
    // Recorded before the reclaim so a release issued while the queue is still
    // being drained parks into this parity, not the previous one's.
    _frameIndex = frameIndex;

    auto& pending = _pendingFrees[frameIndex];
    for (auto& released: pending) {
        // BeginFrame has already waited on the other parity's fence, so the
        // queue is idle: rewriting the descriptor cannot race a reader. Point
        // the slot at the white fallback -- created 1x1 sRGB in
        // InitializeSystemTextures -- so a stale index still baked into an
        // instance or material resolves to white rather than to the image that
        // is destroyed here.
        WriteSlotToHeap(released.index, _slotImages[kFallbackWhiteTextureIndex].Handle(), VK_FORMAT_R8G8B8A8_SRGB, 1, false);
        _freeSlots.push_back(released.index);
    }
    // Dropping the entries releases the images and views of every slot that was
    // not handed out again. Nothing is in flight, so no deletion queue is
    // needed for them.
    pending.clear();
}

void TextureManager::ReleaseSlot(uint32_t bindlessIndex) noexcept {
    // Black/white/normal are what every failed lookup resolves to and what a
    // released slot is pointed at on reclamation, so they stay resident.
    if (bindlessIndex <= kFallbackNormalTextureIndex || bindlessIndex >= _slotImages.size()) [[unlikely]] {
        return;
    }
    if (!_slotImages[bindlessIndex].Valid()) {
        // Never handed out, or already awaiting reclamation: releasing twice
        // would let one index back two live textures.
        return;
    }

    // The descriptor keeps pointing at this slot until reclamation -- in-flight
    // frames may still be sampling it -- so ownership of the image and view
    // moves into the pending entry instead of dying here.
    _pendingFrees[_frameIndex].push_back(
        ReleasedSlot {.index = bindlessIndex, .image = std::move(_slotImages[bindlessIndex]), .view = std::move(_slotViews[bindlessIndex])}
    );
}

void TextureManager::NameSlots() noexcept {
    for (size_t i = 0; i < _slotImages.size(); ++i) {
        // Slots released and awaiting reclamation hold no image; naming
        // VK_NULL_HANDLE would just trip the debug-utils check.
        if (!_slotImages[i].Valid()) {
            continue;
        }
        Vk::Debug::SetImageName(_ctx, _slotImages[i].Handle(), std::format("BindlessTexture{:03}", i));
    }
}

void TextureManager::WriteSlotToHeap(uint32_t bindlessIndex, VkImage image, VkFormat format, uint32_t mipLevels, bool cube) noexcept {
    // The globalTextures[] array is pinned to a contiguous heap region by the
    // binding-11 mapping; index N lives at slot (base + N).
    const Vk::TextureHandle   slot {_bindlessBaseSlot + bindlessIndex};
    const VkImageViewCreateInfo info = cube ? Vk::MakeViewCreateInfoCube(image, format, mipLevels) :
                                              Vk::MakeViewCreateInfo2D(image, format, mipLevels, VK_IMAGE_ASPECT_COLOR_BIT);
    _heaps.WriteImage(slot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace ZHLN
