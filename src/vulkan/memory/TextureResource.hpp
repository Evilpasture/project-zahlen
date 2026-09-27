// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct TextureResource {
    Image                 image;
    ImageView             view;
    VkImageViewCreateInfo viewInfo {};
    VkExtent3D            extent {};
    VkFormat              format = VK_FORMAT_UNDEFINED;
    uint32_t              mipLevels = 1;
    bool                  isCube = false;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return image.Valid() && view.Valid();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }
};

}
