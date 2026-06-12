// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


// src/render/SamplerBuilder.hpp

#pragma once

#include "RenderCore.hpp"

#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

class SamplerBuilder {
  public:
	SamplerBuilder() noexcept;

	auto Linear() noexcept -> SamplerBuilder&;
	auto Nearest() noexcept -> SamplerBuilder&;
	auto Repeat() noexcept -> SamplerBuilder&;
	auto ClampToEdge() noexcept -> SamplerBuilder&;
	auto ClampToBorder(VkBorderColor color) noexcept -> SamplerBuilder&;
	auto Anisotropy(float maxAniso = 16.0f) noexcept -> SamplerBuilder&;
	auto DepthCompare(VkCompareOp op = VK_COMPARE_OP_LESS_OR_EQUAL) noexcept -> SamplerBuilder&;

	[[nodiscard]] auto Build(VkDevice device) const noexcept -> Sampler;

  private:
	VkSamplerCreateInfo _info{};
};

} // namespace ZHLN::Vk
