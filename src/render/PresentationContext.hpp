#pragma once
#include "Allocator.hpp"
#include "RenderCore.hpp"
#include "RenderTarget.hpp"

namespace ZHLN::Vk {

/**
 * @brief Infrastructure orchestrator. Manages Swapchain, Present Semaphores,
 * and the Window-bound Depth Buffer.
 */
class PresentationContext {
  public:
	Swapchain swapchain;
	SemaphorePool presentSemaphores;

	// The main depth buffer is tied to the window resolution
	RenderTarget<VK_FORMAT_D32_SFLOAT> depthTarget;

	PresentationContext() = default;

	// Move-only
	PresentationContext(const PresentationContext&) = delete;
	PresentationContext& operator=(const PresentationContext&) = delete;
	PresentationContext(PresentationContext&&) noexcept = default;
	PresentationContext& operator=(PresentationContext&&) noexcept = default;

	[[nodiscard]] bool Init(const Context& ctx, Allocator& alloc, VkSurfaceKHR surface,
							uint32_t width, uint32_t height) {
		_ctx = &ctx;
		_alloc = &alloc;
		_surface = surface;
		return Rebuild(width, height);
	}

	[[nodiscard]] bool Rebuild(uint32_t width, uint32_t height) {
		if (!_ctx || !_alloc)
			return false;

		vkDeviceWaitIdle(_ctx->Device());

		ZHLN_Device rawDev = {_ctx->Device(), _ctx->GraphicsQueue(), _ctx->PresentQueue()};
		ZHLN_PhysicalDeviceInfo rawPhys = _ctx->PhysicalInfo();
		ZHLN_SwapchainDesc s_desc = {.device = &rawDev,
									 .physical = &rawPhys,
									 .surface = _surface,
									 .width = width,
									 .height = height,
									 .vsync = true,
									 .old_swapchain = swapchain.Get().handle};

		if (!swapchain.Rebuild(s_desc))
			return false;

		presentSemaphores.Rebuild(_ctx->Device(), swapchain.Get().image_count);

		// Automatically recreate the depth buffer to match the new swapchain extent
		depthTarget = RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
			*_alloc, *_ctx, swapchain.Get().extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

		return depthTarget.Valid();
	}

  private:
	const Context* _ctx = nullptr;
	Allocator* _alloc = nullptr;
	VkSurfaceKHR _surface = VK_NULL_HANDLE;
};

} // namespace ZHLN::Vk