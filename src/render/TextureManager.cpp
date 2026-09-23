// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TextureManager.hpp"
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstddef>
#include <cstring>

namespace ZHLN {

namespace {
// globalTextures[1], uploaded by InitializeSystemTextures before anything
// else can allocate a slot. Every lookup and upload failure resolves here so
// a missing texture renders as untinted white rather than as garbage.
constexpr uint32_t kWhiteFallbackBindlessIndex = 1;
} // namespace

TextureHandle TextureManager::Load(RenderContext& rc, AssetManager& cwMgr, std::string_view path, bool isSRGB) {
    uint64_t id     = HashAssetID(path);
    auto     handle = static_cast<TextureHandle>(id);
    Lock(_mutex, [&] {
        if (_textures.Find(id) != nullptr) {
            return;
        }
        uint32_t bindlessIdx = PrefabFactory::LoadTexture(rc, cwMgr, path, isSRGB);
        _textures.Insert(
            id,
            TextureRecord {.handle = handle, .path = String256(path), .isSRGB = isSRGB, .isProcedural = false, .cpuPixels = {}, .gpuBindlessIndex = bindlessIdx}
        );
    });
    return handle;
}

TextureHandle TextureManager::CreateProcedural(RenderContext& rc, std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels) {
    uint64_t     id         = HashAssetID(name);
    auto         handle     = static_cast<TextureHandle>(id);
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

    // CreateTextureInternal reads width*height*4 bytes out of `pixels` with no
    // way to signal a short read, so a null source or an empty extent is a
    // caller bug rather than a runtime condition to recover from.
    ZHLN::Assert(pixels != nullptr && pixelCount > 0, "TextureManager::CreateProcedural('{}'): no pixel data for a {}x{} texture", name, width, height);
    if (pixels == nullptr || pixelCount == 0) {
        ZHLN::Log("[TextureManager] Refusing procedural texture '{}': {}x{} with {} pixel data.", name, width, height, pixels == nullptr ? "no" : "empty");
        return TextureHandle::Invalid;
    }

    // Re-creating a procedural texture under a name that already exists used to
    // leak: the old image and its slot stayed resident for the life of the
    // device because nothing handed a bindless index back. An identical
    // re-creation is still answered from the record -- Load() has always
    // deduplicated by asset id this way -- so callers that rebuild the same
    // atlas per scene do not churn slots at all. A genuine content change goes
    // through and releases the slot the old copy held (see below), so repeated
    // regeneration no longer accumulates.
    const bool duplicate = Lock(_mutex, [&] -> bool {
        const auto* existing = _textures.Find(id);
        if (existing == nullptr) {
            return false;
        }
        const bool sameShape = existing->isProcedural && existing->width == width && existing->height == height && existing->isSRGB == isSRGB;
        if (sameShape && existing->cpuPixels.size() == pixelCount && std::memcmp(existing->cpuPixels.data(), pixels, pixelCount * sizeof(uint32_t)) == 0) {
            return true;
        }
        ZHLN::Log(
            "[TextureManager] Procedural texture '{}' recreated with different contents ({}x{} -> {}x{}); releasing bindless slot {} and uploading a replacement.",
            name, existing->width, existing->height, width, height, existing->gpuBindlessIndex
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
    // old descriptor.
    rc.UnloadTexture(handle);

    auto                  texRes      = rc.CreateTexture(pixels, width, height, isSRGB);
    uint32_t              bindlessIdx = texRes ? *texRes : kWhiteFallbackBindlessIndex;
    std::vector<uint32_t> pixelCopy;
    pixelCopy.assign(pixels, pixels + pixelCount);
    Lock(_mutex, [&] {
        _textures.Insert(
            id, TextureRecord {
                    .handle           = handle,
                    .path             = String256(name),
                    .isSRGB           = isSRGB,
                    .isProcedural     = true,
                    .width            = width,
                    .height           = height,
                    .cpuPixels        = std::move(pixelCopy),
                    .gpuBindlessIndex = bindlessIdx
                }
        );
    });
    return handle;
}

uint32_t TextureManager::GetBindlessIndex(TextureHandle handle) const noexcept {
    if (handle == TextureHandle::Invalid) {
        return kWhiteFallbackBindlessIndex;
    }

    auto id = static_cast<uint64_t>(handle);
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

        return kWhiteFallbackBindlessIndex; // Safe fallback
    });
}

TextureHandle TextureManager::RegisterUploaded(std::string_view identifier, uint32_t gpuBindlessIndex, bool isSRGB) {
    uint64_t id     = HashAssetID(identifier);
    auto     handle = static_cast<TextureHandle>(id);

    Lock(_mutex, [&] {
        _textures.Insert(
            id, TextureRecord {
                    .handle           = handle,
                    .path             = String256(identifier),
                    .isSRGB           = isSRGB,
                    .isProcedural     = false,
                    .width            = 0,
                    .height           = 0,
                    .cpuPixels        = {},
                    .gpuBindlessIndex = gpuBindlessIndex
                }
        );
    });

    return handle;
}

void TextureManager::RebuildGPUResources(RenderContext& rc, AssetManager& cwMgr) {
    Lock(_mutex, [&] {
        ZHLN::Log("[TextureManager] Rebuilding GPU texture resources after Device Loss...");
        _textures.ForEach([&](uint64_t /*id*/, TextureRecord& record) {
            if (record.isProcedural) {
                if (!record.cpuPixels.empty()) {
                    auto texRes             = rc.CreateTexture(record.cpuPixels.data(), record.width, record.height, record.isSRGB);
                    record.gpuBindlessIndex = texRes ? *texRes : kWhiteFallbackBindlessIndex;
                }
            } else {
                record.gpuBindlessIndex = PrefabFactory::LoadTexture(rc, cwMgr, record.path.c_str(), record.isSRGB);
            }
        });
    });
}

auto TextureManager::TakeBindlessIndex(TextureHandle handle) noexcept -> std::optional<uint32_t> {
    if (handle == TextureHandle::Invalid) {
        return std::nullopt;
    }

    const uint64_t id = static_cast<uint64_t>(handle);
    return Lock(_mutex, [&]() -> std::optional<uint32_t> {
        const auto* const record = _textures.Find(id);
        if (record == nullptr) {
            return std::nullopt;
        }
        const uint32_t bindlessIndex = record->gpuBindlessIndex;
        _textures.Erase(id);
        return bindlessIndex;
    });
}

auto TextureManager::Clear() -> std::vector<uint32_t> {
    // The records are the only thing that knows which slots are in use, so
    // collect them before dropping the map: a clear that silently forgot them
    // would leak the whole texture index space.
    std::vector<uint32_t> released;
    Lock(_mutex, [&] {
        released.reserve(_textures.Size());
        _textures.ForEach([&](uint64_t /*id*/, TextureRecord& record) {
            released.push_back(record.gpuBindlessIndex);
        });
        _textures.Clear();
    });
    return released;
}

} // namespace ZHLN
