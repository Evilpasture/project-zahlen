#pragma once
#include "DescriptorLayout.hpp"
namespace ZHLN::Vk {

// ============================================================================
// BindlessRegistry Implementation
// ============================================================================

template <typename Layout, uint32_t BindingID>
void BindlessRegistry<Layout, BindingID>::Init(const VkDevice device, const VkDescriptorSet set,
											   VkImageView defaultView, VkSampler defaultSampler) {
	_device = device;
	_set = set;
	_nextSlot = 0;
	_defaultView = defaultView;
	_defaultSampler = defaultSampler;
	_freeSlots.clear();
}

template <typename Layout, uint32_t BindingID>
void BindlessRegistry<Layout, BindingID>::Free(uint32_t slot) {
	if (slot >= _nextSlot) [[unlikely]] {
		return; // Guard against out-of-bounds or invalid frees
	}

	// Reclaim the slot index
	_freeSlots.push_back(slot);

	// Overwrite the freed slot with the default fallback to avoid dangling descriptors on the GPU
	if (_defaultView != VK_NULL_HANDLE) {
		const VkDescriptorImageInfo imageInfo = {
			.sampler = _defaultSampler,
			.imageView = _defaultView,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = _set,
			.dstBinding = Slot::binding,
			.dstArrayElement = slot,
			.descriptorCount = 1,
			.descriptorType = Slot::type,
			.pImageInfo = &imageInfo,
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr,
		};
		vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
	}
}

template <typename Layout, uint32_t BindingID>
auto BindlessRegistry<Layout, BindingID>::RegisterImage(const VkImageView view,
														const VkImageLayout layout) -> uint32_t
	requires(Slot::type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
{
	return UpdateDescriptor(view, VK_NULL_HANDLE, layout);
}

template <typename Layout, uint32_t BindingID>
auto BindlessRegistry<Layout, BindingID>::RegisterCombined(const VkImageView view,
														   const VkSampler sampler,
														   const VkImageLayout layout) -> uint32_t
	requires(Slot::type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
{
	return UpdateDescriptor(view, sampler, layout);
}

template <typename Layout, uint32_t BindingID>
auto BindlessRegistry<Layout, BindingID>::UpdateDescriptor(VkImageView view, VkSampler sampler,
														   VkImageLayout layout) -> uint32_t {
	uint32_t slot = 0;

	// Reuse a slot from the free-list if available
	if (!_freeSlots.empty()) {
		slot = _freeSlots.back();
		_freeSlots.pop_back();
	} else {
		if (_nextSlot >= Slot::count) [[unlikely]] {
			ReportBindlessRegistryExceeded(BindingID, Slot::count);
			std::abort();
		}
		slot = _nextSlot++;
	}

	const VkDescriptorImageInfo imageInfo = {
		.sampler = sampler,
		.imageView = view,
		.imageLayout = layout,
	};
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = nullptr,
		.dstSet = _set,
		.dstBinding = Slot::binding,
		.dstArrayElement = slot,
		.descriptorCount = 1,
		.descriptorType = Slot::type,
		.pImageInfo = &imageInfo,
		.pBufferInfo = nullptr,
		.pTexelBufferView = nullptr,
	};
	vkUpdateDescriptorSets(_device, 1, &write, 0, nullptr);
	return slot;
}

// ============================================================================
// DescriptorUpdater Implementation
// ============================================================================

inline void DescriptorUpdater::BindUniformBuffer(uint32_t binding, VkBuffer buffer,
												 VkDeviceSize offset, VkDeviceSize range) {
	uint32_t idx = _bufferCount++;
	auto& bufInfo = _bufferInfos[idx];
	bufInfo = {.buffer = buffer, .offset = offset, .range = range};

	auto& write = _writes[_writeCount++];
	write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = {},
		.dstSet = {},
		.dstBinding = binding,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pImageInfo = {},
		.pBufferInfo = &_bufferInfos[idx],
		.pTexelBufferView = {},
	};
}

inline void DescriptorUpdater::BindStorageBuffer(uint32_t binding, VkBuffer buffer,
												 VkDeviceSize offset, VkDeviceSize range) {
	auto& bufInfo = _bufferInfos[_bufferCount++];
	bufInfo = {.buffer = buffer, .offset = offset, .range = range};

	auto& write = _writes[_writeCount++];
	write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = {},
		.dstSet = {},
		.dstBinding = binding,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pImageInfo = {},
		.pBufferInfo = &bufInfo,
		.pTexelBufferView = {},
	};
}

inline void DescriptorUpdater::BindSampledImage(uint32_t binding, VkImageView view,
												VkSampler sampler, VkImageLayout layout) {
	auto& imgInfo = _imageInfos[_imageCount++];
	imgInfo = {.sampler = sampler, .imageView = view, .imageLayout = layout};

	auto& write = _writes[_writeCount++];
	write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = {},
		.dstSet = {},
		.dstBinding = binding,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = sampler == VK_NULL_HANDLE ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
													: VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &imgInfo,
		.pBufferInfo = {},
		.pTexelBufferView = {},
	};
}

inline void DescriptorUpdater::BindSampler(uint32_t binding, VkSampler sampler) {
	auto& imgInfo = _imageInfos[_imageCount++];
	imgInfo = {
		.sampler = sampler, .imageView = VK_NULL_HANDLE, .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED};

	auto& write = _writes[_writeCount++];
	write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = {},
		.dstSet = {},
		.dstBinding = binding,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
		.pImageInfo = &imgInfo,
		.pBufferInfo = {},
		.pTexelBufferView = {},
	};
}

inline void DescriptorUpdater::BindAccelerationStructure(uint32_t binding,
														 const VkAccelerationStructureKHR* as) {
	auto& asInfo = _asInfos[_asCount++];
	asInfo = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
			  .pNext = nullptr,
			  .accelerationStructureCount = 1,
			  .pAccelerationStructures = as};
	auto& write = _writes[_writeCount++];
	write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			 .pNext = &asInfo,
			 .dstSet = VK_NULL_HANDLE,
			 .dstBinding = binding,
			 .dstArrayElement = 0,
			 .descriptorCount = 1,
			 .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			 .pImageInfo = nullptr,
			 .pBufferInfo = nullptr,
			 .pTexelBufferView = nullptr};
}

inline void DescriptorUpdater::UpdateSet(VkDevice device, VkDescriptorSet set) {
	for (uint32_t i = 0; i < _writeCount; ++i) {
		_writes[i].dstSet = set;
	}
	vkUpdateDescriptorSets(device, _writeCount, _writes.data(), 0, nullptr);
	Clear();
}

// ============================================================================
// DescriptorLayout<Slots...> Implementation
// ============================================================================

template <typename... Slots> constexpr auto DescriptorLayout<Slots...>::MakeBindings() noexcept {
	std::array<VkDescriptorSetLayoutBinding, kCount> b{};
	uint32_t i = 0;
	((b[i++] =
		  VkDescriptorSetLayoutBinding{
			  .binding = Slots::binding,
			  .descriptorType = Slots::type,
			  .descriptorCount = Slots::count,
			  .stageFlags = Slots::stages,
			  .pImmutableSamplers = nullptr,
		  }),
	 ...);
	return b;
}

template <typename... Slots>
constexpr auto DescriptorLayout<Slots...>::MakeBindingFlags() noexcept {
	std::array<VkDescriptorBindingFlags, kCount> f{};
	uint32_t i = 0;
	((f[i++] = Slots::flags), ...);
	return f;
}

template <typename... Slots>
constexpr auto DescriptorLayout<Slots...>::HasUpdateAfterBind() noexcept -> bool {
	bool has = false;
	((has |= ((Slots::flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0)), ...);
	return has;
}

template <typename... Slots>
constexpr auto DescriptorLayout<Slots...>::MakePoolSizes(uint32_t setCount) noexcept {
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

template <typename... Slots>
auto DescriptorLayout<Slots...>::CreateLayout(VkDevice device) noexcept
	-> ZHLN::Vk::DescriptorSetLayout {
	constexpr auto bindings = MakeBindings();
	constexpr auto flags = MakeBindingFlags();
	constexpr bool updateAfterBind = HasUpdateAfterBind();

	const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		.pNext = nullptr,
		.bindingCount = kCount,
		.pBindingFlags = flags.data(),
	};

	const VkDescriptorSetLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = &flagsInfo,
		.flags = updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT
								 : static_cast<VkDescriptorSetLayoutCreateFlagBits>(0),
		.bindingCount = kCount,
		.pBindings = bindings.data(),
	};

	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
	return {device, layout};
}

template <typename... Slots>
auto DescriptorLayout<Slots...>::CreatePool(VkDevice device, uint32_t maxSets) noexcept
	-> ZHLN::Vk::DescriptorPool {
	const auto poolSizes = MakePoolSizes(maxSets);
	static constexpr bool updateAfterBind = HasUpdateAfterBind();

	const VkDescriptorPoolCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = updateAfterBind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
								 : static_cast<VkDescriptorPoolCreateFlagBits>(0),
		.maxSets = maxSets,
		.poolSizeCount = kCount,
		.pPoolSizes = poolSizes.data(),
	};

	VkDescriptorPool pool = VK_NULL_HANDLE;
	vkCreateDescriptorPool(device, &info, nullptr, &pool);
	return {device, pool};
}

template <typename... Slots>
auto DescriptorLayout<Slots...>::Allocate(VkDevice device, VkDescriptorPool pool,
										  VkDescriptorSetLayout layout) noexcept
	-> VkDescriptorSet {
	const VkDescriptorSetAllocateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.pNext = nullptr,
		.descriptorPool = pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &layout,
	};
	VkDescriptorSet set = VK_NULL_HANDLE;
	vkAllocateDescriptorSets(device, &info, &set);
	return set;
}

template <typename... Slots>
template <typename... Args>
	requires(sizeof...(Args) == sizeof...(Slots))
void DescriptorLayout<Slots...>::Write(VkDevice device, VkDescriptorSet set,
									   Args&&... args) noexcept {
	const auto argTuple = std::forward_as_tuple(std::forward<Args>(args)...);

	std::array<VkDescriptorImageInfo, kCount> imageInfos{};
	std::array<VkDescriptorBufferInfo, kCount> bufferInfos{};
	std::array<VkWriteDescriptorSetAccelerationStructureKHR, kCount> asInfos{};
	std::array<VkWriteDescriptorSet, kCount> writes{};

	WriteAll(device, set, argTuple, imageInfos, bufferInfos, asInfos, writes,
			 std::make_index_sequence<kCount>{});
}

template <typename... Slots>
template <typename ArgTuple, size_t... I>
void DescriptorLayout<Slots...>::WriteAll(
	VkDevice device, VkDescriptorSet set, ArgTuple& args,
	std::array<VkDescriptorImageInfo, kCount>& imageInfos,
	std::array<VkDescriptorBufferInfo, kCount>& bufferInfos,
	std::array<VkWriteDescriptorSetAccelerationStructureKHR, kCount>& asInfos,
	std::array<VkWriteDescriptorSet, kCount>& writes,
	std::index_sequence<I...> /*unused*/) noexcept {
	(WriteSlot<I, std::tuple_element_t<I, std::tuple<Slots...>>>(
		 set, std::get<I>(args), imageInfos[I], bufferInfos[I], asInfos[I], writes[I]),
	 ...);

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

template <typename... Slots>
template <size_t I, typename Slot, typename Arg>
void DescriptorLayout<Slots...>::WriteSlot(VkDescriptorSet set, const Arg& arg,
										   VkDescriptorImageInfo& imageInfo,
										   VkDescriptorBufferInfo& bufferInfo,
										   VkWriteDescriptorSetAccelerationStructureKHR& asInfo,
										   VkWriteDescriptorSet& write) noexcept {
	using T = std::remove_cvref_t<Arg>;

	if constexpr (std::is_same_v<T, SkipWrite>) {
		static_assert(((Slot::flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0) ||
						  ((Slot::flags & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT) != 0),
					  "SkipWrite can only be used for slots with the UPDATE_AFTER_BIND or "
					  "PARTIALLY_BOUND flags.");

		write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				 .pNext = nullptr,
				 .dstSet = VK_NULL_HANDLE,
				 .dstBinding = 0,
				 .dstArrayElement = 0,
				 .descriptorCount = 0,
				 .descriptorType = Slot::type,
				 .pImageInfo = nullptr,
				 .pBufferInfo = nullptr,
				 .pTexelBufferView = nullptr};
		return;
	} else {
		write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = set,
			.dstBinding = Slot::binding,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = Slot::type,
			.pImageInfo = nullptr,
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr,
		};

		if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
					  Slot::type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
			VkImageView view = VK_NULL_HANDLE;
			VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			if constexpr (std::is_same_v<T, ImageWrite>) {
				view = arg.view;
				layout = arg.layout;
			} else if constexpr (IsTypedImage<T>::value) {
				view = arg.view;
				layout = T::layout;
			} else if constexpr (requires { arg.view.Get(); }) { // RenderTarget<F>
				view = arg.view.Get();
				layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			} else if constexpr (requires {
									 { arg.Get() } -> std::convertible_to<VkImageView>;
								 }) { // ImageView / DeviceHandle
				view = arg.Get();
				layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			} else if constexpr (std::is_same_v<T, VkImageView>) {
				view = arg;
				layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			} else {
				static_assert(sizeof(T) == 0, "Unsupported argument type for SampledImage slot.");
			}

			imageInfo = {.sampler = VK_NULL_HANDLE, .imageView = view, .imageLayout = layout};
			write.pImageInfo = &imageInfo;

		} else if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
			VkImageView view = VK_NULL_HANDLE;
			if constexpr (std::is_same_v<T, ImageWrite>) {
				view = arg.view;
			} else if constexpr (requires { arg.view.Get(); }) {
				view = arg.view.Get();
			} else if constexpr (requires {
									 { arg.Get() } -> std::convertible_to<VkImageView>;
								 }) {
				view = arg.Get();
			} else if constexpr (std::is_same_v<T, VkImageView>) {
				view = arg;
			} else {
				static_assert(sizeof(T) == 0, "Unsupported argument type for StorageImage slot.");
			}

			imageInfo = {.sampler = VK_NULL_HANDLE,
						 .imageView = view,
						 .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
			write.pImageInfo = &imageInfo;

		} else if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_SAMPLER) {
			VkSampler sampler = VK_NULL_HANDLE;
			if constexpr (std::is_same_v<T, SamplerWrite>) {
				sampler = arg.sampler;
			} else if constexpr (requires {
									 { arg.Get() } -> std::convertible_to<VkSampler>;
								 }) { // Sampler / DeviceHandle
				sampler = arg.Get();
			} else if constexpr (std::is_same_v<T, VkSampler>) {
				sampler = arg;
			} else {
				static_assert(sizeof(T) == 0, "Unsupported argument type for Sampler slot.");
			}

			imageInfo = {.sampler = sampler,
						 .imageView = VK_NULL_HANDLE,
						 .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED};
			write.pImageInfo = &imageInfo;

		} else if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
							 Slot::type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceSize offset = 0;
			VkDeviceSize range = VK_WHOLE_SIZE;

			if constexpr (std::is_same_v<T, BufferWrite>) {
				buffer = arg.buffer;
				offset = arg.offset;
				range = arg.range;
			} else if constexpr (requires { arg.Handle(); }) { // Vk::Buffer
				buffer = arg.Handle();
			} else if constexpr (std::is_same_v<T, VkBuffer>) {
				buffer = arg;
			} else {
				static_assert(sizeof(T) == 0, "Unsupported argument type for Buffer slot.");
			}

			bufferInfo = {.buffer = buffer, .offset = offset, .range = range};
			write.pBufferInfo = &bufferInfo;

		} else if constexpr (Slot::type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
			if constexpr (std::is_pointer_v<T> &&
						  (std::is_same_v<std::remove_pointer_t<T>,
										  const VkAccelerationStructureKHR> ||
						   std::is_same_v<std::remove_pointer_t<T>, VkAccelerationStructureKHR>)) {
				if (arg == nullptr || *arg == VK_NULL_HANDLE) {
					write.descriptorCount = 0;
				} else {
					asInfo = {.sType =
								  VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
							  .pNext = nullptr,
							  .accelerationStructureCount = 1,
							  .pAccelerationStructures = arg};
					write.pNext = &asInfo;
				}
			} else {
				static_assert(sizeof(T) == 0,
							  "Unsupported argument type for AccelerationStructure slot.");
			}
		}
	}
}

template <typename LayoutT>
inline void AllocateDoubleBufferedSet(VkDevice device, DescriptorSetLayout& outLayout,
									  DescriptorPool& outPool,
									  ZHLN::DoubleBuffered<VkDescriptorSet>& outSets) noexcept {
	outLayout = LayoutT::CreateLayout(device);
	outPool = LayoutT::CreatePool(device, 2);
	outSets[0] = LayoutT::Allocate(device, outPool.Get(), outLayout.Get());
	outSets[1] = LayoutT::Allocate(device, outPool.Get(), outLayout.Get());
}

template <typename LayoutT>
inline void AllocateSingleBufferedSet(VkDevice device, DescriptorSetLayout& outLayout,
									  DescriptorPool& outPool, VkDescriptorSet& outSet) noexcept {
	outLayout = LayoutT::CreateLayout(device);
	outPool = LayoutT::CreatePool(device, 1);
	outSet = LayoutT::Allocate(device, outPool.Get(), outLayout.Get());
}

} // namespace ZHLN::Vk
