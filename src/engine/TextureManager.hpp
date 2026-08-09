// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Types.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <string_view>
#include <vector>

namespace ZHLN {

class RenderContext;
class CreativeWorksManager;

class TextureManager {
  public:
    TextureManager() = default;
    ~TextureManager() = default;
    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    TextureHandle Load(RenderContext& rc, CreativeWorksManager& cwMgr, std::string_view path, bool isSRGB = true);
    TextureHandle CreateProcedural(RenderContext& rc, std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels);
    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;
    void RebuildGPUResources(RenderContext& rc, CreativeWorksManager& cwMgr);
    void Unload(TextureHandle handle) noexcept;
    void Clear() noexcept;

  private:
    struct TextureRecord {
        TextureHandle handle = TextureHandle::Invalid;
        String256 path;
        bool isSRGB = true;
        bool isProcedural = false;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint32_t> cpuPixels;
        uint32_t gpuBindlessIndex = 1;
    };

    HashMap<uint64_t, TextureRecord> _textures;
    mutable Mutex _mutex;
};

} // namespace ZHLN
