// File: src/engine/RenderFrame.cpp
#include "RenderInternal.hpp"
#include "Zahlen/GUI.hpp"
#include "Zahlen/Profiler.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "detail/RadixSort.hpp"
#include "engine/RenderState.hpp"
#include "imgui.h"

#include <algorithm>
#include <bit>
#include <threading/TaskSystem.hpp>

namespace ZHLN {

void RenderContext::BeginFrame() {
	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, _impl->pools[_impl->frame_index]);

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

	// Defensive image acquisition check
	auto acq_res = ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index);
	if (acq_res == ZHLN_FrameResult_OutOfDate || acq_res == ZHLN_FrameResult_Suboptimal) {
		_impl->resized = true;
		_impl->current_cmd = VK_NULL_HANDLE;
		return;
	} else if (acq_res == ZHLN_FrameResult_Error) {
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

		// Self-contained DynamicPass clears the framebuffers once on startup
		Vk::DynamicPass<4, false>(_impl->presentation.swapchain.Get().extent)
			.Color(0, _impl->sceneColor.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(1, _impl->velocityBuffer.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(2, _impl->accumBuffers[0].view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(3, _impl->accumBuffers[1].view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR)
			.ClearColor(0, 0.0f, 0.0f, 0.0f, 1.0f)
			.ClearColor(1, 0.0f, 0.0f, 0.0f, 1.0f)
			.ClearColor(2, 0.0f, 0.0f, 0.0f, 1.0f)
			.ClearColor(3, 0.0f, 0.0f, 0.0f, 1.0f)
			.Execute(_impl->current_cmd, []() {});

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

	Vk::DynamicPass<0, true>({.width = SHADOW_RES, .height = SHADOW_RES})
		.Depth(shadowMap.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
			   1.0f)
		.Execute(cmd, [&]() {
			for (const auto& draw : drawQueue) {
				auto* mesh = std::bit_cast<NativeMesh*>(draw.mesh);
				JPH::Mat44 lightMVP = shadowProjView * draw.transform;

				FrameConstants shadowConstants = {
					.transform = lightMVP,
					.prevTransform = JPH::Mat44::sIdentity(),
					.albedoIndex = draw.albedoIndex,
					.normalIndex = 0,
					.pbrIndex = 0,
					.emissiveIndex = 0,
					.isShadowPass = 1,
					.metallicFactor = 0.0f,
					.roughnessFactor = 0.0f,
					.alphaCutoff = draw.alphaCutoff,
					.alphaMode = draw.alphaMode,
					.jointOffset = draw.jointOffset,
					.isSkinned = draw.isSkinned,
					._padding = {},
					.baseColorFactor = {draw.baseColorFactor[0], draw.baseColorFactor[1],
										draw.baseColorFactor[2], draw.baseColorFactor[3]}};

				Vk::DrawInstanced(cmd,
								  {.pipeline = shadowPipeline.Get(),
								   .layout = shadowPipelineLayout.Get(),
								   .set = bindlessSet,
								   .vbo = mesh->buffer.Handle(),
								   .vertexCount = mesh->vertexCount},
								  shadowConstants,
								  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			}
		});

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, shadowMap.image.Handle(),
																   VK_IMAGE_ASPECT_DEPTH_BIT);
}

void RenderContext::Impl::RenderMainPass(RenderContext& ctx, VkCommandBuffer cmd) {
	if (RenderMainPassGpuCulling(ctx, cmd)) {
		return;
	}

	ZHLN::Log("RENDERING DIAGNOSTIC: Falling back to CPU Traditional Path!");

	auto drawCount = static_cast<uint32_t>(drawQueue.size());
	if (drawCount == 0) {
		return;
	}

	// 1. Perform stable Radix Sort
	JPH::Array<SortItem> items(drawCount);
	JPH::Array<SortItem> temp(drawCount);

	for (uint32_t i = 0; i < drawCount; ++i) {
		items[i] = {.key = SortKey::Pack(drawQueue[i].material, drawQueue[i].mesh), .payload = i};
	}

	RadixSort64(items.data(), temp.data(), drawCount);

	JPH::Array<DrawCommand> sortedDrawQueue(drawCount);
	for (uint32_t i = 0; i < drawCount; ++i) {
		sortedDrawQueue[i] = drawQueue[items[i].payload];
	}
	drawQueue = std::move(sortedDrawQueue);

	// 2. Transition layouts to optimal states before starting the render pass
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, sceneColor.image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd,
																   velocityBuffer.image.Handle());

	if (!depth_ready) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
		depth_ready = true;
	} else {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
	}

	// 3. Prepare secondary command buffers
	uint32_t numChunks = (drawCount + 255) / 256;
	JPH::Array<VkCommandBuffer> secondaries(numChunks);

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

	Vk::DynamicPass<2, true>(presentation.swapchain.Get().extent)
		.Color(0, sceneColor.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE)
		.Color(1, velocityBuffer.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR,
			   VK_ATTACHMENT_STORE_OP_STORE)
		.Depth(presentation.depthTarget.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR,
			   VK_ATTACHMENT_STORE_OP_STORE, 1.0f)
		.ClearColor(0, 0.08f, 0.09f, 0.12f, 1.0f)
		.ClearColor(1, 0.0f, 0.0f, 0.0f, 0.0f)
		.Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
		.Execute(cmd, [&]() {
			TaskSystem::ParallelFor(
				drawCount, 256, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) -> void {
					uint32_t wIdx = TaskSystem::GetWorkerIndex();
					if (wIdx >= workerCmds.size()) {
						wIdx = (uint32_t)(workerCmds.size() - 1);
					}

					uint32_t localCmdIdx = workerCmds[wIdx].cmdCount[frame_index].fetch_add(
						1, std::memory_order_relaxed);
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
						const auto& drawCmd = drawQueue[i];

						if (!drawCmd.material->pipeline.Valid()) {
							continue;
						}

						Vk::DrawInstanced(
							sec_cmd,
							{.pipeline = drawCmd.material->pipeline.Get(),
							 .layout = drawCmd.material->layout.Get(),
							 .set = bindlessSet,
							 .vbo = drawCmd.mesh->buffer.Handle(),
							 .vertexCount = drawCmd.mesh->vertexCount},
							FrameConstants{.transform = drawCmd.transform,
										   .prevTransform = drawCmd.prevTransform,
										   .albedoIndex = drawCmd.albedoIndex,
										   .normalIndex = drawCmd.normalIndex,
										   .pbrIndex = drawCmd.pbrIndex,
										   .emissiveIndex = drawCmd.emissiveIndex,
										   .isShadowPass = 0,
										   .metallicFactor = drawCmd.metallicFactor,
										   .roughnessFactor = drawCmd.roughnessFactor,
										   .alphaCutoff = drawCmd.alphaCutoff,
										   .alphaMode = drawCmd.alphaMode,
										   .jointOffset = drawCmd.jointOffset,
										   .isSkinned = drawCmd.isSkinned,
										   ._padding = {},
										   .baseColorFactor = {drawCmd.baseColorFactor[0],
															   drawCmd.baseColorFactor[1],
															   drawCmd.baseColorFactor[2],
															   drawCmd.baseColorFactor[3]}});
					}

					ZHLN_EndCommandBuffer(sec_cmd);
					secondaries[chunkIdx] = sec_cmd;
				});

			Vk::ExecuteCommands(cmd, secondaries);
		});

	drawQueue.clear();
}

bool RenderContext::Impl::RenderMainPassGpuCulling(RenderContext& /*ctx*/, VkCommandBuffer cmd) {
	auto drawCount = static_cast<uint32_t>(drawQueue.size());
	if (drawCount == 0) {
		return true;
	}

	if (!cullingPipeline.Valid() || !instanceDataBuffer.Valid() ||
		!indirectCommandsBuffer.Valid()) {
		return false;
	}

	if (drawCount > kGpuCullingMaxInstances) {
		return false;
	}

	JPH::Array<SortItem> items(drawCount);
	JPH::Array<SortItem> temp(drawCount);

	for (uint32_t i = 0; i < drawCount; ++i) {
		items[i] = {.key = SortKey::Pack(drawQueue[i].material, drawQueue[i].mesh), .payload = i};
	}

	RadixSort64(items.data(), temp.data(), drawCount);

	JPH::Array<DrawCommand> sortedDrawQueue(drawCount);
	for (uint32_t i = 0; i < drawCount; ++i) {
		sortedDrawQueue[i] = drawQueue[items[i].payload];
	}
	drawQueue = std::move(sortedDrawQueue);

	struct GroupRange {
		NativeMaterial* material;
		NativeMesh* mesh;
		uint32_t start;
		uint32_t count;
	};

	JPH::Array<GroupRange> groups;
	groups.reserve((drawCount + 15) / 16);

	// Keep mapRegion in scope so the destructor unmaps only AFTER the loop ends
	auto mapRegion = instanceDataBuffer.Map();
	auto* mapped = reinterpret_cast<InstanceData*>(mapRegion.data);

	NativeMaterial* currentMaterial = nullptr;
	NativeMesh* currentMesh = nullptr;
	for (uint32_t i = 0; i < drawCount; ++i) {
		const auto& drawCmd = drawQueue[i];
		mapped[i] = InstanceData{
			.world = drawCmd.transform,
			.prevWorld = drawCmd.prevTransform,
			.albedoIndex = drawCmd.albedoIndex,
			.normalIndex = drawCmd.normalIndex,
			.pbrIndex = drawCmd.pbrIndex,
			.emissiveIndex = drawCmd.emissiveIndex,
			.vertexCount = drawCmd.mesh->vertexCount,
			.cullRadius = drawCmd.cullRadius,
			.metallicFactor = drawCmd.metallicFactor,
			.roughnessFactor = drawCmd.roughnessFactor,
			.alphaCutoff = drawCmd.alphaCutoff,
			.alphaMode = drawCmd.alphaMode,
			.jointOffset = drawCmd.jointOffset,
			.isSkinned = drawCmd.isSkinned,
			.baseColorFactor = {drawCmd.baseColorFactor[0], drawCmd.baseColorFactor[1],
								drawCmd.baseColorFactor[2], drawCmd.baseColorFactor[3]}};

		if (i == 0 || drawCmd.material != currentMaterial || drawCmd.mesh != currentMesh) {
			groups.push_back(
				{.material = drawCmd.material, .mesh = drawCmd.mesh, .start = i, .count = 1});
			currentMaterial = drawCmd.material;
			currentMesh = drawCmd.mesh;
		} else {
			groups.back().count++;
		}
	}

	struct FrustumPlanes {
		JPH::Vec4 planes[6];
		uint32_t drawCount;
	};
	FrustumPlanes planes{};

	const auto& vp = current_view_proj;
	JPH::Vec4 r0(vp(0, 0), vp(0, 1), vp(0, 2), vp(0, 3));
	JPH::Vec4 r1(vp(1, 0), vp(1, 1), vp(1, 2), vp(1, 3));
	JPH::Vec4 r2(vp(2, 0), vp(2, 1), vp(2, 2), vp(2, 3));
	JPH::Vec4 r3(vp(3, 0), vp(3, 1), vp(3, 2), vp(3, 3));

	auto NormalizePlane = [&](const JPH::Vec4& plane) {
		float len = JPH::Vec3(plane.GetX(), plane.GetY(), plane.GetZ()).Length();
		return plane / std::max(len, 1e-6f);
	};

	planes.planes[0] = NormalizePlane(r3 + r0);
	planes.planes[1] = NormalizePlane(r3 - r0);
	planes.planes[2] = NormalizePlane(r3 + r1);
	planes.planes[3] = NormalizePlane(r3 - r1);
	planes.planes[4] = NormalizePlane(r2);
	planes.planes[5] = NormalizePlane(r3 - r2);
	planes.drawCount = drawCount;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullingPipeline.Get());
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullingPipelineLayout.Get(), 0, 1,
							&cullingSet, 0, nullptr);
	vkCmdPushConstants(cmd, cullingPipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
					   sizeof(FrustumPlanes), &planes);

	vkCmdDispatch(cmd, (drawCount + 63) / 64, 1, 1);

	const VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &barrier, 0, nullptr, 0,
						 nullptr);

	// Transition layouts to optimal states before starting the render pass
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, sceneColor.image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd,
																   velocityBuffer.image.Handle());

	if (!depth_ready) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
		depth_ready = true;
	} else {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
	}

	Vk::DynamicPass<2, true>(presentation.swapchain.Get().extent)
		.Color(0, sceneColor.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE)
		.Color(1, velocityBuffer.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR,
			   VK_ATTACHMENT_STORE_OP_STORE)
		.Depth(presentation.depthTarget.view.Get(), VK_ATTACHMENT_LOAD_OP_CLEAR,
			   VK_ATTACHMENT_STORE_OP_STORE, 1.0f)
		.ClearColor(0, 0.08f, 0.09f, 0.12f, 1.0f)
		.ClearColor(1, 0.0f, 0.0f, 0.0f, 0.0f)
		.Execute(cmd, [&]() {
			const VkDeviceSize stride = sizeof(VkDrawIndirectCommand);
			for (const auto& group : groups) {
				if (!group.material->pipeline.Valid()) {
					continue;
				}

				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
								  group.material->pipeline.Get());
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
										group.material->layout.Get(), 0, 1, &bindlessSet, 0,
										nullptr);

				FrameConstants constants{};
				vkCmdPushConstants(cmd, group.material->layout.Get(),
								   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
								   sizeof(FrameConstants), &constants);

				VkDeviceSize offset = 0;
				VkBuffer vbo = group.mesh->buffer.Handle();
				vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &offset);
				vkCmdDrawIndirect(cmd, indirectCommandsBuffer.Handle(), group.start * stride,
								  group.count, stride);
			}
		});

	drawQueue.clear();
	return true;
}

void RenderContext::Impl::ApplyTAAPass(VkCommandBuffer cmd, VkExtent2D extent) {
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		cmd, accumBuffers.Current().image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		cmd, accumBuffers.Next().image.Handle());

	Vk::DynamicPass<1, false>(extent)
		.Color(0, accumBuffers.Next().view.Get(), VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		.Execute(cmd, [&]() {
			taaPass.WriteNext(ctx.Device(),
							  Vk::ImageWrite{.view = sceneColor.view.Get(),
											 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
							  Vk::ImageWrite{.view = accumBuffers.Current().view.Get(),
											 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
							  Vk::ImageWrite{.view = velocityBuffer.view.Get(),
											 .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
							  Vk::SamplerWrite{.sampler = defaultSampler.Get()});

			taaPass.Execute(cmd, g_TAAState.feedback);
		});

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
		Vk::DynamicPass<1, false>(extent)
			.Color(0, presentation.swapchain.Get().views[imageIdx], VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() {
				blitPass.Execute(cmd);

				if (!uiDrawQueue.empty()) {
					struct UIObjectConstants {
						JPH::Mat44 orthoMatrix;
						JPH::Mat44 unused;
						uint32_t albedoIdx;
					} pc{};

					pc.orthoMatrix =
						GUI::CreateOrthoMatrix((float)extent.width, (float)extent.height);

					for (const auto& draw : uiDrawQueue) {
						pc.albedoIdx = draw.fontIndex;

						Vk::DrawInstanced(cmd,
										  {.pipeline = uiPipeline.Get(),
										   .layout = uiPipelineLayout.Get(),
										   .set = bindlessSet,
										   .vbo = draw.mesh->buffer.Handle(),
										   .vertexCount = draw.mesh->vertexCount},
										  pc);
					}
					uiDrawQueue.clear();
				}

				ImGui::Render();
				ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
			});
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(
		cmd, swapImg);
}

void RenderContext::Impl::SubmitFrame() {
	const ZHLN_FrameSync& s = sync[frame_index];
	ZHLN_FrameSubmitDesc submitDesc = {
		.graphicsQueue = ctx.GraphicsQueue(),
		.presentQueue = ctx.PresentQueue(),
		.cmd = current_cmd,
		.imageAvailable = s.image_available,
		.renderFinished =
			presentation.presentSemaphores[current_image_index], // Safely reverted back to
																 // swapchain image index tracking
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
void Clear(RenderContext& ctx, const ZHLN::Color4& color, float depth, bool useSecondaries) {
	// Standalone no-op: clearing is now cleanly integrated into attachment load operations
	// inside RenderMainPass and RenderMainPassGpuCulling.
	(void)ctx;
	(void)color;
	(void)depth;
	(void)useSecondaries;
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
		  const JPH::Mat44& transform, const JPH::Mat44& prevTransform, float cullRadius,
		  uint32_t jointOffset, bool isSkinned) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	if (mesh.vertexBuffer == BufferHandle::Invalid) {
		return;
	}

	impl->drawQueue.push_back(
		{.material = std::bit_cast<NativeMaterial*>(material.pipeline),
		 .mesh = std::bit_cast<NativeMesh*>(mesh.vertexBuffer),
		 .transform = transform,
		 .prevTransform = prevTransform,
		 .albedoIndex = material.albedoIndex,
		 .normalIndex = material.normalIndex,
		 .pbrIndex = material.pbrIndex,
		 .emissiveIndex = material.emissiveIndex,
		 .cullRadius = cullRadius,
		 .metallicFactor = material.metallicFactor,
		 .roughnessFactor = material.roughnessFactor,
		 .alphaCutoff = material.alphaCutoff,
		 .alphaMode = material.alphaMode,
		 .jointOffset = jointOffset,
		 .isSkinned = isSkinned ? 1u : 0u,
		 .baseColorFactor = {material.baseColorFactor[0], material.baseColorFactor[1],
							 material.baseColorFactor[2], material.baseColorFactor[3]}});
}

void DrawUI(RenderContext& ctx, const Mesh& mesh, uint32_t fontIndex) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	if (mesh.vertexBuffer == BufferHandle::Invalid) {
		return;
	}

	impl->uiDrawQueue.push_back(
		{.mesh = std::bit_cast<NativeMesh*>(mesh.vertexBuffer), .fontIndex = fontIndex});
}
} // namespace Renderer
} // namespace ZHLN
