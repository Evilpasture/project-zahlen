// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/TextureManager.hpp"
#include <Zahlen/Render.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Core/ControlFlow.hpp>

namespace ZHLN {

TextureHandle TextureManager::Load(RenderContext& rc, CreativeWorksManager& cwMgr, std::string_view path, bool isSRGB) {
    uint64_t id = HashAssetID(path);
    auto handle = static_cast<TextureHandle>(id);
    Lock(_mutex, [&] {
        if (_textures.Find(id) != nullptr) {
            return;
        }
        uint32_t bindlessIdx = CreativeWorksFactory::LoadTexture(rc, cwMgr, path, isSRGB);
        _textures.Insert(id, TextureRecord{
            .handle = handle,
            .path = String256(path),
            .isSRGB = isSRGB,
            .isProcedural = false,
            .cpuPixels = {},
            .gpuBindlessIndex = bindlessIdx
        });
    });
    return handle;
}

TextureHandle TextureManager::CreateProcedural(
    RenderContext& rc,
    std::string_view name,
    uint32_t width,
    uint32_t height,
    bool isSRGB,
    const uint32_t* pixels
) {
    uint64_t id = HashAssetID(name);
    auto handle = static_cast<TextureHandle>(id);
    auto texRes = rc.CreateTexture(pixels, width, height, isSRGB);
    uint32_t bindlessIdx = texRes ? *texRes : 1;
    std::vector<uint32_t> pixelCopy;
    if (pixels != nullptr && width > 0 && height > 0) {
        pixelCopy.assign(pixels, pixels + (width * height));
    }
    Lock(_mutex, [&] {
        _textures.Insert(id, TextureRecord{
            .handle = handle,
            .path = String256(name),
            .isSRGB = isSRGB,
            .isProcedural = true,
            .width = width,
            .height = height,
            .cpuPixels = std::move(pixelCopy),
            .gpuBindlessIndex = bindlessIdx
        });
    });
    return handle;
}

uint32_t TextureManager::GetBindlessIndex(TextureHandle handle) const noexcept {
    if (handle == TextureHandle::Invalid) {
        return 1;
    }
    auto id = static_cast<uint64_t>(handle);
    return Lock(const_cast<Mutex&>(_mutex), [&]() -> uint32_t {
        if (const auto* record = _textures.Find(id)) {
            return record->gpuBindlessIndex;
        }
        return 1;
    });
}

void TextureManager::RebuildGPUResources(RenderContext& rc, CreativeWorksManager& cwMgr) {
    Lock(_mutex, [&] {
        ZHLN::Log("[TextureManager] Rebuilding GPU texture resources after Device Loss...");
        _textures.ForEach([&](uint64_t /*id*/, TextureRecord& record) {
            if (record.isProcedural) {
                if (!record.cpuPixels.empty()) {
                    auto texRes = rc.CreateTexture(record.cpuPixels.data(), record.width, record.height, record.isSRGB);
                    record.gpuBindlessIndex = texRes ? *texRes : 1;
                }
            } else {
                record.gpuBindlessIndex = CreativeWorksFactory::LoadTexture(rc, cwMgr, record.path.c_str(), record.isSRGB);
            }
        });
    });
}

void TextureManager::Unload(TextureHandle handle) noexcept {
    if (handle == TextureHandle::Invalid) return;
    Lock(_mutex, [&] {
        // Slot memory recycled during render frame cleanup
    });
}

void TextureManager::Clear() noexcept {
    Lock(_mutex, [&] {
        _textures.Clear();
    });
}

} // namespace ZHLN
