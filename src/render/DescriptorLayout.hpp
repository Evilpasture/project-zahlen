#pragma once
#include "RenderCore.hpp"

#include <array>
#include <tuple>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

// ============================================================================
// Binding descriptors — compile-time slot definitions
// ============================================================================

template <uint32_t Binding, VkDescriptorType Type, VkShaderStageFlags Stages> struct BindingSlot {
	static constexpr uint32_t binding = Binding;
	static constexpr VkDescriptorType type = Type;
	static constexpr VkShaderStageFlags stages = Stages;
};

// Convenience aliases matching your existing usage patterns
template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using SampledImageSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, S>;

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using SamplerSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_SAMPLER, S>;

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_VERTEX_BIT>
using UniformSlot = BindingSlot<B, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, S>;

// ============================================================================
// DescriptorWrite — typed wrapper so Write() knows what VkDescriptorImageInfo
// to fill for each slot type
// ============================================================================

struct ImageWrite {
	VkImageView view;
	VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};
struct SamplerWrite {
	VkSampler sampler;
};

// ============================================================================
// DescriptorLayout<Slots...> — the main TMP type
// ============================================================================

template <typename... Slots> class DescriptorLayout {
	static constexpr uint32_t kCount = sizeof...(Slots);

	// Builds the VkDescriptorSetLayoutBinding array at compile time
	static constexpr auto MakeBindings() noexcept {
		std::array<VkDescriptorSetLayoutBinding, kCount> b{};
		uint32_t i = 0;
		((b[i++] =
			  VkDescriptorSetLayoutBinding{
				  .binding = Slots::binding,
				  .descriptorType = Slots::type,
				  .descriptorCount = 1,
				  .stageFlags = Slots::stages,
			  }),
		 ...);
		return b;
	}

	// Pool size: one entry per unique descriptor type.
	// Simple approach: one entry per slot, driver merges duplicates.
	static constexpr auto MakePoolSizes(uint32_t setCount) noexcept {
		std::array<VkDescriptorPoolSize, kCount> ps{};
		uint32_t i = 0;
		((ps[i++] =
			  VkDescriptorPoolSize{
				  .type = Slots::type,
				  .descriptorCount = setCount,
			  }),
		 ...);
		return ps;
	}

  public:
	// -------------------------------------------------------------------------
	// CreateLayout — one-shot VkDescriptorSetLayout from the slot list
	// -------------------------------------------------------------------------
	[[nodiscard]] static ZHLN::Vk::DescriptorSetLayout CreateLayout(VkDevice device) noexcept {
		static constexpr auto bindings = MakeBindings();
		const VkDescriptorSetLayoutCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = kCount,
			.pBindings = bindings.data(),
		};
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
		return ZHLN::Vk::DescriptorSetLayout(device, layout); // RAII wrap
	}

	// -------------------------------------------------------------------------
	// CreatePool — pool sized for maxSets descriptor sets of this layout
	// -------------------------------------------------------------------------
	[[nodiscard]] static ZHLN::Vk::DescriptorPool CreatePool(VkDevice device,
															 uint32_t maxSets) noexcept {
		auto poolSizes = MakePoolSizes(maxSets);
		const VkDescriptorPoolCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = maxSets,
			.poolSizeCount = kCount,
			.pPoolSizes = poolSizes.data(),
		};
		VkDescriptorPool pool = VK_NULL_HANDLE;
		vkCreateDescriptorPool(device, &info, nullptr, &pool);
		return ZHLN::Vk::DescriptorPool(device, pool); // RAII wrap
	}

	// -------------------------------------------------------------------------
	// Allocate — allocates one set from an existing pool
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
	// Write — the core: one argument per slot, type-checked at compile time.
	//
	// Slot type → expected argument type:
	//   SAMPLED_IMAGE  → ImageWrite   { view, layout }
	//   SAMPLER        → SamplerWrite { sampler }
	//
	// The fold expression pairs each Slot with its argument positionally,
	// so swapping arguments is a compile error, not a silent Vulkan corruption.
	// -------------------------------------------------------------------------
	template <typename... Args>
		requires(sizeof...(Args) == kCount)
	static void Write(VkDevice device, VkDescriptorSet set, Args&&... args) noexcept {
		// 1. Pack args into a tuple
		auto argTuple = std::forward_as_tuple(std::forward<Args>(args)...);

		// 2. Prepare arrays for Vulkan structures
		std::array<VkDescriptorImageInfo, kCount> imageInfos{};
		std::array<VkWriteDescriptorSet, kCount> writes{};

		// 3. Call the helper that uses index_sequence (compile-time indices)
		WriteAll(device, set, argTuple, imageInfos, writes, std::make_index_sequence<kCount>{});
	}

  private:
	// This helper correctly uses 'I' as a template parameter for std::get
	template <typename ArgTuple, size_t... I>
	static void WriteAll(VkDevice device, VkDescriptorSet set, ArgTuple& args,
						 std::array<VkDescriptorImageInfo, kCount>& imageInfos,
						 std::array<VkWriteDescriptorSet, kCount>& writes,
						 std::index_sequence<I...>) noexcept {

		// Expand the fold expression using the compile-time index I
		(WriteSlot<I, std::tuple_element_t<I, std::tuple<Slots...>>>(
			 set,
			 std::get<I>(args), // This now works because I is a constant
			 imageInfos[I], writes[I]),
		 ...);

		vkUpdateDescriptorSets(device, kCount, writes.data(), 0, nullptr);
	}

	template <size_t I, typename Slot, typename Arg>
	static void WriteSlot(VkDescriptorSet set, Arg&& arg, VkDescriptorImageInfo& imageInfo,
						  VkWriteDescriptorSet& write) noexcept {
		write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = set,
			.dstBinding = Slot::binding,
			.descriptorCount = 1,
			.descriptorType = Slot::type,
		};

		if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
			static_assert(std::is_same_v<std::remove_cvref_t<Arg>, ImageWrite>,
						  "Binding expects ImageWrite { view, layout }");
			imageInfo = {.imageView = arg.view, .imageLayout = arg.layout};
			write.pImageInfo = &imageInfo;

		} else if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_SAMPLER) {
			static_assert(std::is_same_v<std::remove_cvref_t<Arg>, SamplerWrite>,
						  "Binding expects SamplerWrite { sampler }");
			imageInfo = {.sampler = arg.sampler};
			write.pImageInfo = &imageInfo;

		} else {
			static_assert(sizeof(Slot) == 0,
						  "Unhandled descriptor type — add a specialization in WriteSlot");
		}
	}
};

} // namespace ZHLN::Vk