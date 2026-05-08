#pragma once
#include "RenderCore.hpp"

#include <string_view>
#include <vector>

namespace ZHLN::Vk {

// Tracks the persistent state of an image across passes
struct GraphImage {
	VkImage handle;
	VkImageView view;
	VkExtent2D extent;
	VkImageAspectFlags aspect;

	// Internal state tracking
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkAccessFlags2 access = VK_ACCESS_2_NONE;
	VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;

	static GraphImage Create(VkImage handle, VkImageView view, VkExtent2D extent,
							 VkImageAspectFlags aspect) {
		GraphImage img;
		img.handle = handle;
		img.view = view;
		img.extent = extent;
		img.aspect = aspect;

		img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
		img.access = VK_ACCESS_2_NONE;
		// FIX: Match the Wait Semaphore stage so the layout transition waits for the image!
		img.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		return img;
	}
};

class RenderGraph {
	struct Pass {
		std::string_view name;
		std::vector<std::pair<GraphImage*, VkImageLayout>> transitions;
		PassRecordFn record = nullptr;
		const void* userData = nullptr;

		// Pass-specific rendering info
		VkImageView colorTarget = VK_NULL_HANDLE;
		VkImageView depthTarget = VK_NULL_HANDLE;
		VkExtent2D extent = {0, 0};
		float clearColor[4] = {0, 0, 0, 1};
	};

	std::vector<Pass> _passes;

  public:
	struct PassBuilder {
		RenderGraph& graph;
		Pass& pass;

		// Transitions image to SHADER_READ_ONLY_OPTIMAL
		PassBuilder& Read(GraphImage& img) {
			pass.transitions.push_back({&img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
			return *this;
		}

		// Transitions image to COLOR_ATTACHMENT_OPTIMAL and sets as render target
		PassBuilder& WriteColor(GraphImage& img, bool clear = true) {
			pass.transitions.push_back({&img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
			pass.colorTarget = img.view;
			pass.extent = img.extent;
			return *this;
		}

		// Transitions image to DEPTH_ATTACHMENT_OPTIMAL and sets as depth target
		PassBuilder& WriteDepth(GraphImage& img, bool clear = true) {
			pass.transitions.push_back({&img, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL});
			pass.depthTarget = img.view;
			pass.extent = img.extent;
			return *this;
		}

		// Final handoff to the OS
		PassBuilder& Present(GraphImage& img) {
			pass.transitions.push_back({&img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR});
			return *this;
		}

		void Record(PassRecordFn fn, const void* data) {
			pass.record = fn;
			pass.userData = data;
		}
	};

	PassBuilder AddPass(std::string_view name) {
		_passes.push_back({.name = name});
		return {*this, _passes.back()};
	}

	void Execute(VkCommandBuffer cmd) {
		for (auto& pass : _passes) {
			// 1. Automatic Barrier Generation
			std::vector<VkImageMemoryBarrier2> barriers;
			for (auto& [img, nextLayout] : pass.transitions) {
				// Get flags from ZHLN LayoutTraits based on the requested layout
				VkAccessFlags2 nextAccess;
				VkPipelineStageFlags2 nextStage;

				// Simple internal switch to map layout to ZHLN traits
				if (nextLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
					nextAccess = LayoutTraits<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>::access;
					nextStage = LayoutTraits<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>::stage;
				} else if (nextLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
					nextAccess = LayoutTraits<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>::access;
					nextStage = LayoutTraits<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>::stage;
				} else if (nextLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
					nextAccess = LayoutTraits<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>::access;
					nextStage = LayoutTraits<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>::stage;
				} else {
					nextAccess = VK_ACCESS_2_NONE;
					nextStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
				}

				// Only add barrier if state actually changes
				if (img->layout != nextLayout) {
					barriers.push_back(
						{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
						 .srcStageMask = img->stage,
						 .srcAccessMask = img->access,
						 .dstStageMask = nextStage,
						 .dstAccessMask = nextAccess,
						 .oldLayout = img->layout,
						 .newLayout = nextLayout,
						 .image = img->handle,
						 .subresourceRange = {img->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1}});

					// Update tracker
					img->layout = nextLayout;
					img->access = nextAccess;
					img->stage = nextStage;
				}
			}

			if (!barriers.empty()) {
				VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
										.imageMemoryBarrierCount = (uint32_t)barriers.size(),
										.pImageMemoryBarriers = barriers.data()};
				vkCmdPipelineBarrier2(cmd, &dep);
			}

			// 2. Pass Execution
			if (pass.record) {
				// If the pass has render targets, wrap in BeginRendering
				if (pass.colorTarget != VK_NULL_HANDLE || pass.depthTarget != VK_NULL_HANDLE) {
					ZHLN_RenderPassDesc rp = {.target_view = pass.colorTarget,
											  .depth_view = pass.depthTarget,
											  .extent = pass.extent,
											  .clear_color = {0.05f, 0.05f, 0.07f, 1.0f},
											  .clear_depth = 1.0f};
					ZHLN_BeginRendering(cmd, &rp);
					pass.record(cmd, pass.userData);
					ZHLN_EndRendering(cmd);
				} else {
					pass.record(cmd, pass.userData);
				}
			}
		}
		_passes.clear(); // Reset for next frame
	}
};

/**
 * @brief Base concept for a RenderNode.
 * A Node encapsulates a specific technique (PBR, Shadows, FXAA).
 */
template <typename T>
concept is_render_node = requires(T node, RenderGraph& g) {
	{ T::Execute(g, node.config) };
};

} // namespace ZHLN::Vk