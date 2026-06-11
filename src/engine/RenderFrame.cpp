// File: src/engine/RenderFrame.cpp
#include "ParallelDraw.hpp"
#include "RenderInternal.hpp"
#include "Zahlen/GUI.hpp"
#include "Zahlen/Profiler.hpp"
#include "backends/imgui_impl_vulkan.h"
#include "detail/RadixSort.hpp"
#include "imgui.h"

#include <algorithm>
#include <bit>
#include <threading/TaskSystem.hpp>

namespace ZHLN {

void RenderContext::Impl::SortDrawQueue() {
	auto drawCount = static_cast<uint32_t>(drawQueue.size());
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
}

Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>
RenderContext::Impl::GetDepthAttachmentForMainPass(VkCommandBuffer cmd) {
	if (!depth_ready) {
		auto depth_u = presentation.depthTarget.State();
		depth_ready = true;
		return Vk::Transition<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, depth_u);
	}

	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth_current = {
		.handle = presentation.depthTarget.image.Handle(),
		.view = presentation.depthTarget.view.Get(),
		.extent = presentation.depthTarget.extent,
		.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
	return Vk::Transition<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, depth_current);
}

void RenderContext::BeginFrame() {
	if (_impl->initFence != VK_NULL_HANDLE) {
		vkWaitForFences(_impl->ctx.Device(), 1, &_impl->initFence, VK_TRUE, UINT64_MAX);
		vkDestroyFence(_impl->ctx.Device(), _impl->initFence, nullptr);
		_impl->initFence = VK_NULL_HANDLE;
		delete _impl->stagingContext;
		_impl->stagingContext = nullptr;
	}

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, _impl->pools[_impl->frame_index]);

	float timestampPeriod = _impl->ctx.PhysicalInfo().properties.properties.limits.timestampPeriod;
	_impl->gpuProfiler.RetrieveResults(
		_impl->frame_index, timestampPeriod,
		[](std::string_view name, float durationMS) { CPUProfiler::Record(name, durationMS); });
	_impl->gpuProfiler.Reset(_impl->frame_index);

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
		_impl->normalRoughnessBuffer = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
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
	}
	if (acq_res == ZHLN_FrameResult_Error) {
		_impl->current_cmd = VK_NULL_HANDLE;
		return;
	}

	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);

	if (_impl->needsInitialClear) {
		auto sColor_u = _impl->sceneColor.State();
		auto sVel_u = _impl->velocityBuffer.State();
		auto sAcc0_u = _impl->accumBuffers[0].State();
		auto sAcc1_u = _impl->accumBuffers[1].State();
		auto sNorm_u = _impl->normalRoughnessBuffer.State();

		auto sColor_att =
			Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(_impl->current_cmd, sColor_u);
		auto sVel_att =
			Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(_impl->current_cmd, sVel_u);
		auto sAcc0_att =
			Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(_impl->current_cmd, sAcc0_u);
		auto sAcc1_att =
			Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(_impl->current_cmd, sAcc1_u);
		auto sNorm_att =
			Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(_impl->current_cmd, sNorm_u);

		Vk::DynamicPass<5, false>(_impl->presentation.swapchain.Get().extent)
			.Color(0, sColor_att, VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(1, sVel_att, VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(2, sAcc0_att, VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(3, sAcc1_att, VK_ATTACHMENT_LOAD_OP_CLEAR)
			.Color(4, sNorm_att, VK_ATTACHMENT_LOAD_OP_CLEAR) // Clear 5th target
			.ClearColor(0, 0.0f, 0.0f, 0.0f, 1.0f)
			.ClearColor(1, 0.0f, 0.0f, 0.0f, 1.0f)
			.ClearColor(2, 0.0f, 0.0f, 0.0f, 1.0f)
			.ClearColor(3, 0.0f, 0.0f, 0.0f, 1.0f)
			.ClearColor(4, 0.0f, 0.0f, 0.0f, 1.0f)
			.Execute(_impl->current_cmd, []() {});

		auto sColor_ro = Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, sColor_att);
		auto sVel_ro =
			Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->current_cmd, sVel_att);
		auto sAcc0_ro =
			Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->current_cmd, sAcc0_att);
		auto sAcc1_ro =
			Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->current_cmd, sAcc1_att);
		[[maybe_unused]] auto sNorm_ro =
			Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->current_cmd, sNorm_att);

		for (int i = 0; i < 2; ++i) {
			_impl->taaPass.WriteIndex(
				_impl->ctx.Device(), i, sColor_ro, i == 0 ? sAcc1_ro : sAcc0_ro, sVel_ro,
				Vk::SamplerWrite{.sampler = _impl->defaultSampler.Get()},
				Vk::BufferWrite{.buffer = _impl->frameUniformBuffers[i].Handle()});
		}

		_impl->needsInitialClear = false;
	}
}

void RenderContext::Impl::RenderShadowPass(VkCommandBuffer cmd) {
	Profiler::ScopedGpuProfile<Stages::ShadowPass, FrameProfiler> timer(cmd, frame_index,
																		gpuProfiler);
	auto shadow_u = shadowMap.State();
	auto shadow_att = Vk::Transition<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, shadow_u);

	Vk::DynamicPass<0, true>({.width = SHADOW_RES, .height = SHADOW_RES})
		.Depth(shadow_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 1.0f)
		.Execute(cmd, [&]() {
			for (uint32_t i = 0; i < drawQueue.size(); ++i) {
				const auto& draw = drawQueue[i];
				auto* mesh = std::bit_cast<NativeMesh*>(draw.mesh);

				ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 1};

				Vk::DrawInstanced(
					cmd,
					{.pipeline = shadowPipeline.Get(),
					 .layout = shadowPipelineLayout.Get(),
					 .set = bindlessSets[frame_index],
					 .vbo = mesh->buffer.Handle(),
					 .ibo = draw.indexMesh ? draw.indexMesh->buffer.Handle() : VK_NULL_HANDLE,
					 .vertexCount = mesh->vertexCount,
					 .indexCount = draw.indexCount,
					 .instanceCount = 1},
					pushConstants, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
			}
		});

	(void)Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, shadow_att);
}

void RenderContext::Impl::RenderMainPass(RenderContext& ctx, VkCommandBuffer cmd) {
	bool gpuCullingSuccess = RenderMainPassGpuCulling(ctx, cmd);
	if (gpuCullingSuccess) {
		return;
	}

	[[maybe_unused]] static auto loggedOnce = []() -> bool {
		ZHLN::Log("RENDERING DIAGNOSTIC: Falling back to CPU Traditional Path!");
		return true;
	}();

	Profiler::ScopedGpuProfile<Stages::MainPass, FrameProfiler> timer(cmd, frame_index,
																	  gpuProfiler);

	auto drawCount = static_cast<uint32_t>(drawQueue.size());
	if (drawCount == 0) {
		return;
	}

	// 1. Sort the draw queue (Radix Sort)
	SortDrawQueue();

	// 2. Map and write draw parameters to the GPU instance buffer once [1]
	auto mapRegion = instanceDataBuffers[frame_index].Map();
	auto* mapped = reinterpret_cast<InstanceData*>(mapRegion.data);
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
			.morphOffset = drawCmd.morphOffset,
			.activeMorphCount = drawCmd.activeMorphCount,
			.indexCount = drawCmd.indexCount,
			._pad = 0,
			.morphWeights = {drawCmd.morphWeights[0], drawCmd.morphWeights[1],
							 drawCmd.morphWeights[2], drawCmd.morphWeights[3]},
			.baseColorFactor = {drawCmd.baseColorFactor[0], drawCmd.baseColorFactor[1],
								drawCmd.baseColorFactor[2], drawCmd.baseColorFactor[3]}};
	}

	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> sceneColor_ro = {
		.handle = sceneColor.image.Handle(),
		.view = sceneColor.view.Get(),
		.extent = sceneColor.extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> velocity_ro = {
		.handle = velocityBuffer.image.Handle(),
		.view = velocityBuffer.view.Get(),
		.extent = velocityBuffer.extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

	auto sceneColor_att =
		Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, sceneColor_ro);
	auto velocity_att = Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, velocity_ro);
	auto depth_att = GetDepthAttachmentForMainPass(cmd);

	VkExtent2D extent = presentation.swapchain.Get().extent;
	std::array<VkFormat, 3> colorFormats = {VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
											VK_FORMAT_R16G16B16A16_SFLOAT};

	// 3. Dispatch parallel secondary buffers drawing our lightweight index [1]
	Vk::DynamicPass<2, true>(extent)
		.Color(0, sceneColor_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE)
		.Color(1, velocity_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE)
		.Depth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 1.0f)
		.ClearColor(0, 0.08f, 0.09f, 0.12f, 1.0f)
		.ClearColor(1, 0.0f, 0.0f, 0.0f, 0.0f)
		.Flags(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
		.Execute(cmd, [&]() {
			Vk::ParallelDrawDispatch(
				cmd,
				Vk::SecondaryInheritance{.colorFormats = colorFormats,
										 .depthFormat = VK_FORMAT_D32_SFLOAT},
				extent, drawCount, 256, frame_index,
				std::span<WorkerCmdContext>(workerCmds.data(), workerCmds.size()),
				[&](VkCommandBuffer sec_cmd, uint32_t i) {
					const auto& drawCmd = drawQueue[i];

					if (!drawCmd.material->pipeline.Valid()) {
						return;
					}

					ObjectConstants pushConstants = {.instanceId = i, .isShadowPass = 0};

					Vk::DrawInstanced(sec_cmd,
									  {.pipeline = drawCmd.material->pipeline.Get(),
									   .layout = drawCmd.material->layout.Get(),
									   .set = bindlessSets[frame_index],
									   .vbo = drawCmd.mesh->buffer.Handle(),
									   .ibo = drawCmd.indexMesh ? drawCmd.indexMesh->buffer.Handle()
																: VK_NULL_HANDLE,
									   .vertexCount = drawCmd.mesh->vertexCount,
									   .indexCount = drawCmd.indexCount},
									  pushConstants);
				});
		});

	drawQueue.clear();
}

bool RenderContext::Impl::RenderMainPassGpuCulling(RenderContext& /*ctx*/, VkCommandBuffer cmd) {
	Profiler::ScopedGpuProfile<Stages::MainPass, FrameProfiler> timer(cmd, frame_index,
																	  gpuProfiler);
	auto drawCount = static_cast<uint32_t>(drawQueue.size());
	if (drawCount == 0) {
		return true;
	}

	if (!cullingPass.pipeline.Valid() || !instanceDataBuffers[frame_index].Valid() ||
		!indirectCommandsBuffers[frame_index].Valid()) {
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
		NativeMesh* indexMesh;
		uint32_t indexCount;
		uint32_t start;
		uint32_t count;
	};

	JPH::Array<GroupRange> groups;
	groups.reserve((drawCount + 15) / 16);

	auto mapRegion = instanceDataBuffers[frame_index].Map();
	auto* mapped = reinterpret_cast<InstanceData*>(mapRegion.data);

	NativeMaterial* currentMaterial = nullptr;
	NativeMesh* currentMesh = nullptr;
	NativeMesh* currentIndexMesh = nullptr;
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
			.morphOffset = drawCmd.morphOffset,
			.activeMorphCount = drawCmd.activeMorphCount,
			.indexCount = drawCmd.indexCount,
			._pad = 0,
			.morphWeights = {drawCmd.morphWeights[0], drawCmd.morphWeights[1],
							 drawCmd.morphWeights[2], drawCmd.morphWeights[3]},
			.baseColorFactor = {drawCmd.baseColorFactor[0], drawCmd.baseColorFactor[1],
								drawCmd.baseColorFactor[2], drawCmd.baseColorFactor[3]}};

		if (i == 0 || drawCmd.material != currentMaterial || drawCmd.mesh != currentMesh ||
			drawCmd.indexMesh != currentIndexMesh) {
			groups.push_back({.material = drawCmd.material,
							  .mesh = drawCmd.mesh,
							  .indexMesh = drawCmd.indexMesh,
							  .indexCount = drawCmd.indexCount,
							  .start = i,
							  .count = 1});
			currentMaterial = drawCmd.material;
			currentMesh = drawCmd.mesh;
			currentIndexMesh = drawCmd.indexMesh;
		} else {
			groups.back().count++;
		}
	}
	struct FrustumPlanes {
		JPH::Vec4 planes[6];
		uint32_t drawCount;
	};
	FrustumPlanes planes{};

	const auto& vp = unjittered_view_proj;
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

	// Dispatch the culling pass using our new abstracted API
	cullingPass.Dispatch(cmd, cullingSets[frame_index], (drawCount + 63) / 64, 1, 1, planes);

	// Apply the execution/memory barrier so the GPU knows compute has finished writing
	Vk::CmdBarrierComputeToIndirect(cmd);

	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> sceneColor_ro = {
		.handle = sceneColor.image.Handle(),
		.view = sceneColor.view.Get(),
		.extent = sceneColor.extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> velocity_ro = {
		.handle = velocityBuffer.image.Handle(),
		.view = velocityBuffer.view.Get(),
		.extent = velocityBuffer.extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> normRough_ro = {
		.handle = normalRoughnessBuffer.image.Handle(),
		.view = normalRoughnessBuffer.view.Get(),
		.extent = normalRoughnessBuffer.extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

	auto sceneColor_att =
		Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, sceneColor_ro);
	auto velocity_att = Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, velocity_ro);
	auto normRough_att =
		Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, normRough_ro);

	Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth_att;
	if (!depth_ready) {
		auto depth_u = presentation.depthTarget.State();
		depth_att = Vk::Transition<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, depth_u);
		depth_ready = true;
	} else {
		Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth_current = {
			.handle = presentation.depthTarget.image.Handle(),
			.view = presentation.depthTarget.view.Get(),
			.extent = presentation.depthTarget.extent,
			.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};
		depth_att = Vk::Transition<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, depth_current);
	}

	Vk::DynamicPass<3, true>(presentation.swapchain.Get().extent)
		.Color(0, sceneColor_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE)
		.Color(1, velocity_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE)
		.Color(2, normRough_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE)
		.Depth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, 1.0f)
		.ClearColor(0, 0.08f, 0.09f, 0.12f, 1.0f)
		.ClearColor(1, 0.0f, 0.0f, 0.0f, 0.0f)
		.ClearColor(2, 0.0f, 0.0f, 0.0f, 0.0f)
		.Execute(cmd, [&]() {
			const VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
			for (const auto& group : groups) {
				if (!group.material->pipeline.Valid()) {
					continue;
				}

				Vk::DrawIndirect(
					cmd,
					{.pipeline = group.material->pipeline.Get(),
					 .layout = group.material->layout.Get(),
					 .set = bindlessSets[frame_index],
					 .vbo = group.mesh->buffer.Handle(),
					 .ibo = group.indexMesh ? group.indexMesh->buffer.Handle() : VK_NULL_HANDLE,
					 .argumentBuffer = indirectCommandsBuffers[frame_index].Handle(),
					 .offset = group.start * stride,
					 .drawCount = group.count,
					 .stride = static_cast<uint32_t>(stride)},
					ObjectConstants{
						.instanceId = 4294967295u,
						.isShadowPass =
							0}); // 4294967295u is 0xFFFFFFFF, signaling SV_InstanceID fallback [1]
			}
		});

	drawQueue.clear();
	return true;
}

void RenderContext::Impl::ApplyTAAPass(VkCommandBuffer cmd, VkExtent2D extent) {
	Profiler::ScopedGpuProfile<Stages::TaaPass, FrameProfiler> timer(cmd, frame_index, gpuProfiler);
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> accumCurr_ro = {
		.handle = accumBuffers.Current().image.Handle(),
		.view = accumBuffers.Current().view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> accumNext_ro = {
		.handle = accumBuffers.Next().image.Handle(),
		.view = accumBuffers.Next().view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

	auto accumCurr_ready =
		Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, accumCurr_ro);
	auto accumNext_att =
		Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, accumNext_ro);

	struct TAAPushConstants {
		float feedback;
	};

	Vk::DynamicPass<1, false>(extent)
		.Color(0, accumNext_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		.Execute(cmd, [&]() {
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> sceneColor_ro = {
				.handle = sceneColor.image.Handle(),
				.view = sceneColor.view.Get(),
				.extent = extent,
				.aspect = VK_IMAGE_ASPECT_COLOR_BIT};
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> velocity_ro = {
				.handle = velocityBuffer.image.Handle(),
				.view = velocityBuffer.view.Get(),
				.extent = extent,
				.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

			taaPass.WriteNext(ctx.Device(), sceneColor_ro, accumCurr_ready, velocity_ro,
							  Vk::SamplerWrite{.sampler = defaultSampler.Get()},
							  Vk::BufferWrite{.buffer = frameUniformBuffers[frame_index].Handle()});

			taaPass.Execute(cmd, TAAPushConstants{.feedback = taaState.feedback});
		});

	(void)Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, accumNext_att);
}

void RenderContext::Impl::BlitAndDrawUI(VkCommandBuffer cmd, VkExtent2D extent, uint32_t imageIdx) {
	Profiler::ScopedGpuProfile<Stages::BlitPass, FrameProfiler> timer(cmd, frame_index,
																	  gpuProfiler);
	VkImage swapImg = presentation.swapchain.Get().images[imageIdx];
	VkImageView swapView = presentation.swapchain.Get().views[imageIdx];

	Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> swap_u = {
		.handle = swapImg, .view = swapView, .extent = extent, .aspect = VK_IMAGE_ASPECT_COLOR_BIT};

	auto swap_att = Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, swap_u);

	bool useTAA = taaState.enabled && taaPass.pipeline.Valid();

	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth_ro = {
		.handle = presentation.depthTarget.image.Handle(),
		.view = presentation.depthTarget.view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};

	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> normRough_ro = {
		.handle = normalRoughnessBuffer.image.Handle(),
		.view = normalRoughnessBuffer.view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> blitSource_ro = {
		.handle = useTAA ? accumBuffers.Next().image.Handle() : sceneColor.image.Handle(),
		.view = useTAA ? accumBuffers.Next().view.Get() : sceneColor.view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

	blitPass.WriteNext(ctx.Device(),
					   blitSource_ro,											// 0
					   Vk::SamplerWrite{.sampler = defaultSampler.Get()},		// 1
					   depth_ro,												// 2
					   normRough_ro,											// 3
					   Vk::SamplerWrite{.sampler = pointSampler.Get()},			// 4
					   Vk::ImageWrite{.view = iblPayload.prefilteredView.Get(), // 5
									  .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});

	struct BlitPushConstants {
		JPH::Mat44 invViewProj;
		JPH::Mat44 viewProj;
		alignas(16) float camPos[4];
		int giMode;
		float aoRadius;
		float aoBias;
		float aoPower;
		float giIntensity;
		int giSamples;
		float vignetteIntensity;
		float vignettePower;
		int enableSSR;
	} pc = {.invViewProj = currentUniforms.invViewProj,
			.viewProj = currentUniforms.unjitteredViewProj,
			.camPos = {currentUniforms.camPos[0], currentUniforms.camPos[1],
					   currentUniforms.camPos[2], (float)taaState.frameIndex},
			.giMode = giSettings.mode,
			.aoRadius = giSettings.aoRadius,
			.aoBias = giSettings.aoBias,
			.aoPower = giSettings.aoPower,
			.giIntensity = giSettings.giIntensity,
			.giSamples = giSettings.giSamples,
			.vignetteIntensity = giSettings.vignetteIntensity,
			.vignettePower = giSettings.vignettePower,
			.enableSSR = giSettings.enableSSR};
	if (blitPass.pipeline.Valid()) {
		Vk::DynamicPass<1, false>(extent)
			.Color(0, swap_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
			.Execute(cmd, [&]() {
				blitPass.Execute(cmd, pc);

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
										   .set = bindlessSets[frame_index],
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

	(void)Vk::Transition<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swap_att);
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

	if (!_impl->sceneColor.image.Valid() || !_impl->accumBuffers.Current().image.Valid()) {
		ZHLN_EndCommandBuffer(_impl->current_cmd);
		_impl->current_cmd = VK_NULL_HANDLE;
		return;
	}

	Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> sceneColor_att = {
		.handle = _impl->sceneColor.image.Handle(),
		.view = _impl->sceneColor.view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};
	Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> velocity_att = {
		.handle = _impl->velocityBuffer.image.Handle(),
		.view = _impl->velocityBuffer.view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};
	Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> normRough_att = {
		.handle = _impl->normalRoughnessBuffer.image.Handle(),
		.view = _impl->normalRoughnessBuffer.view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT};
	Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth_att = {
		.handle = _impl->presentation.depthTarget.image.Handle(),
		.view = _impl->presentation.depthTarget.view.Get(),
		.extent = extent,
		.aspect = VK_IMAGE_ASPECT_DEPTH_BIT};

	(void)Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->current_cmd,
																   sceneColor_att);
	(void)Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->current_cmd,
																   velocity_att);
	(void)Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->current_cmd,
																   normRough_att);
	(void)Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->current_cmd, depth_att);

	if (_impl->taaState.enabled && _impl->taaPass.pipeline.Valid()) {
		_impl->ApplyTAAPass(_impl->current_cmd, extent);
	}

	_impl->BlitAndDrawUI(_impl->current_cmd, extent, _impl->current_image_index);
	ZHLN_EndCommandBuffer(_impl->current_cmd);

	_impl->SubmitFrame();
}

namespace Renderer {
void Clear(RenderContext& ctx, const ZHLN::Color4& color, float depth, bool useSecondaries) {
	(void)ctx;
	(void)color;
	(void)depth;
	(void)useSecondaries;
}

void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj,
				 const JPH::Mat44& unjitteredViewProj) {
	auto* impl = ctx.GetImpl();
	impl->current_view_proj = viewProj;
	impl->unjittered_view_proj = unjitteredViewProj;
}

void SetFrameData(RenderContext& ctx, const FrameUniforms& uniforms,
				  const JPH::Mat44& shadowProjView) {
	auto* impl = ctx.GetImpl();
	impl->shadowProjView = shadowProjView;
	impl->currentUniforms = uniforms;

	std::memcpy(impl->currentUniforms.sh, impl->iblPayload.shCoeffs.data(), sizeof(JPH::Vec4) * 9);

	std::memcpy(impl->frameUniformBuffers[impl->frame_index].Map().data, &impl->currentUniforms,
				sizeof(FrameUniforms));
}

void SetGISettings(RenderContext& ctx, const GISettings& settings) {
	auto* impl = ctx.GetImpl();
	impl->giSettings = settings;
}

void SetLights(RenderContext& ctx, const GPULight* lights, uint32_t count) {
	auto* impl = ctx.GetImpl();
	uint32_t safeCount = ZHLN::Min(count, 128u);
	if (safeCount > 0 && lights != nullptr) {
		std::memcpy(impl->lightStorageBuffers[impl->frame_index].Map().data, lights,
					sizeof(GPULight) * safeCount);
	}
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform, const JPH::Mat44& prevTransform, float cullRadius,
		  uint32_t jointOffset, bool isSkinned, uint32_t morphOffset, uint32_t activeMorphCount,
		  const float* morphWeights) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	if (mesh.vertexBuffer == BufferHandle::Invalid) {
		return;
	}

	impl->drawQueue.push_back(
		DrawCommand{.material = std::bit_cast<NativeMaterial*>(material.pipeline),
					.mesh = std::bit_cast<NativeMesh*>(mesh.vertexBuffer),
					.indexMesh = std::bit_cast<NativeMesh*>(mesh.indexBuffer),
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
										material.baseColorFactor[2], material.baseColorFactor[3]},
					.morphOffset = morphOffset,
					.activeMorphCount = activeMorphCount,
					.morphWeights = {(morphWeights != nullptr) ? morphWeights[0] : 0.0f,
									 (morphWeights != nullptr) ? morphWeights[1] : 0.0f,
									 (morphWeights != nullptr) ? morphWeights[2] : 0.0f,
									 (morphWeights != nullptr) ? morphWeights[3] : 0.0f},
					.indexCount = mesh.indexCount});
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
