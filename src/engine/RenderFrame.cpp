// File: src/engine/Render_Frame.cpp
#include "RenderInternal.hpp"
#include "Zahlen/GUI.hpp"
#include "Zahlen/Profiler.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "detail/RadixSort.hpp"
#include "detail/Ranges.hpp"
#include "engine/RenderState.hpp"
#include "imgui.h"

#include <threading/TaskSystem.hpp>

namespace ZHLN {

void RenderContext::BeginFrame() {
	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, &_impl->pools[_impl->frame_index]);

	for (auto& worker : _impl->workerCmds) {
		worker.cmdCount[_impl->frame_index].store(0, std::memory_order_relaxed);
		worker.pools[_impl->frame_index].Reset();
	}

	if (_impl->resized) {
		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(static_cast<GLFWwindow*>(_impl->window.GetNativeHandle()), &width,
							   &height);
		if (width == 0 || height == 0) {
			return;
		}

		if (!_impl->presentation.Rebuild((uint32_t)width, (uint32_t)height)) {
			return;
		}

		VkExtent2D ext = _impl->presentation.swapchain.Get().extent;

		_impl->sceneColor = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->velocityBuffer = Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->accumBuffers[0] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->accumBuffers[1] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

		_impl->needsInitialClear = true;
		_impl->depth_ready = false;
		_impl->resized = false;
	}

	ZHLN_AcquireDesc acq = {.swapchain = _impl->presentation.swapchain.Get().handle,
							.image_available = s.image_available,
							.timeout_ns = UINT64_MAX};
	if (ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index) ==
		ZHLN_FrameResult_OutOfDate) {
		_impl->resized = true;
		_impl->current_cmd = VK_NULL_HANDLE;
		return;
	}

	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);

	if (_impl->needsInitialClear) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->sceneColor.image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->velocityBuffer.image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers[0].image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers[1].image.Handle());

		VkClearValue clearColor = {.color = {.float32 = {0, 0, 0, 1}}};
		std::array<VkRenderingAttachmentInfo, 4> attachments{};
		const std::array<VkImageView, 4> views = {
			_impl->sceneColor.view.Get(), _impl->velocityBuffer.view.Get(),
			_impl->accumBuffers[0].view.Get(), _impl->accumBuffers[1].view.Get()};

		// Clean, highly readable, structured zip loop:
		for (auto&& [attachment, view] : ZHLN::Ranges::Zip(attachments, views)) {
			attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
						  .pNext = nullptr,
						  .imageView = view,
						  .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
						  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
						  .clearValue = clearColor};
		}

		VkRenderingInfo renderInfo = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = 0,
			.renderArea = {.offset = {.x = 0, .y = 0},
						   .extent = _impl->presentation.swapchain.Get().extent},
			.layerCount = 1,
			.colorAttachmentCount = 4,
			.pColorAttachments = attachments.data()};
		vkCmdBeginRendering(_impl->current_cmd, &renderInfo);
		vkCmdEndRendering(_impl->current_cmd);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->sceneColor.image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->velocityBuffer.image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers[0].image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers[1].image.Handle());

		for (int i = 0; i < 2; ++i) {
			_impl->taaPass.WriteIndex(
				_impl->ctx.Device(), i,
				Vk::ImageWrite{.view = _impl->sceneColor.view.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::ImageWrite{.view = _impl->accumBuffers[1 - i].view.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::ImageWrite{.view = _impl->velocityBuffer.view.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::SamplerWrite{.sampler = _impl->defaultSampler.Get()});
		}

		_impl->needsInitialClear = false;
	}
}

void RenderContext::Impl::RenderShadowPass(VkCommandBuffer cmd) {
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
		cmd, shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);

	VkRenderingAttachmentInfo depthAtt = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = shadowMap.view.Get(),
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}}};

	VkRenderingInfo shadowRenderInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = {.offset = {0, 0}, .extent = {SHADOW_RES, SHADOW_RES}},
		.layerCount = 1,
		.pDepthAttachment = &depthAtt};

	vkCmdBeginRendering(cmd, &shadowRenderInfo);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline.Get());

	VkViewport shadowViewport = {.x = 0.0f,
								 .y = (float)SHADOW_RES,
								 .width = (float)SHADOW_RES,
								 .height = -(float)SHADOW_RES,
								 .minDepth = 0.0f,
								 .maxDepth = 1.0f};
	VkRect2D shadowScissor = {.offset = {0, 0}, .extent = {SHADOW_RES, SHADOW_RES}};
	vkCmdSetViewport(cmd, 0, 1, &shadowViewport);
	vkCmdSetScissor(cmd, 0, 1, &shadowScissor);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout.Get(), 0, 1,
							&bindlessSet, 0, nullptr);

	for (const auto& draw : drawQueue) {
		auto* mesh = std::bit_cast<NativeMesh*>(draw.mesh);
		JPH::Mat44 lightMVP = shadowProjView * draw.transform;

		FrameConstants shadowConstants = {.transform = lightMVP,
										  .prevTransform = JPH::Mat44::sIdentity(),
										  .albedoIndex = 0,
										  .normalIndex = 0,
										  .pbrIndex = 0,
										  .emissiveIndex = 0,
										  .isShadowPass = 1,
										  ._padding = {0, 0, 0}};

		vkCmdPushConstants(cmd, shadowPipelineLayout.Get(), VK_SHADER_STAGE_VERTEX_BIT, 0,
						   sizeof(FrameConstants), &shadowConstants);

		VkDeviceSize offset = 0;
		VkBuffer vbo = mesh->buffer.Handle();
		vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
		vkCmdDraw(cmd, mesh->vertexCount, 1, 0, 0);
	}

	vkCmdEndRendering(cmd);

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, shadowMap.image.Handle(),
																   VK_IMAGE_ASPECT_DEPTH_BIT);
}

void RenderContext::Impl::RenderMainPass(RenderContext& ctx, VkCommandBuffer cmd) {
	Renderer::Clear(ctx, {0.08f, 0.09f, 0.12f, 1.0f});

	uint32_t drawCount = static_cast<uint32_t>(drawQueue.size());
	if (drawCount == 0) {
		return;
	}

	// 1. Allocate continuous temporary sort items
	std::vector<SortItem> items(drawCount);
	std::vector<SortItem> temp(drawCount);

	for (uint32_t i = 0; i < drawCount; ++i) {
		items[i] = {.key = SortKey::Pack(drawQueue[i].material, drawQueue[i].mesh), .payload = i};
	}

	// 2. Perform stable Radix Sort on the packed keys
	RadixSort64(items.data(), temp.data(), drawCount);

	// 3. Reorder drawQueue elements on-the-fly to preserve cache locality for workers
	std::vector<DrawCommand> sortedDrawQueue(drawCount);
	for (uint32_t i = 0; i < drawCount; ++i) {
		sortedDrawQueue[i] = drawQueue[items[i].payload];
	}
	drawQueue = std::move(sortedDrawQueue);

	// 4. Submit draw commands in sorted order across threads
	uint32_t numChunks = (drawCount + 255) / 256;
	std::vector<VkCommandBuffer> secondaries(numChunks);

	std::array<VkFormat, 2> formats = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT};
	const VkCommandBufferInheritanceRenderingInfo inherit = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
		.pNext = nullptr,
		.flags = 0,
		.viewMask = 0,
		.colorAttachmentCount = 2,
		.pColorAttachmentFormats = formats.data(),
		.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
		.stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};

	const VkCommandBufferInheritanceInfo pInherit = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
		.pNext = &inherit,
		.renderPass = VK_NULL_HANDLE,
		.subpass = 0,
		.framebuffer = VK_NULL_HANDLE,
		.occlusionQueryEnable = VK_FALSE,
		.queryFlags = 0,
		.pipelineStatistics = 0};

	TaskSystem::ParallelFor(
		drawCount, 256, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) -> void {
			uint32_t wIdx = TaskSystem::GetWorkerIndex();
			if (wIdx >= workerCmds.size()) {
				wIdx = (uint32_t)(workerCmds.size() - 1);
			}

			uint32_t localCmdIdx =
				workerCmds[wIdx].cmdCount[frame_index].fetch_add(1, std::memory_order_relaxed);
			VkCommandBuffer sec_cmd = workerCmds[wIdx].pools[frame_index][localCmdIdx];

			const VkCommandBufferBeginInfo beginInfo = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.pNext = nullptr,
				.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
						 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				.pInheritanceInfo = &pInherit};
			vkBeginCommandBuffer(sec_cmd, &beginInfo);

			VkExtent2D extent = presentation.swapchain.Get().extent;
			const VkViewport viewport = {.x = 0.0f,
										 .y = (float)extent.height,
										 .width = (float)extent.width,
										 .height = -(float)extent.height,
										 .minDepth = 0.0f,
										 .maxDepth = 1.0f};
			const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = extent};
			vkCmdSetViewport(sec_cmd, 0, 1, &viewport);
			vkCmdSetScissor(sec_cmd, 0, 1, &scissor);

			for (uint32_t i = start; i < end; ++i) {
				const auto& drawCmd = drawQueue[i]; // Now naturally aligned in memory

				if (!drawCmd.material->pipeline.Valid()) {
					continue;
				}

				vkCmdBindPipeline(sec_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
								  drawCmd.material->pipeline.Get());
				vkCmdBindDescriptorSets(sec_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
										drawCmd.material->layout.Get(), 0, 1, &bindlessSet, 0,
										nullptr);

				FrameConstants constants = {.transform = drawCmd.transform,
											.prevTransform = drawCmd.prevTransform,
											.albedoIndex = drawCmd.albedoIndex,
											.normalIndex = drawCmd.normalIndex,
											.pbrIndex = drawCmd.pbrIndex,
											.emissiveIndex = drawCmd.emissiveIndex};

				vkCmdPushConstants(sec_cmd, drawCmd.material->layout.Get(),
								   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
								   sizeof(FrameConstants), &constants);

				VkDeviceSize offset = 0;
				VkBuffer vbo = drawCmd.mesh->buffer.Handle();
				vkCmdBindVertexBuffers(sec_cmd, 0, 1, &vbo, &offset);
				vkCmdDraw(sec_cmd, drawCmd.mesh->vertexCount, 1, 0, 0);
			}

			ZHLN_EndCommandBuffer(sec_cmd);
			secondaries[chunkIdx] = sec_cmd;
		});

	Vk::ExecuteCommands(cmd, secondaries);
	drawQueue.clear();
}

void RenderContext::Impl::ApplyTAAPass(VkCommandBuffer cmd, VkExtent2D extent) {
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		cmd, accumBuffers.Current().image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		cmd, accumBuffers.Next().image.Handle());

	VkRenderingAttachmentInfo col = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
									 .pNext = nullptr,
									 .imageView = accumBuffers.Next().view.Get(),
									 .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
									 .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
									 .storeOp = VK_ATTACHMENT_STORE_OP_STORE};

	const VkRenderingInfo renderInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.pNext = nullptr,
		.flags = 0,
		.renderArea = {.offset = {.x = 0, .y = 0}, .extent = extent},
		.layerCount = 1,
		.viewMask = 0,
		.colorAttachmentCount = 1,
		.pColorAttachments = &col,
		.pDepthAttachment = nullptr,
		.pStencilAttachment = nullptr};

	vkCmdBeginRendering(cmd, &renderInfo);

	taaPass.WriteNext(ctx.Device(),
					  Vk::ImageWrite{.view = sceneColor.view.Get(),
									 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
					  Vk::ImageWrite{.view = accumBuffers.Current().view.Get(),
									 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
					  Vk::ImageWrite{.view = velocityBuffer.view.Get(),
									 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
					  Vk::SamplerWrite{.sampler = defaultSampler.Get()});

	taaPass.Execute(cmd, g_TAAState.feedback);
	ZHLN_EndRendering(cmd);

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		cmd, accumBuffers.Next().image.Handle());
}

void RenderContext::Impl::BlitAndDrawUI(VkCommandBuffer cmd, VkExtent2D extent, uint32_t imageIdx) {
	VkImage swapImg = presentation.swapchain.Get().images[imageIdx];
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		cmd, swapImg);

	bool useTAA = g_TAAState.enabled && taaPass.pipeline.Valid();
	VkImageView blitSource = useTAA ? accumBuffers.Next().view.Get() : sceneColor.view.Get();

	blitPass.WriteNext(
		ctx.Device(),
		Vk::ImageWrite{.view = blitSource, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		Vk::SamplerWrite{defaultSampler.Get()});

	if (blitPass.pipeline.Valid()) {
		VkRenderingAttachmentInfo col = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
										 .pNext = nullptr,
										 .imageView = presentation.swapchain.Get().views[imageIdx],
										 .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
										 .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
										 .storeOp = VK_ATTACHMENT_STORE_OP_STORE};

		const VkRenderingInfo renderInfo = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = 0,
			.renderArea{.offset = {.x = 0, .y = 0}, .extent = extent},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &col};

		vkCmdBeginRendering(cmd, &renderInfo);
		blitPass.Execute(cmd);

		if (!uiDrawQueue.empty()) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline.Get());
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipelineLayout.Get(), 0,
									1, &bindlessSet, 0, nullptr);

			struct UIObjectConstants {
				JPH::Mat44 orthoMatrix;
				JPH::Mat44 unused;
				uint32_t albedoIdx;
				uint32_t padding[3];
			} pc{};

			pc.orthoMatrix = GUI::CreateOrthoMatrix((float)extent.width, (float)extent.height);

			for (const auto& draw : uiDrawQueue) {
				pc.albedoIdx = draw.fontIndex;
				vkCmdPushConstants(cmd, uiPipelineLayout.Get(),
								   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
								   sizeof(UIObjectConstants), &pc);

				VkDeviceSize offset = 0;
				VkBuffer vbo = draw.mesh->buffer.Handle();
				vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
				vkCmdDraw(cmd, draw.mesh->vertexCount, 1, 0, 0);
			}
			uiDrawQueue.clear();
		}

		ImGui::Render();
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
		ZHLN_EndRendering(cmd);
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(
		cmd, swapImg);
}

void RenderContext::Impl::SubmitFrame() {
	const ZHLN_FrameSync& s = sync[frame_index];
	ZHLN_FrameSubmitDesc submitDesc = {.graphicsQueue = ctx.GraphicsQueue(),
									   .presentQueue = ctx.PresentQueue(),
									   .cmd = current_cmd,
									   .imageAvailable = s.image_available,
									   .renderFinished =
										   presentation.presentSemaphores[current_image_index],
									   .inFlight = s.in_flight,
									   .swapchain = presentation.swapchain.Get().handle,
									   .imageIndex = current_image_index};

	if (Vk::SubmitAndPresent(submitDesc) != ZHLN_FrameResult_Ok) {
		resized = true;
	}

	accumBuffers.Flip();
	taaPass.sets.Flip();
	blitPass.sets.Flip();

	frame_index = (frame_index + 1) % 2;
	current_cmd = VK_NULL_HANDLE;
}

void RenderContext::EndFrame() {
	ZHLN_PROFILE_SCOPE("Render (CPU Record)");
	if (_impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	VkExtent2D extent = _impl->presentation.swapchain.Get().extent;

	if (!_impl->drawQueue.empty()) {
		_impl->RenderShadowPass(_impl->current_cmd);
		_impl->RenderMainPass(*this, _impl->current_cmd);
	}

	ZHLN_EndRendering(_impl->current_cmd);

	if (!_impl->sceneColor.image.Valid() || !_impl->accumBuffers.Current().image.Valid()) {
		ZHLN_EndCommandBuffer(_impl->current_cmd);
		_impl->current_cmd = VK_NULL_HANDLE;
		return;
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		_impl->current_cmd, _impl->sceneColor.image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		_impl->current_cmd, _impl->velocityBuffer.image.Handle());

	if (g_TAAState.enabled && _impl->taaPass.pipeline.Valid()) {
		_impl->ApplyTAAPass(_impl->current_cmd, extent);
	}

	_impl->BlitAndDrawUI(_impl->current_cmd, extent, _impl->current_image_index);
	ZHLN_EndCommandBuffer(_impl->current_cmd);

	_impl->SubmitFrame();
}

namespace Renderer {
void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		impl->current_cmd, impl->sceneColor.image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		impl->current_cmd, impl->velocityBuffer.image.Handle());

	if (!impl->depth_ready) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			impl->current_cmd, impl->presentation.depthTarget.image.Handle(),
			VK_IMAGE_ASPECT_DEPTH_BIT);
		impl->depth_ready = true;
	} else {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			impl->current_cmd, impl->presentation.depthTarget.image.Handle(),
			VK_IMAGE_ASPECT_DEPTH_BIT);
	}

	std::array<VkRenderingAttachmentInfo, 2> cols = {
		VkRenderingAttachmentInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.pNext = nullptr,
			.imageView = impl->sceneColor.view.Get(),
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.resolveMode = VK_RESOLVE_MODE_NONE,
			.resolveImageView = VK_NULL_HANDLE,
			.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = {.color = {.float32 = {color.GetX(), color.GetY(), color.GetZ(),
												 color.GetW()}}}},
		{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		 .pNext = nullptr,
		 .imageView = impl->velocityBuffer.view.Get(),
		 .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		 .resolveMode = VK_RESOLVE_MODE_NONE,
		 .resolveImageView = VK_NULL_HANDLE,
		 .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		 .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}}};
	VkRenderingAttachmentInfo depthAtt = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.pNext = nullptr,
		.imageView = impl->presentation.depthTarget.view.Get(),
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
		.resolveMode = VK_RESOLVE_MODE_NONE,
		.resolveImageView = VK_NULL_HANDLE,
		.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = {.depthStencil = {.depth = depth, .stencil = 0}}};
	const VkRenderingInfo renderInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.pNext = nullptr,
		.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
		.renderArea{.offset = {.x = 0, .y = 0},
					.extent = impl->presentation.swapchain.Get().extent},
		.layerCount = 1,
		.viewMask = 0,
		.colorAttachmentCount = 2,
		.pColorAttachments = cols.data(),
		.pDepthAttachment = &depthAtt,
		.pStencilAttachment = nullptr};

	vkCmdBeginRendering(impl->current_cmd, &renderInfo);

	const VkViewport viewport = {.x = 0.0f,
								 .y = (float)impl->presentation.swapchain.Get().extent.height,
								 .width = (float)impl->presentation.swapchain.Get().extent.width,
								 .height = -(float)impl->presentation.swapchain.Get().extent.height,
								 .minDepth = 0.0f,
								 .maxDepth = 1.0f};
	const VkRect2D scissor = {.offset = {.x = 0, .y = 0},
							  .extent = impl->presentation.swapchain.Get().extent};
	vkCmdSetViewport(impl->current_cmd, 0, 1, &viewport);
	vkCmdSetScissor(impl->current_cmd, 0, 1, &scissor);
}

void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj, const JPH::Mat44& prevViewProj) {
	auto* impl = ctx.GetImpl();
	impl->current_view_proj = viewProj;
	impl->prev_view_proj = prevViewProj;
}

void SetFrameData(RenderContext& ctx, const FrameUniforms& uniforms,
				  const JPH::Mat44& shadowProjView) {
	auto* impl = ctx.GetImpl();
	impl->shadowProjView = shadowProjView;
	std::memcpy(impl->frameUniformBuffer.Map().data, &uniforms, sizeof(FrameUniforms));
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform, const JPH::Mat44& prevTransform) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}
	impl->drawQueue.push_back({.material = std::bit_cast<NativeMaterial*>(material.pipeline),
							   .mesh = std::bit_cast<NativeMesh*>(mesh.vertexBuffer),
							   .transform = transform,
							   .prevTransform = prevTransform,
							   .albedoIndex = material.albedoIndex,
							   .normalIndex = material.normalIndex,
							   .pbrIndex = material.pbrIndex,
							   .emissiveIndex = material.emissiveIndex});
}

void DrawUI(RenderContext& ctx, const Mesh& mesh, uint32_t fontIndex) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	impl->uiDrawQueue.push_back(
		{.mesh = std::bit_cast<NativeMesh*>(mesh.vertexBuffer), .fontIndex = fontIndex});
}
} // namespace Renderer
} // namespace ZHLN
