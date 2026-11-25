// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Where "the device is gone" is decided, in one place, for every renderer.
//
// It names the six enums that carry a DeviceLost member and nothing else, so a
// render TU can ask the question without pulling RenderContext::Impl.
//
// GPU build only: the six declarations bring in vulkan/VulkanHeader.hpp and thus
// volk, which the null backend (ZHLN_BUILD_RENDER=OFF) does not have on its
// include path -- that build compiles null/RenderContextNull.cpp alone, links
// neither zahlen_vulkan nor the GPU passes, and defines a *different*
// RenderContext::Impl. It answers the same question with the one code it can
// produce, FrameResult::DeviceLost.
//
// FrameResult::DeviceLost is accepted alongside the six: once a failure has been
// folded at the frame boundary it is still the same fact, and that is the
// spelling the recovery path holds.

#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Render/FrameResult.hpp>

#include "vulkan/core/RenderCore.hpp"                  // VulkanCallError
#include "vulkan/execution/CommandPool.hpp"            // CommandPoolError
#include "vulkan/execution/CommandRecorder.hpp"        // CommandRecordingError
#include "vulkan/execution/RenderQueue.hpp"            // CommandRingError
#include "vulkan/presentation/Swapchain.hpp"           // SwapchainError
#include "vulkan/presentation/SwapchainPresenter.hpp"  // PresentationError

namespace ZHLN {

[[nodiscard]] inline auto IsDeviceLost(ErrorCode code) noexcept -> bool {
    return code == FrameResult::DeviceLost || code == Vk::VulkanCallError::DeviceLost || code == Vk::CommandPoolError::DeviceLost ||
           code == Vk::CommandRecordingError::DeviceLost || code == Vk::CommandRingError::DeviceLost ||
           code == Vk::SwapchainError::DeviceLost || code == Vk::PresentationError::DeviceLost;
}

} // namespace ZHLN
