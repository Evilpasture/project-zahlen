// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


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

enum class TextureUploadError : uint8_t {
    NoPixelData ZHLN_ANNOTATION(ZHLN::Description<"Texture upload was handed no pixel data"> {}) = 1,
};

}

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
            const auto index = Adopt(std::move(tex.image), std::move(tex.view));
            if (index) {
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
            const auto index = Adopt(std::move(tex.image), std::move(tex.view));
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
    const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);

    ZHLN::Assert(pixels != nullptr && pixelBytes > 0, "TextureManager::Upload('{}'): no pixel data for a {}x{} texture", identifier, width, height);
    if (pixels == nullptr || pixelBytes == 0) {
        ZHLN::Log("[TextureManager] Refusing upload '{}': {}x{} with {} pixel data.", identifier, width, height, pixels == nullptr ? "no" : "empty");
        return std::unexpected(TextureUploadError::NoPixelData);
    }

    const uint64_t pixelHash = Hash64(static_cast<const char*>(pixels), pixelBytes);

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

        return kFallbackWhiteTextureIndex;
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

    if (released) {
        ReleaseSlot(*released);
    }
}

void TextureManager::Clear() {
    ZHLN::Array<uint32_t> released;
    Lock(_mutex, [&] {
        released.reserve(_textures.Size());
        _textures.ForEach([&](uint64_t , TextureRecord& record) {
            released.push_back(record.gpuBindlessIndex);
        });
        _textures.Clear();
    });
    for (const uint32_t bindlessIndex: released) {
        ReleaseSlot(bindlessIndex);
    }
}

void TextureManager::OnDeviceLost() {
    // Views must be released before their images, including pending slot frees.
    _slotViews.clear();
    _slotImages.clear();
    _nextSlotIndex    = 0;
    _bindlessBaseSlot = 0;
    _freeSlots.clear();
    for (auto& pending: _pendingFrees) {
        pending.clear();
    }

    Lock(_mutex, [&] {
        _textures.ForEach([&](uint64_t , TextureRecord& record) {
            record.gpuBindlessIndex = kFallbackWhiteTextureIndex;
        });
    });
}

auto TextureManager::Adopt(Vk::Image&& image, Vk::ImageView&& view) -> std::expected<uint32_t, ErrorCode> {
    uint32_t index = 0;
    if (!_freeSlots.empty()) {
        index = _freeSlots.back();
        _freeSlots.pop_back();
    } else {
        if (_nextSlotIndex >= kGlobalTextureSlots) [[unlikely]] {
            ZHLN::Log("[Bindless] globalTextures[] exhausted: all {} slots are occupied. Refusing the upload.", kGlobalTextureSlots);
            return std::unexpected(Vk::DescriptorHeapError::ResourceSlotsExhausted);
        }
        index = _nextSlotIndex++;
    }

    if (_slotImages.size() <= index) {
        _slotImages.resize(static_cast<size_t>(index) + 1);
        _slotViews.resize(static_cast<size_t>(index) + 1);
    }

    WriteSlotToHeap(index, view);
    _slotViews[index]  = std::move(view);
    _slotImages[index] = std::move(image);
    return index;
}

void TextureManager::BeginFrame(uint32_t frameIndex) noexcept {
    _frameIndex = Vk::FrameSlot(frameIndex);

    auto& pending = _pendingFrees[_frameIndex];
    for (auto& released: pending) {
        WriteSlotToHeap(released.index, _slotViews[kFallbackWhiteTextureIndex]);
        _freeSlots.push_back(released.index);
    }
    pending.clear();
}

void TextureManager::ReleaseSlot(uint32_t bindlessIndex) noexcept {
    if (bindlessIndex <= kFallbackNormalTextureIndex || bindlessIndex >= _slotImages.size()) [[unlikely]] {
        return;
    }
    if (!_slotImages[bindlessIndex].Valid()) {
        return;
    }

    _pendingFrees[_frameIndex].push_back(
        ReleasedSlot {.index = bindlessIndex, .image = std::move(_slotImages[bindlessIndex]), .view = std::move(_slotViews[bindlessIndex])}
    );
}

void TextureManager::NameSlots() noexcept {
    for (size_t i = 0; i < _slotImages.size(); ++i) {
        if (!_slotImages[i].Valid()) {
            continue;
        }
        Vk::Debug::SetImageName(_ctx, _slotImages[i].Handle(), std::format("BindlessTexture{:03}", i));
    }
}

void TextureManager::WriteSlotToHeap(uint32_t bindlessIndex, const Vk::ImageView& view) noexcept {
    const Vk::TextureHandle slot {_bindlessBaseSlot + bindlessIndex};
    _heaps.WriteImage(slot, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}
