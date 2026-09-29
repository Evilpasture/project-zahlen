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
    HandleCollision ZHLN_ANNOTATION(ZHLN::Description<"Texture name hashes to a reserved or different texture handle"> {}),
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

auto TextureManager::UploadCube(const void* const* faceData, uint32_t size) -> std::expected<TextureHandle, ErrorCode> {
    const std::span<const void* const, 6> faces {faceData, 6};

    return Vk::TextureUploader(_ctx, _allocator, _staging, _cmdRing)
        .UploadCube({.faceData = faces, .size = size, .format = VK_FORMAT_R8G8B8A8_UNORM})
        .and_then([&](Vk::TextureResource tex) -> std::expected<TextureHandle, ErrorCode> {
            auto index = Adopt(std::move(tex.image), std::move(tex.view));
            return index.transform([&](uint32_t slot) {
                Vk::Debug::SetImageName(_ctx, _slotImages[slot].Handle(), std::format("BindlessCubeTexture{:03}", slot));
                return RegisterAnonymous(slot, VK_FORMAT_R8G8B8A8_UNORM, size, size);
            });
        });
}

auto TextureManager::UploadUnnamed(const void* pixels, uint32_t width, uint32_t height, VkFormat format)
    -> std::expected<TextureHandle, ErrorCode> {
    return Upload2D(pixels, width, height, format).transform([&](uint32_t slot) {
        return RegisterAnonymous(slot, format, width, height);
    });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

auto TextureManager::Upload(std::string_view identifier, const void* pixels, uint32_t width, uint32_t height, VkFormat format)
    -> std::expected<TextureHandle, ErrorCode> {
    const uint64_t id         = HashAssetID(identifier);
    const auto     handle     = static_cast<TextureHandle>(id);
    const size_t   pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t);

    ZHLN::Assert(pixels != nullptr && pixelBytes > 0, "TextureManager::Upload('{}'): no pixel data for a {}x{} texture", identifier, width, height);
    if (pixels == nullptr || pixelBytes == 0) {
        ZHLN::Log("[TextureManager] Refusing upload '{}': {}x{} with {} pixel data.", identifier, width, height, pixels == nullptr ? "no" : "empty");
        return std::unexpected(TextureUploadError::NoPixelData);
    }
    if (id <= static_cast<uint64_t>(SystemTextures::FlatNormal)) {
        return std::unexpected(TextureUploadError::HandleCollision);
    }

    const uint64_t pixelHash = Hash64(static_cast<const char*>(pixels), pixelBytes);

    enum class ExistingTexture : uint8_t { Missing, Duplicate, Changed, Collision };
    const auto stateOf = [&](const TextureRecord* existing) -> ExistingTexture {
        if (existing == nullptr) {
            return ExistingTexture::Missing;
        }
        if (!existing->named || existing->identifier != identifier) {
            return ExistingTexture::Collision;
        }
        // After device loss the record still exists but points to white, not its old image.
        if (existing->gpuBindlessIndex > kFallbackNormalTextureIndex && existing->format == format && existing->width == width &&
            existing->height == height && existing->pixelHash == pixelHash) {
            return ExistingTexture::Duplicate;
        }
        return ExistingTexture::Changed;
    };
    const auto initialState = Lock(_mutex, [&] { return stateOf(_textures.Find(id)); });
    if (initialState == ExistingTexture::Collision) {
        return std::unexpected(TextureUploadError::HandleCollision);
    }
    if (initialState == ExistingTexture::Duplicate) {
        return handle;
    }

    auto uploaded = Upload2D(pixels, width, height, format);
    if (!uploaded) {
        return std::unexpected(uploaded.error());
    }

    // The upload can fail without disturbing an existing texture. Recheck the
    // registry before inserting: another caller may have uploaded this name,
    // or an anonymous handle may have claimed its hash while the GPU worked.
    std::optional<uint32_t> oldSlot;
    const auto committedState = Lock(_mutex, [&] {
        const auto* previous = _textures.Find(id);
        const auto  state    = stateOf(previous);
        if (state == ExistingTexture::Collision || state == ExistingTexture::Duplicate) {
            return state;
        }
        if (state == ExistingTexture::Changed) {
            oldSlot = previous->gpuBindlessIndex;
            ZHLN::Log(
                "[TextureManager] Texture '{}' re-uploaded with different contents ({}x{} -> {}x{}); retiring bindless slot {}.",
                identifier, previous->width, previous->height, width, height, *oldSlot
            );
        }
        _textures.Insert(
            id, TextureRecord {
                    .handle           = handle,
                    .identifier       = std::string(identifier),
                    .named            = true,
                    .format           = format,
                    .width            = width,
                    .height           = height,
                    .pixelHash        = pixelHash,
                    .gpuBindlessIndex = *uploaded
                }
        );
        return state;
    });

    if (committedState == ExistingTexture::Collision || committedState == ExistingTexture::Duplicate) {
        ReleaseSlot(*uploaded);
        if (committedState == ExistingTexture::Collision) {
            return std::unexpected(TextureUploadError::HandleCollision);
        }
    } else if (oldSlot) {
        ReleaseSlot(*oldSlot);
    }
    return handle;
}

auto TextureManager::RegisterAnonymous(uint32_t bindlessIndex, VkFormat format, uint32_t width, uint32_t height) -> TextureHandle {
    return Lock(_mutex, [&] {
        uint64_t id = 0;
        do {
            id = _nextAnonymousHandle++;
        } while (id <= static_cast<uint64_t>(SystemTextures::FlatNormal) || _textures.Find(id) != nullptr);

        const auto handle = static_cast<TextureHandle>(id);
        _textures.Insert(
            id, TextureRecord {
                    .handle           = handle,
                    .format           = format,
                    .width            = width,
                    .height           = height,
                    .gpuBindlessIndex = bindlessIndex
                }
        );
        return handle;
    });
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

TextureManager::~TextureManager() { DestroyAllSlots(); }

void TextureManager::DestroyAllSlots() noexcept {
    // Never destroy a VMA image while any of its Vulkan views still exists.
    _slotViews.clear();
    for (auto& pending: _pendingFrees) {
        for (auto& released: pending) {
            released.view = {};
        }
    }
    for (auto& image: _slotImages) {
        _allocator.DestroyImage(image);
    }
    _slotImages.clear();
    for (auto& pending: _pendingFrees) {
        for (auto& released: pending) {
            _allocator.DestroyImage(released.image);
        }
        pending.clear();
    }
    _nextSlotIndex = 0;
    _bindlessBaseSlot = 0;
    _freeSlots.clear();
}

void TextureManager::OnDeviceLost() {
    DestroyAllSlots();
    Lock(_mutex, [&] {
        _textures.ForEach([&](uint64_t, TextureRecord& record) {
            record.gpuBindlessIndex = kFallbackWhiteTextureIndex;
        });
    });
}

auto TextureManager::AdoptTexture(Vk::Image image, Vk::ImageView view, uint32_t width, uint32_t height)
    -> std::expected<TextureHandle, ErrorCode> {
    const VkFormat format = view.Info().format;
    return Adopt(std::move(image), std::move(view)).transform([&](uint32_t slot) {
        return RegisterAnonymous(slot, format, width, height);
    });
}

auto TextureManager::Adopt(Vk::Image image, Vk::ImageView view) -> std::expected<uint32_t, ErrorCode> {
    uint32_t index = 0;
    if (!_freeSlots.empty()) {
        index = _freeSlots.back();
        _freeSlots.pop_back();
    } else {
        if (_nextSlotIndex >= kGlobalTextureSlots) [[unlikely]] {
            ZHLN::Log("[Bindless] globalTextures[] exhausted: all {} slots are occupied. Refusing the upload.", kGlobalTextureSlots);
            // A consuming sink also owns its rejected arguments.
            view = {};
            _allocator.DestroyImage(image);
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

void TextureManager::RetireBatch(ZHLN::Array<ReleasedSlot>& pending) noexcept {
    for (auto& released: pending) {
        WriteSlotToHeap(released.index, _slotViews[kFallbackWhiteTextureIndex]);
        released.view = {};
        _allocator.DestroyImage(released.image);
        _freeSlots.push_back(released.index);
    }
    pending.clear();
}

void TextureManager::BeginFrame(uint32_t frameIndex) noexcept {
    _frameIndex = Vk::FrameSlot(frameIndex);
    // The frame slot's fence was waited on before this call.
    RetireBatch(_pendingFrees[_frameIndex]);
}

void TextureManager::RetireAll() noexcept {
    for (auto& pending: _pendingFrees) {
        RetireBatch(pending);
    }
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
