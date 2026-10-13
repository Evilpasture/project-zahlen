// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

// An uploaded sampled texture: the image plus the plain view description
// every heap write of it is built from. No VkImageView handle exists for it.
struct TextureResource {
    Image                 image;
    VkImageViewCreateInfo viewInfo {};
    VkExtent3D            extent {};

    void Destroy(Allocator& allocator) noexcept {
        viewInfo = {};
        allocator.DestroyImage(image);
        extent = {};
    }

    [[nodiscard]] auto AsSlice() const noexcept -> ImageSlice {
        return ImageSlice {viewInfo, extent};
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return image.Valid() && viewInfo.image != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }
};

}
