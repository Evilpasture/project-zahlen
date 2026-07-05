#include "Commands.hpp"

namespace ZHLN::Vk {

// ============================================================================
// Immediate Command PIMPL Implementation (Refactored)
// ============================================================================

template <QueueType QType> struct ImmediateCommand<QType>::Impl {
	VkDevice device = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	StagingRingBuffer* ringBuffer = nullptr;

	Impl(VkDevice dev, StagingRingBuffer& rb) noexcept : device(dev), ringBuffer(&rb) {
		uint32_t queueFamily = rb.GetQueueFamily();

		VkCommandPoolCreateInfo poolInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.queueFamilyIndex = queueFamily,
		};
		vkCreateCommandPool(device, &poolInfo, nullptr, &pool);

		VkCommandBufferAllocateInfo allocInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.pNext = nullptr,
			.commandPool = pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		vkAllocateCommandBuffers(device, &allocInfo, &cmd);

		VkCommandBufferBeginInfo beginInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.pNext = nullptr,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			.pInheritanceInfo = nullptr,
		};
		vkBeginCommandBuffer(cmd, &beginInfo);
	}

	~Impl() noexcept {
		if (cmd != VK_NULL_HANDLE) {
			vkEndCommandBuffer(cmd);
			uint64_t submitVal = ringBuffer->Submit(cmd);
			ringBuffer->RetirePool(pool, submitVal);
		}
	}
};

template <QueueType QType>
ImmediateCommand<QType>::ImmediateCommand(VkDevice dev, StagingRingBuffer& ringBuffer) noexcept
	: _impl(std::make_unique<Impl>(dev, ringBuffer)) {}

template <QueueType QType> ImmediateCommand<QType>::~ImmediateCommand() noexcept = default;

template <QueueType QType>
auto ImmediateCommand<QType>::AllocateStaging(VkDeviceSize size, VkDeviceSize alignment) noexcept
	-> StagingRingBuffer::Allocation {
	return _impl->ringBuffer->Allocate(size, alignment);
}

template <QueueType QType> ImmediateCommand<QType>::operator CommandBuffer<QType>() const noexcept {
	return CommandBuffer<QType>{_impl->cmd};
}

template <QueueType QType> ImmediateCommand<QType>::operator VkCommandBuffer() const noexcept {
	return _impl->cmd; // Directly return the raw Vulkan handle
}

template class ImmediateCommand<QueueType::Graphics>;
template class ImmediateCommand<QueueType::Compute>;
template class ImmediateCommand<QueueType::Transfer>;

} // namespace ZHLN::Vk
