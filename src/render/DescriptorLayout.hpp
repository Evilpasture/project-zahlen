// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include "RenderCore.hpp"

#include <array>
#include <type_traits>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

namespace ZHLN::Vk {

// Reporting helpers for descriptor utilities
void ReportBindlessRegistryExceeded(uint32_t bindingID, uint32_t capacity) noexcept;

// ============================================================================
// Binding Descriptors — Compile-Time Slot Definitions
// ============================================================================

template <uint32_t Binding, VkDescriptorType Type, VkShaderStageFlags Stages, uint32_t Count = 1,
		  VkDescriptorBindingFlags Flags = 0>
struct BindingSlot {
	static constexpr uint32_t binding = Binding;
	static constexpr VkDescriptorType type = Type;
	static constexpr VkShaderStageFlags stages = Stages;
	static constexpr uint32_t count = Count;
	static constexpr VkDescriptorBindingFlags flags = Flags;
};

inline constexpr VkDescriptorBindingFlags kBindlessFlags =
	VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

template <uint32_t B, uint32_t Count = 4096, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using BindlessSampledImageSlot =
	BindingSlot<B, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, S, Count, kBindlessFlags>;

template <uint32_t B, uint32_t Count = 4096, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using BindlessCombinedImageSamplerSlot =
	BindingSlot<B, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, S, Count, kBindlessFlags>;

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using SampledImageSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, S>;

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using SamplerSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_SAMPLER, S>;

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using CombinedImageSamplerSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, S>;

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_COMPUTE_BIT>
using StorageImageSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, S>;

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_VERTEX_BIT>
using UniformSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, S>;

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_COMPUTE_BIT>
using StorageBufferSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, S>;

// ============================================================================
// DescriptorWrite Types
// ============================================================================

struct ImageWrite {
	VkImageView view = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

struct SamplerWrite {
	VkSampler sampler = VK_NULL_HANDLE;
};

struct BufferWrite {
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	VkDeviceSize range = VK_WHOLE_SIZE;
};

struct SkipWrite {};

// ============================================================================
// Helper: Extract Slot Info by Binding ID
// ============================================================================

template <uint32_t B, typename... Slots> struct GetSlot;

template <uint32_t B, typename Head, typename... Tail>
struct GetSlot<B, Head, Tail...>
	: std::conditional_t<Head::binding == B, Head, GetSlot<B, Tail...>> {};

template <uint32_t B> struct GetSlot<B> {
	static_assert(sizeof(std::integral_constant<uint32_t, B>) == 0,
				  "The requested Binding ID was not found in the DescriptorLayout definition.");
};

// ============================================================================
// Type-Safe Bindless Registry
// ============================================================================

template <typename Layout, uint32_t BindingID> class BindlessRegistry {
	using Slot = typename Layout::template SlotInfo<BindingID>;
	static_assert((Slot::flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0);

  public:
	BindlessRegistry() = default;

	void Init(const VkDevice device, const VkDescriptorSet set);

	auto RegisterImage(const VkImageView view,
					   const VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		-> uint32_t
		requires(Slot::type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);

	auto RegisterCombined(const VkImageView view, const VkSampler sampler,
						  const VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		-> uint32_t
		requires(Slot::type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

	[[nodiscard]] constexpr auto Capacity() const noexcept -> uint32_t { return Slot::count; }
	[[nodiscard]] auto Size() const noexcept -> uint32_t { return _nextSlot; }

  private:
	auto UpdateDescriptor(VkImageView view, VkSampler sampler, VkImageLayout layout) -> uint32_t;

	VkDevice _device = VK_NULL_HANDLE;
	VkDescriptorSet _set = VK_NULL_HANDLE;
	uint32_t _nextSlot = 0;
};

// ============================================================================
// Descriptor Updater (Zero Allocation System)
// ============================================================================

class DescriptorUpdater {
	std::array<VkWriteDescriptorSet, 32> _writes{};
	std::array<VkDescriptorImageInfo, 32> _imageInfos{};
	std::array<VkDescriptorBufferInfo, 32> _bufferInfos{};
	uint32_t _writeCount = 0;
	uint32_t _imageCount = 0;
	uint32_t _bufferCount = 0;

  public:
	void BindUniformBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset = 0,
						   VkDeviceSize range = VK_WHOLE_SIZE);
	void BindStorageBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset = 0,
						   VkDeviceSize range = VK_WHOLE_SIZE);
	void BindSampledImage(uint32_t binding, VkImageView view, VkSampler sampler = VK_NULL_HANDLE,
						  VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	void BindSampler(uint32_t binding, VkSampler sampler);
	void UpdateSet(VkDevice device, VkDescriptorSet set);
};

// ============================================================================
// DescriptorLayout<Slots...>
// ============================================================================

template <typename T> struct IsTypedImage : std::false_type {};
template <VkImageLayout L> struct IsTypedImage<TypedImage<L>> : std::true_type {};

template <typename... Slots> class DescriptorLayout {
	static constexpr uint32_t kCount = sizeof...(Slots);

	static constexpr auto MakeBindings() noexcept;
	static constexpr auto MakeBindingFlags() noexcept;
	static constexpr auto HasUpdateAfterBind() noexcept -> bool;
	static constexpr auto MakePoolSizes(uint32_t setCount) noexcept;

  public:
	template <uint32_t B> using SlotInfo = GetSlot<B, Slots...>;

	[[nodiscard]] static auto CreateLayout(VkDevice device) noexcept
		-> ZHLN::Vk::DescriptorSetLayout;
	[[nodiscard]] static auto CreatePool(VkDevice device, uint32_t maxSets) noexcept
		-> ZHLN::Vk::DescriptorPool;
	[[nodiscard]] static auto Allocate(VkDevice device, VkDescriptorPool pool,
									   VkDescriptorSetLayout layout) noexcept -> VkDescriptorSet;

	template <typename... Args>
		requires(sizeof...(Args) == sizeof...(Slots))
	static void Write(VkDevice device, VkDescriptorSet set, Args&&... args) noexcept;

  private:
	template <typename ArgTuple, size_t... I>
	static void WriteAll(VkDevice device, VkDescriptorSet set, ArgTuple& args,
						 std::array<VkDescriptorImageInfo, kCount>& imageInfos,
						 std::array<VkDescriptorBufferInfo, kCount>& bufferInfos,
						 std::array<VkWriteDescriptorSet, kCount>& writes,
						 std::index_sequence<I...> /*unused*/) noexcept;

	template <size_t I, typename Slot, typename Arg>
	static void WriteSlot(VkDescriptorSet set, Arg&& arg, VkDescriptorImageInfo& imageInfo,
						  VkDescriptorBufferInfo& bufferInfo, VkWriteDescriptorSet& write) noexcept;
};

} // namespace ZHLN::Vk

#include "DescriptorLayout.inl"
