// src/render/legacy/DynamicPass.hpp
#pragma once

#include "PassCache.hpp"

namespace ZHLN::Vk11 {

static constexpr size_t kMaxColorAttachments = 8;

struct ColorAttachmentData {
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ZHLN::Color4 clearColor{};
};

struct DepthAttachmentData {
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	float clearVal = 1.0f;
};

template <size_t ColorCount = 0, bool HasDepth = false> class DynamicPass {
  public:
	constexpr explicit DynamicPass(VkExtent2D extent) noexcept : _extent(extent) {}

	// Mirroring your exact inter-state move constructor
	template <size_t InsideCount, bool InsideDepth>
	constexpr explicit DynamicPass(DynamicPass<InsideCount, InsideDepth>&& other) noexcept
		: _extent(other._extent), _flags(other._flags), _colors(std::move(other)._colors),
		  _depth(other._depth), _viewMask(other._viewMask) {}

	template <VkImageLayout Layout>
	constexpr auto AddColor(
		const Vk::TypedImage<Layout>& img, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		const ZHLN::Color4& clearColor = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f}) && noexcept
		-> DynamicPass<ColorCount + 1, HasDepth> {

		static_assert(ColorCount < kMaxColorAttachments,
					  "ZHLN Error: Legacy DynamicPass exceeded color attachment limits.");

		_colors[ColorCount] = {.view = img.view,
							   .format = img.format,
							   .loadOp = loadOp,
							   .storeOp = storeOp,
							   .clearColor = clearColor};

		return DynamicPass<ColorCount + 1, HasDepth>(std::move(*this));
	}

	template <VkImageLayout Layout>
	constexpr auto AddDepth(const Vk::TypedImage<Layout>& img,
							VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
							VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE,
							float clearVal = 1.0f) && noexcept -> DynamicPass<ColorCount, true> {
		static_assert(!HasDepth,
					  "ZHLN Execution Error: Depth target already bound to legacy DynamicPass.");

		_depth = {.view = img.view,
				  .format = img.format,
				  .loadOp = loadOp,
				  .storeOp = storeOp,
				  .clearVal = clearVal};

		return DynamicPass<ColorCount, true>(std::move(*this));
	}

	constexpr auto Flags(VkRenderingFlags flags) && noexcept
		-> DynamicPass<ColorCount, HasDepth>&& {
		_flags = flags;
		return std::move(*this);
	}

	constexpr auto ViewMask(uint32_t mask) && noexcept -> DynamicPass<ColorCount, HasDepth>&& {
		_viewMask = mask;
		return std::move(*this);
	}

	template <typename Func> void Execute(VkCommandBuffer cmd, Func&& func) const {
		RenderPassKey rpKey{};
		FramebufferKey fbKey{};

		rpKey.colorFormats.reserve(ColorCount);
		rpKey.colorLoadOps.reserve(ColorCount);
		rpKey.colorStoreOps.reserve(ColorCount);
		fbKey.attachments.reserve(ColorCount + (HasDepth ? 1 : 0));

		std::vector<VkClearValue> clearValues;
		clearValues.reserve(ColorCount + (HasDepth ? 1 : 0));

		for (size_t i = 0; i < ColorCount; ++i) {
			rpKey.colorFormats.push_back(_colors[i].format);
			rpKey.colorLoadOps.push_back(_colors[i].loadOp);
			rpKey.colorStoreOps.push_back(_colors[i].storeOp);
			fbKey.attachments.push_back(_colors[i].view);

			VkClearValue cv{};
			cv.color = {{_colors[i].clearColor.r, _colors[i].clearColor.g, _colors[i].clearColor.b,
						 _colors[i].clearColor.a}};
			clearValues.push_back(cv);
		}

		if constexpr (HasDepth) {
			rpKey.depthFormat = _depth.format;
			rpKey.depthLoadOp = _depth.loadOp;
			rpKey.depthStoreOp = _depth.storeOp;
			fbKey.attachments.push_back(_depth.view);

			VkClearValue cv{};
			cv.depthStencil = {.depth = _depth.clearVal, .stencil = 0};
			clearValues.push_back(cv);
		}

		VkRenderPass renderPass = PassCache::GetOrCreateRenderPass(rpKey);

		fbKey.renderPass = renderPass;
		fbKey.width = _extent.width;
		fbKey.height = _extent.height;
		VkFramebuffer framebuffer = PassCache::GetOrCreateFramebuffer(fbKey);

		VkRenderPassBeginInfo beginInfo{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.pNext = nullptr,
			.renderPass = renderPass,
			.framebuffer = framebuffer,
			.renderArea = {.offset = {.x = 0, .y = 0}, .extent = _extent},
			.clearValueCount = static_cast<uint32_t>(clearValues.size()),
			.pClearValues = clearValues.data()};

		vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport{.x = 0.0f,
							.y = 0.0f,
							.width = static_cast<float>(_extent.width),
							.height = static_cast<float>(_extent.height),
							.minDepth = 0.0f,
							.maxDepth = 1.0f};
		VkRect2D scissor{.offset = {.x = 0, .y = 0}, .extent = _extent};
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		std::forward<Func>(func)();

		vkCmdEndRenderPass(cmd);
	}

  private:
	template <size_t C, bool D> friend class DynamicPass;

	VkExtent2D _extent{};
	VkRenderingFlags _flags = 0;
	std::array<ColorAttachmentData, kMaxColorAttachments> _colors{};
	DepthAttachmentData _depth{};
	uint32_t _viewMask = 0;
};

// CTAD helper
DynamicPass(VkExtent2D) -> DynamicPass<0, false>;

} // namespace ZHLN::Vk11
