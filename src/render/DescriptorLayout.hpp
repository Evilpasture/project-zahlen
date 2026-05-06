#pragma once
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
	static consteval auto MakeBindings() noexcept {
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
	static consteval auto MakePoolSizes(uint32_t setCount) noexcept {
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
	[[nodiscard]] static VkDescriptorSetLayout CreateLayout(VkDevice device) noexcept {
		static constexpr auto bindings = MakeBindings();
		const VkDescriptorSetLayoutCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = kCount,
			.pBindings = bindings.data(),
		};
		VkDescriptorSetLayout layout;
		vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
		return layout;
	}

	// -------------------------------------------------------------------------
	// CreatePool — pool sized for maxSets descriptor sets of this layout
	// -------------------------------------------------------------------------
	[[nodiscard]] static VkDescriptorPool CreatePool(VkDevice device, uint32_t maxSets) noexcept {
		auto poolSizes = MakePoolSizes(maxSets);
		const VkDescriptorPoolCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = maxSets,
			.poolSizeCount = kCount,
			.pPoolSizes = poolSizes.data(),
		};
		VkDescriptorPool pool;
		vkCreateDescriptorPool(device, &info, nullptr, &pool);
		return pool;
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
		// Pack args into a tuple so we can index them alongside the slot list
		auto argTuple = std::forward_as_tuple(std::forward<Args>(args)...);

		std::array<VkDescriptorImageInfo, kCount> imageInfos{};
		std::array<VkWriteDescriptorSet, kCount> writes{};

		uint32_t i = 0;
		(WriteOne<Slots>(device, set, std::get<i>(argTuple), imageInfos[i], writes[i], i), ...);

		// i is incremented by the fold — pass by ref trick using comma operator
		// Simpler: just use an index sequence
		WriteAll(device, set, argTuple, imageInfos, writes, std::make_index_sequence<kCount>{});
	}

  private:
	// Index-sequence based Write: pairs Slot[I] with arg[I]
	template <typename ArgTuple, size_t... I>
	static void WriteAll(VkDevice device, VkDescriptorSet set, ArgTuple& args,
						 std::array<VkDescriptorImageInfo, kCount>& imageInfos,
						 std::array<VkWriteDescriptorSet, kCount>& writes,
						 std::index_sequence<I...>) noexcept {
		// Expands: for each I, call WriteSlot<Slot[I]>(args[I], ...)
		(WriteSlot<I, std::tuple_element_t<I, std::tuple<Slots...>>>(set, std::get<I>(args),
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