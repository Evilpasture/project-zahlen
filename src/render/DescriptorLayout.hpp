#pragma once
#include "RenderCore.hpp"

#include <array>
#include <tuple>
#include <type_traits>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

// ============================================================================
// Binding descriptors — compile-time slot definitions
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

// --- Bindless Constants & Aliases ---
inline constexpr VkDescriptorBindingFlags kBindlessFlags =
	VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

template <uint32_t B, uint32_t Count = 4096, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using BindlessSampledImageSlot =
	BindingSlot<B, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, S, Count, kBindlessFlags>;

template <uint32_t B, uint32_t Count = 4096, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using BindlessCombinedImageSamplerSlot =
	BindingSlot<B, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, S, Count, kBindlessFlags>;

// --- Traditional Aliases ---
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
	VkImageView view;
	VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};
struct SamplerWrite {
	VkSampler sampler;
};
struct BufferWrite {
	VkBuffer buffer;
	VkDeviceSize offset = 0;
	VkDeviceSize range = VK_WHOLE_SIZE;
};

// Use this to explicitly skip writing an array slot during initial setup,
// deferring the write to the BindlessRegistry instead.
struct SkipWrite {};

// ============================================================================
// TMP Helper: Extract Slot Info by Binding ID
// ============================================================================

template <uint32_t B, typename... Slots> struct GetSlot;

template <uint32_t B, typename Head, typename... Tail>
struct GetSlot<B, Head, Tail...>
	: std::conditional_t<Head::binding == B, Head, GetSlot<B, Tail...>> {};

template <uint32_t B> struct GetSlot<B> {
	static_assert(B == 0xFFFFFFFF, "Binding ID not found in DescriptorLayout");
};

// ============================================================================
// Type-Safe Bindless Registry
// ============================================================================

template <typename Layout, uint32_t BindingID> class BindlessRegistry {
	// 1. Proof of Correctness: Extract the specific slot definition at compile-time
	using Slot = typename Layout::template SlotInfo<BindingID>;

	static_assert((Slot::flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0,
				  "BindlessRegistry can only be used with UPDATE_AFTER_BIND slots.");

  public:
	BindlessRegistry() = default;

	// No longer need to pass Binding or MaxSlots; they are baked into the Type.
	void Init(const VkDevice device, const VkDescriptorSet set) {
		_device = device;
		_set = set;
		_nextSlot = 0;
	}

	// Only compiled if the Slot type is actually a Sampled Image
	uint32_t RegisterImage(const VkImageView view,
						   const VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		requires(Slot::type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
	{
		return UpdateDescriptor(view, VK_NULL_HANDLE, layout);
	}

	// Only compiled if the Slot type is a Combined Image Sampler
	uint32_t RegisterCombined(const VkImageView view, const VkSampler sampler,
							  const VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		requires(Slot::type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
	{
		return UpdateDescriptor(view, sampler, layout);
	}

	[[nodiscard]] constexpr uint32_t Capacity() const noexcept { return Slot::count; }
	[[nodiscard]] uint32_t Size() const noexcept { return _nextSlot; }

  private:
	uint32_t UpdateDescriptor(VkImageView view, VkSampler sampler, VkImageLayout layout) {
		// 2. Proof of Correctness: Runtime bounds check against compile-time limit
		if (_nextSlot >= Slot::count) [[unlikely]] {
			std::println(stderr, "[BindlessRegistry<{}>] FATAL: Exceeded capacity of {} slots.",
						 BindingID, Slot::count);
			std::abort();
		}

		const uint32_t slot = _nextSlot++;
		const VkDescriptorImageInfo imageInfo = {
			.sampler = sampler,
			.imageView = view,
			.imageLayout = layout,
		};
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = _set,
			.dstBinding = Slot::binding,
			.dstArrayElement = slot,
			.descriptorCount = 1,
			.descriptorType = Slot::type,
			.pImageInfo = &imageInfo,
		};
		vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
		return slot;
	}

	VkDevice _device = VK_NULL_HANDLE;
	VkDescriptorSet _set = VK_NULL_HANDLE;
	uint32_t _nextSlot = 0;
};

// ============================================================================
// DescriptorLayout<Slots...> — the main TMP type
// ============================================================================

template <typename... Slots> class DescriptorLayout {
	static constexpr uint32_t kCount = sizeof...(Slots);

	// Builds the VkDescriptorSetLayoutBinding array
	static constexpr auto MakeBindings() noexcept {
		std::array<VkDescriptorSetLayoutBinding, kCount> b{};
		uint32_t i = 0;
		((b[i++] =
			  VkDescriptorSetLayoutBinding{
				  .binding = Slots::binding,
				  .descriptorType = Slots::type,
				  .descriptorCount = Slots::count, // Handles arrays
				  .stageFlags = Slots::stages,
			  }),
		 ...);
		return b;
	}

	// Builds the VkDescriptorBindingFlags array
	static constexpr auto MakeBindingFlags() noexcept {
		std::array<VkDescriptorBindingFlags, kCount> f{};
		uint32_t i = 0;
		((f[i++] = Slots::flags), ...);
		return f;
	}

	// Detect if ANY slot requires UPDATE_AFTER_BIND logic
	static constexpr bool HasUpdateAfterBind() noexcept {
		bool has = false;
		((has |= ((Slots::flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0)), ...);
		return has;
	}

	// Pool size calculation multiplies by actual array slot counts
	static constexpr auto MakePoolSizes(uint32_t setCount) noexcept {
		std::array<VkDescriptorPoolSize, kCount> ps{};
		uint32_t i = 0;
		((ps[i++] =
			  VkDescriptorPoolSize{
				  .type = Slots::type,
				  .descriptorCount = Slots::count * setCount,
			  }),
		 ...);
		return ps;
	}

  public:
	template <uint32_t B> using SlotInfo = GetSlot<B, Slots...>;
	// -------------------------------------------------------------------------
	// CreateLayout
	// -------------------------------------------------------------------------
	[[nodiscard]] static ZHLN::Vk::DescriptorSetLayout CreateLayout(VkDevice device) noexcept {
		static constexpr auto bindings = MakeBindings();
		static constexpr auto flags = MakeBindingFlags();
		static constexpr bool updateAfterBind = HasUpdateAfterBind();

		const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			.bindingCount = kCount,
			.pBindingFlags = flags.data(),
		};

		const VkDescriptorSetLayoutCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext = &flagsInfo, // Bindless Flags attached here
			.flags = updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT
									 : (VkDescriptorSetLayoutCreateFlags)0,
			.bindingCount = kCount,
			.pBindings = bindings.data(),
		};

		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
		return ZHLN::Vk::DescriptorSetLayout(device, layout); // RAII wrap
	}

	// -------------------------------------------------------------------------
	// CreatePool
	// -------------------------------------------------------------------------
	[[nodiscard]] static ZHLN::Vk::DescriptorPool CreatePool(VkDevice device,
															 uint32_t maxSets) noexcept {
		const auto poolSizes = MakePoolSizes(maxSets);
		static constexpr bool updateAfterBind = HasUpdateAfterBind();

		const VkDescriptorPoolCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
									 : (VkDescriptorPoolCreateFlags)0,
			.maxSets = maxSets,
			.poolSizeCount = kCount,
			.pPoolSizes = poolSizes.data(),
		};

		VkDescriptorPool pool = VK_NULL_HANDLE;
		vkCreateDescriptorPool(device, &info, nullptr, &pool);
		return ZHLN::Vk::DescriptorPool(device, pool); // RAII wrap
	}

	// -------------------------------------------------------------------------
	// Allocate
	// -------------------------------------------------------------------------
	[[nodiscard]] static VkDescriptorSet Allocate(VkDevice device, VkDescriptorPool pool,
												  VkDescriptorSetLayout layout) noexcept {
		const VkDescriptorSetAllocateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &layout,
		};
		VkDescriptorSet set;
		vkAllocateDescriptorSets(device, &info, &set);
		return set;
	}

	// -------------------------------------------------------------------------
	// Write
	// -------------------------------------------------------------------------
	template <typename... Args>
		requires(sizeof...(Args) == kCount)
	static void Write(VkDevice device, VkDescriptorSet set, Args&&... args) noexcept {
		const auto argTuple = std::forward_as_tuple(std::forward<Args>(args)...);

		std::array<VkDescriptorImageInfo, kCount> imageInfos{};
		std::array<VkDescriptorBufferInfo, kCount> bufferInfos{};
		std::array<VkWriteDescriptorSet, kCount> writes{};

		WriteAll(device, set, argTuple, imageInfos, bufferInfos, writes,
				 std::make_index_sequence<kCount>{});
	}

  private:
	template <typename ArgTuple, size_t... I>
	static void WriteAll(VkDevice device, VkDescriptorSet set, ArgTuple& args,
						 std::array<VkDescriptorImageInfo, kCount>& imageInfos,
						 std::array<VkDescriptorBufferInfo, kCount>& bufferInfos,
						 std::array<VkWriteDescriptorSet, kCount>& writes,
						 std::index_sequence<I...>) noexcept {

		(WriteSlot<I, std::tuple_element_t<I, std::tuple<Slots...>>>(
			 set, std::get<I>(args), imageInfos[I], bufferInfos[I], writes[I]),
		 ...);

		// Compact the writes array to ignore 'SkipWrite' entries where descriptorCount is 0
		std::array<VkWriteDescriptorSet, kCount> validWrites{};
		uint32_t validCount = 0;
		for (uint32_t i = 0; i < kCount; ++i) {
			if (writes[i].descriptorCount > 0) {
				validWrites[validCount++] = writes[i];
			}
		}

		if (validCount > 0) {
			vkUpdateDescriptorSets(device, validCount, validWrites.data(), 0, nullptr);
		}
	}

	template <size_t I, typename Slot, typename Arg>
	static void WriteSlot(VkDescriptorSet set, Arg&& arg, VkDescriptorImageInfo& imageInfo,
						  VkDescriptorBufferInfo& bufferInfo,
						  VkWriteDescriptorSet& write) noexcept {

		using T = std::remove_cvref_t<Arg>;

		// 1. If the user passes SkipWrite{}, early out and set count to 0
		if constexpr (std::is_same_v<T, SkipWrite>) {
			write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .descriptorCount = 0};
		}
		// 2. Otherwise, perform the type-checked write
		else {
			write = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = set,
				.dstBinding = Slot::binding,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = Slot::type,
			};

			if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
						  Slot::type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {

				static_assert(std::is_same_v<T, ImageWrite>, "Binding expects ImageWrite");
				imageInfo = {.imageView = arg.view, .imageLayout = arg.layout};
				write.pImageInfo = &imageInfo;

			} else if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {

				static_assert(std::is_same_v<T, ImageWrite>, "Binding expects ImageWrite");
				imageInfo = {.imageView = arg.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
				write.pImageInfo = &imageInfo;

			} else if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_SAMPLER) {

				static_assert(std::is_same_v<T, SamplerWrite>, "Binding expects SamplerWrite");
				imageInfo = {.sampler = arg.sampler};
				write.pImageInfo = &imageInfo;

			} else if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
								 Slot::type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {

				static_assert(std::is_same_v<T, BufferWrite>, "Binding expects BufferWrite");
				bufferInfo = {.buffer = arg.buffer, .offset = arg.offset, .range = arg.range};
				write.pBufferInfo = &bufferInfo;

			} else {
				static_assert(sizeof(Slot) == 0, "Unhandled descriptor type");
			}
		}
	}
};

} // namespace ZHLN::Vk