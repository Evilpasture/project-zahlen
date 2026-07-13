// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// RenderParams.hpp
#pragma once
#include "Rendering.hpp"
#include "Zahlen/Config.hpp"
#include <tuple>

namespace ZHLN {

using EngineASType = std::conditional_t<isMac, Vk::SkipWrite, const VkAccelerationStructureKHR*>;

struct AmbientPassParams {
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> sceneColor;
    VkSampler                                                defaultSampler = VK_NULL_HANDLE;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> normRough;
    VkSampler                                                pointSampler       = VK_NULL_HANDLE;
    VkImageView                                              prefilteredView    = VK_NULL_HANDLE;
    VkImageView                                              brdfLutView        = VK_NULL_HANDLE;
    VkSampler                                                clampSampler       = VK_NULL_HANDLE;
    VkBuffer                                                 frameUniformBuffer = VK_NULL_HANDLE;

    [[nodiscard]] constexpr auto AsTuple() const noexcept {
        return std::tuple {sceneColor, defaultSampler, depth, normRough, pointSampler, prefilteredView, brdfLutView, clampSampler, frameUniformBuffer};
    }
};

struct LightingPassParams {
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> sceneColor;
    VkSampler                                                defaultSampler = VK_NULL_HANDLE;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> normRough;
    VkBuffer                                                 lightStorageBuffer   = VK_NULL_HANDLE;
    VkBuffer                                                 frameUniformBuffer   = VK_NULL_HANDLE;
    VkImageView                                              shadowMapView        = VK_NULL_HANDLE;
    VkSampler                                                shadowSampler        = VK_NULL_HANDLE;
    VkImageView                                              ltcMatView           = VK_NULL_HANDLE;
    VkImageView                                              ltcAmpView           = VK_NULL_HANDLE;
    VkSampler                                                clampSampler         = VK_NULL_HANDLE;
    VkBuffer                                                 clusterGridBuffer    = VK_NULL_HANDLE;
    VkBuffer                                                 lightIndexListBuffer = VK_NULL_HANDLE;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> ambientTarget;
    VkSampler                                                pointSampler = VK_NULL_HANDLE;
    EngineASType                                             tlas {};
    VkImageView                                              shadowAtlasCubeView = VK_NULL_HANDLE;
    VkImageView                                              shadowAtlas2DView   = VK_NULL_HANDLE;

    [[nodiscard]] constexpr auto AsTuple() const noexcept {
        return std::tuple {sceneColor,           defaultSampler, depth,        normRough,  lightStorageBuffer,  frameUniformBuffer,
                           shadowMapView,        shadowSampler,  ltcMatView,   ltcAmpView, clampSampler,        clusterGridBuffer,
                           lightIndexListBuffer, ambientTarget,  pointSampler, tlas,       shadowAtlasCubeView, shadowAtlas2DView};
    }
};

struct ReflectionPassParams {
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> sceneColor;
    VkSampler                                                defaultSampler = VK_NULL_HANDLE;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> normRough;
    VkSampler                                                pointSampler    = VK_NULL_HANDLE;
    VkImageView                                              prefilteredView = VK_NULL_HANDLE;
    EngineASType                                             tlas {};
    VkBuffer                                                 frameUniformBuffer = VK_NULL_HANDLE;
    VkImageView                                              brdfLutView        = VK_NULL_HANDLE;
    VkSampler                                                clampSampler       = VK_NULL_HANDLE;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> lightingTarget;
    Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> voxelIntegrated;

    [[nodiscard]] constexpr auto AsTuple() const noexcept {
        return std::tuple {sceneColor, defaultSampler,     depth,       normRough,    pointSampler,   prefilteredView,
                           tlas,       frameUniformBuffer, brdfLutView, clampSampler, lightingTarget, voxelIntegrated};
    }
};

} // namespace ZHLN
