// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/memory/TextureResource.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

/**
 * @brief All-in-one GPU texture bundle: the image, the view created for it,
 *        and the parameters the view was created with, as one RAII unit.
 *
 * The VkImageViewCreateInfo rides along next to the view because
 * VK_EXT_descriptor_heap descriptors consume the create info -- image
 * descriptors in the heaps carry it instead of a handle -- so an upload that
 * produced the view can hand both to AdoptBindlessTexture without rebuilding
 * the info by hand.
 *
 * No declared special members on purpose: the handles are move-only, so the
 * implicit move is move-only and the copies are implicitly deleted, and
 * declaring none of them keeps this an aggregate -- the upload paths hand
 * bundles back with designated initializers.
 */
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

} // namespace ZHLN::Vk
