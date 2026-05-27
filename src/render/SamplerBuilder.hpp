#pragma once

#include "RenderCore.hpp"

#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

class SamplerBuilder {
  public:
	SamplerBuilder() noexcept {
		_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.mipLodBias = 0.0f,
			.anisotropyEnable = VK_FALSE,
			.maxAnisotropy = 1.0f,
			.compareEnable = VK_FALSE,
			.compareOp = VK_COMPARE_OP_ALWAYS,
			.minLod = 0.0f,
			.maxLod = VK_LOD_CLAMP_NONE,
			.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
			.unnormalizedCoordinates = VK_FALSE,
		};
	}

	auto Linear() noexcept -> SamplerBuilder& {
		_info.magFilter = VK_FILTER_LINEAR;
		_info.minFilter = VK_FILTER_LINEAR;
		_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		return *this;
	}

	auto Nearest() noexcept -> SamplerBuilder& {
		_info.magFilter = VK_FILTER_NEAREST;
		_info.minFilter = VK_FILTER_NEAREST;
		_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		return *this;
	}

	auto Repeat() noexcept -> SamplerBuilder& {
		_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		return *this;
	}

	auto ClampToEdge() noexcept -> SamplerBuilder& {
		_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		return *this;
	}

	auto ClampToBorder(VkBorderColor color) noexcept -> SamplerBuilder& {
		_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		_info.borderColor = color;
		return *this;
	}

	auto Anisotropy(float maxAniso = 16.0f) noexcept -> SamplerBuilder& {
		_info.anisotropyEnable = VK_TRUE;
		_info.maxAnisotropy = maxAniso;
		return *this;
	}

	auto DepthCompare(VkCompareOp op = VK_COMPARE_OP_LESS_OR_EQUAL) noexcept -> SamplerBuilder& {
		_info.compareEnable = VK_TRUE;
		_info.compareOp = op;
		return *this;
	}

	[[nodiscard]] auto Build(VkDevice device) const noexcept -> Sampler {
		VkSampler sampler = VK_NULL_HANDLE;
		vkCreateSampler(device, &_info, nullptr, &sampler);
		return {device, sampler};
	}

  private:
	VkSamplerCreateInfo _info{};
};

} // namespace ZHLN::Vk
