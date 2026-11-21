// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <volk.h>

namespace ZHLN {

class NativeSurfaceHandle;

namespace Vk {
class Surface;
class ExtensionBuilder;
}

[[nodiscard]] auto CreateSurfaceFromNative(VkInstance instance, const NativeSurfaceHandle& handle) noexcept -> std::expected<Vk::Surface, ErrorCode>;

void AppendPlatformSurfaceExtensions(Vk::ExtensionBuilder& builder, const NativeSurfaceHandle& handle) noexcept;

}
