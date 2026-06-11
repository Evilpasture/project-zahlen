// src/render/StagingContext.hpp

#pragma once

#include "RenderCore.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

namespace ZHLN::Vk {

class Allocator;
class Buffer;

class StagingContext {
  public:
	StagingContext(Allocator& allocator, const Context& ctx);
	~StagingContext();

	// Disable copying to enforce strict ownership
	StagingContext(const StagingContext&) = delete;
	StagingContext& operator=(const StagingContext&) = delete;

	StagingContext(StagingContext&& other) noexcept;
	StagingContext& operator=(StagingContext&&) noexcept = delete;

	void Begin();

	void UploadImage2D(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels,
					   const void* data, size_t bytes);

	void UploadImage2DBuffer(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels,
							 VkBuffer stagingBuf, VkDeviceSize offset);

	void UploadPrefilteredCubeMap(VkImage dstImage, VkBuffer stagingBuf, uint32_t baseSize,
								  uint32_t mipLevels);

	void AddBuffer(Buffer&& buf);

	void ExecuteAsync();

	void Wait() noexcept;

  private:
	Allocator& _allocator;
	const Context& _ctx;
	CommandPool _cmdPool;
	VkCommandBuffer _cmd = VK_NULL_HANDLE;
	std::vector<Buffer> _stagingBuffers;
	VkFence _fence = VK_NULL_HANDLE; // Owned internally
};

} // namespace ZHLN::Vk
