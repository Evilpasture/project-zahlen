// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"
#include "vulkan/memory/Allocator.hpp"

namespace ZHLN::Vk {

struct TextureResource {
    Image      image;
    ImageView  view {};
    VkExtent3D extent {};

    void Destroy(Allocator& allocator) noexcept {
        view = {};
        allocator.DestroyImage(image);
        extent = {};
    }

    [[nodiscard]] auto AsSlice() const noexcept -> ImageSlice {
        return ImageSlice {view, extent};
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return image.Valid() && view.Handle() != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }
};

}
