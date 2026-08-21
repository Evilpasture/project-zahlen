// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TextureManager.hpp"
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <cstddef>

namespace ZHLN {

TextureHandle TextureManager::Load(RenderContext& rc, CreativeWorksManager& cwMgr, std::string_view path, bool isSRGB) {
    uint64_t id     = HashAssetID(path);
    auto     handle = static_cast<TextureHandle>(id);
    Lock(_mutex, [&] {
        if (_textures.Find(id) != nullptr) {
            return;
        }
        uint32_t bindlessIdx = CreativeWorksFactory::LoadTexture(rc, cwMgr, path, isSRGB);
        _textures.Insert(
            id,
            TextureRecord {.handle = handle, .path = String256(path), .isSRGB = isSRGB, .isProcedural = false, .cpuPixels = {}, .gpuBindlessIndex = bindlessIdx}
        );
    });
    return handle;
}

TextureHandle TextureManager::CreateProcedural(RenderContext& rc, std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels) {
    uint64_t              id          = HashAssetID(name);
    auto                  handle      = static_cast<TextureHandle>(id);
    auto                  texRes      = rc.CreateTexture(pixels, width, height, isSRGB);
    uint32_t              bindlessIdx = texRes ? *texRes : 1;
    std::vector<uint32_t> pixelCopy;
    if (pixels != nullptr && width > 0 && height > 0) {
        pixelCopy.assign(pixels, pixels + (static_cast<size_t>(width * height)));
    }
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
        return 1; // Default white
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

        return 1; // Safe fallback
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

void TextureManager::RebuildGPUResources(RenderContext& rc, CreativeWorksManager& cwMgr) {
    Lock(_mutex, [&] {
        ZHLN::Log("[TextureManager] Rebuilding GPU texture resources after Device Loss...");
        _textures.ForEach([&](uint64_t /*id*/, TextureRecord& record) {
            if (record.isProcedural) {
                if (!record.cpuPixels.empty()) {
                    auto texRes             = rc.CreateTexture(record.cpuPixels.data(), record.width, record.height, record.isSRGB);
                    record.gpuBindlessIndex = texRes ? *texRes : 1;
                }
            } else {
                record.gpuBindlessIndex = CreativeWorksFactory::LoadTexture(rc, cwMgr, record.path.c_str(), record.isSRGB);
            }
        });
    });
}

void TextureManager::Unload(TextureHandle handle) noexcept {
    if (handle == TextureHandle::Invalid) {
        return;
    }
    Lock(_mutex, [&] {
        // Slot memory recycled during render frame cleanup
    });
}

void TextureManager::Clear() noexcept {
    Lock(_mutex, [&] { _textures.Clear(); });
}

} // namespace ZHLN
