// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/PresentationSurface.hpp
//
// The one place in the engine that is allowed to see both sides of the
// presentation seam at once.
//
// src/window publishes an OS presentation descriptor and knows nothing about
// Vulkan. src/vulkan is a leaf whose only presentation type is VkSurfaceKHR and
// which knows nothing about windows. Somebody has to marry the two, and the
// layer that legitimately knows both is this one: it already links the RHI and
// already calls the loader directly elsewhere (OpenGLHacks/HostBlitSwapchain).
//
// So the descriptor -> VkSurfaceKHR conversion lives here rather than in either
// neighbour, and both of those stay clean.
#pragma once

// VkInstance comes from the loader headers rather than being spelled by hand:
// it is a handle typedef, and re-typedefing it here would be one Vulkan-Headers
// change away from silently disagreeing with the real one. SYSTEM include, same
// as OpenGLHacks/HostBlit.hpp.
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <volk.h>

namespace ZHLN {

class NativeSurfaceHandle;

namespace Vk {
class Surface;
class ExtensionBuilder;
} // namespace Vk

/// @brief Builds the surface for a windowed presentation target.
///
/// Visits the handle's platform descriptor and calls the matching
/// vkCreate*SurfaceKHR, resolved through the loader rather than declared. A
/// target whose platform this build has no WSI for answers
/// Vk::SurfaceCreationError::WindowSurfaceUnsupported rather than guessing.
///
/// A headless target is not an error and not a special case for the caller: it
/// yields a Surface holding VK_NULL_HANDLE, which is exactly the "no WSI in this
/// session" state the renderer already models.
[[nodiscard]] auto CreateSurfaceFromNative(VkInstance instance, const NativeSurfaceHandle& handle) noexcept -> std::expected<Vk::Surface, ErrorCode>;

/// @brief Appends the instance extensions a presentation target's surface needs.
///
/// Asks for the WSI that matches what the OS actually gave us --
/// VK_KHR_win32_surface for an HWND, VK_KHR_wayland_surface for a wl_surface,
/// nothing at all for a headless or Metal target, because there is no WSI to ask
/// for. An extension the target needs but the driver lacks is recorded as
/// missing by Require(), so Build() reports it with its name.
void AppendPlatformSurfaceExtensions(Vk::ExtensionBuilder& builder, const NativeSurfaceHandle& handle) noexcept;

} // namespace ZHLN
