// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Profiler.hpp"
#include "detail/RadixSort.hpp"

#include <array>
#include <cstring>
#include <threading/TaskSystem.hpp>

namespace ZHLN {

namespace {

inline RenderFrameResult MapFrameResult(ZHLN_FrameResult res) noexcept {
	switch (res) {
		case ZHLN_FrameResult_Ok:
			return RenderFrameResult::Success;
		case ZHLN_FrameResult_Suboptimal:
			return RenderFrameResult::Suboptimal;
		case ZHLN_FrameResult_OutOfDate:
			return RenderFrameResult::OutOfDate;
		case ZHLN_FrameResult_DeviceLost:
			return RenderFrameResult::DeviceLost;
		default:
			return RenderFrameResult::Error;
	}
}

[[nodiscard]] inline std::array<float, 4> UnpackMorphWeights(const float* weights) noexcept {
	if (weights == nullptr) {
		return {0.0f, 0.0f, 0.0f, 0.0f};
	}
	return {weights[0], weights[1], weights[2], weights[3]};
}

} // namespace

// ============================================================================
// RenderContext Infrastructure & Lifecycles
// ============================================================================

void RenderContext::Impl::SortDrawQueue() {
	auto drawCount = static_cast<uint32_t>(drawQueue.size());
	ZHLN::Array<SortItem> items(drawCount);
	ZHLN::Array<SortItem> temp(drawCount);

	for (uint32_t i = 0; i < drawCount; ++i) {
		items[i] = {.key = SortKey::Pack(drawQueue[i].material, drawQueue[i].posMesh),
					.payload = i};
	}

	RadixSort64(items.data(), temp.data(), drawCount);

	ZHLN::Array<DrawCommand> sortedDrawQueue(drawCount);
	for (uint32_t i = 0; i < drawCount; ++i) {
		sortedDrawQueue[i] = drawQueue[items[i].payload];
	}
	drawQueue = std::move(sortedDrawQueue);
}

std::optional<Extent2D> RenderContext::GetFramebufferSize() const {
	Extent2D size = _impl->window.GetSize();
	if (size.width == 0 || size.height == 0) {
		return std::nullopt;
	}
	return size;
}

void RenderContext::Impl::InitialClearTargets(VkCommandBuffer cmd) noexcept {
	auto mainBundle = Vk::TieTargets(sceneColor, velocityBuffer, accumBuffers[0], accumBuffers[1],
									 normalRoughnessBuffer);
	auto ppBundle = Vk::TieTargets(postProcessTarget, ambientTarget, lightingTarget, smaaEdgeTarget,
								   smaaWeightTarget);
	auto bloomBundle = Vk::TieTargets(bloomThresholdTarget, bloomBlurTarget, bloomFinalTarget);

	auto mainAtts = mainBundle.Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);
	auto ppAtts = ppBundle.Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);
	auto bloomAtts = bloomBundle.Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);

	auto sShadowMap_att = Vk::Transition(cmd, shadowMap, Vk::AsDepthAttachment);
	auto sShadowAtlas_att = Vk::Transition(cmd, shadowAtlas, Vk::AsDepthAttachment);
	auto depth_att = Vk::Transition(cmd, presentation.depthTarget, Vk::AsDepthAttachment);

	Vk::DynamicPass(presentation.swapchain.Get().extent)
		.AddColorGroup(mainAtts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					   {.r = 0, .g = 0, .b = 0, .a = 0})
		.AddDepth(depth_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
				  kClearDepthValue)
		.Execute(cmd, []() {});

	Vk::DynamicPass(presentation.swapchain.Get().extent)
		.AddColorGroup(ppAtts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					   kClearColorBlack)
		.Execute(cmd, []() {});

	Vk::DynamicPass(bloomThresholdTarget.extent)
		.AddColorGroup(bloomAtts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					   kClearColorBlack)
		.Execute(cmd, []() {});

	[[maybe_unused]] auto mainRo = std::apply(
		[&](const auto&... atts) {
			return Vk::TransitionBatch<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, atts...);
		},
		mainAtts);

	[[maybe_unused]] auto ppRo = std::apply(
		[&](const auto&... atts) {
			return Vk::TransitionBatch<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, atts...);
		},
		ppAtts);

	[[maybe_unused]] auto bloomRo = std::apply(
		[&](const auto&... atts) {
			return Vk::TransitionBatch<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, atts...);
		},
		bloomAtts);

	[[maybe_unused]] auto sDepth_ro = Vk::Transition(cmd, depth_att, Vk::AsReadOnly);
	[[maybe_unused]] auto sShadowMap_ro = Vk::Transition(cmd, sShadowMap_att, Vk::AsReadOnly);
	[[maybe_unused]] auto sShadowAtlas_ro = Vk::Transition(cmd, sShadowAtlas_att, Vk::AsReadOnly);

	for (int i = 0; i < 2; ++i) {
		taaPass.WriteIndex(ctx.Device(), i, std::get<0>(mainRo),
						   i == 0 ? std::get<3>(mainRo) : std::get<2>(mainRo), std::get<1>(mainRo),
						   defaultSampler.Get(), frameUniformBuffers[i].Handle());
	}
}

void RenderContext::Impl::DispatchSkinningPasses() {
	if (!hasSkinnedThisFrame) {
		return;
	}

	ZHLN_PROFILE_SCOPE("GPU Compute Skinning");
	VkCommandBuffer cmd = current_cmd;
	skinningPass.Bind(cmd);

	for (const auto& drawCmd : drawQueue) {
		if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
			auto* posMesh = drawCmd.posMesh;
			auto* attrMesh = drawCmd.attrMesh;
			auto* skinMesh = drawCmd.skinMesh;
			auto* scratchMesh = meshPool.Resolve(drawCmd.skinnedVertexBuffer);

			if ((posMesh == nullptr) || (attrMesh == nullptr) || (skinMesh == nullptr) ||
				(scratchMesh == nullptr)) {
				continue;
			}

			SkinningConstants pcs = {
				.inPosAddr = posMesh->vboAddress,
				.inAttrAddr = attrMesh->vboAddress,
				.inSkinAddr = skinMesh->vboAddress,
				.outPosAddr = scratchMesh->vboAddress,
				.outAttrAddr =
					scratchMesh->vboAddress + (scratchMesh->vertexCount * sizeof(VertexPosition)),
				.jointsAddr =
					Vk::GetBufferDeviceAddress(ctx.Device(), jointBuffers[frame_index].Handle()),
				.morphDeltasAddr =
					Vk::GetBufferDeviceAddress(ctx.Device(), morphDeltasBuffer.Handle()),
				.vertexCount = posMesh->vertexCount,
				.jointOffset = drawCmd.jointOffset,
				.morphOffset = drawCmd.morphOffset,
				.activeMorphCount = drawCmd.activeMorphCount,
				.morphWeights = {drawCmd.morphWeights[0], drawCmd.morphWeights[1],
								 drawCmd.morphWeights[2], drawCmd.morphWeights[3]}};

			skinningPass.PushConstants(cmd, pcs);
			skinningPass.Dispatch(cmd, (posMesh->vertexCount + 63) / 64, 1, 1);
		}
	}

	Vk::BarrierBuilder()
		.From(Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite)
		.To(cmd, Vk::BarrierStage::Vertex, Vk::BarrierAccess::ShaderRead);
}

RenderResult RenderContext::BeginFrame() noexcept {
	if (_impl->stagingContext) {
		_impl->stagingContext->Wait();
		_impl->stagingContext.reset();
	}

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	auto wait_res =
		ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, _impl->pools[_impl->frame_index]);
	if (wait_res == ZHLN_FrameResult_DeviceLost) {
		return std::unexpected(RenderFrameResult::DeviceLost);
	}

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
		auto fbSize = GetFramebufferSize();
		if (!fbSize.has_value()) {
			return std::unexpected(RenderFrameResult::OutOfDate);
		}

		VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

		if (!_impl->RecreateTargets(ext)) {
			return std::unexpected(RenderFrameResult::Error);
		}

		_impl->needsInitialClear = true;
		_impl->resized = false;
	}

	ZHLN_AcquireDesc acq = {.swapchain = _impl->presentation.swapchain.Get().handle,
							.image_available = s.image_available,
							.timeout_ns = UINT64_MAX};

	auto acq_res = ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index);
	if (acq_res == ZHLN_FrameResult_OutOfDate || acq_res == ZHLN_FrameResult_Suboptimal) {
		_impl->resized = true;
		_impl->current_cmd = VK_NULL_HANDLE;
		return std::unexpected(MapFrameResult(acq_res));
	}
	if (acq_res == ZHLN_FrameResult_DeviceLost) {
		_impl->current_cmd = VK_NULL_HANDLE;
		return std::unexpected(RenderFrameResult::DeviceLost);
	}
	if (acq_res == ZHLN_FrameResult_Error) {
		_impl->current_cmd = VK_NULL_HANDLE;
		return std::unexpected(RenderFrameResult::Error);
	}

	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);

	if (_impl->needsInitialClear) {
		_impl->InitialClearTargets(_impl->current_cmd);
		_impl->needsInitialClear = false;
	}

	return {};
}

ZHLN_FrameResult RenderContext::Impl::SubmitFrame() {
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

	ZHLN_FrameResult res = Vk::SubmitAndPresent(submitDesc);
	if (res != ZHLN_FrameResult_Ok) {
		resized = true;
	}

	auto manager = StaticResourceManager(
		&accumBuffers, &taaPass, &fxaaPass, &smaaEdgePass, &smaaWeightPass, &smaaBlendPass,
		&ambientPass, &lightingPass, &reflectionPass, &blitPass, &bloomThresholdPass,
		&bloomBlurHPass, &bloomBlurVPass, &frameUniformBuffers, &lightStorageBuffers,
		&instanceDataBuffers, &indirectCommandsBuffers, &jointBuffers, &bindlessSets, &tlas,
		&tlasBuffer, &tlasScratchBuffer, &clusterGridBuffers, &lightIndexListBuffers,
		&globalCounterBuffers, &clusterCullingSets);
	manager.FlipAll();

	frame_index = (frame_index + 1) % 2;
	current_cmd = VK_NULL_HANDLE;
	hasSkinnedThisFrame = false;
	return res;
}

// ============================================================================
// InstanceData & TLAS Assembly Operations
// ============================================================================

InstanceData RenderContext::Impl::ResolveInstanceData(const DrawCommand& cmdData) const noexcept {
	auto* posMesh = (cmdData.skinnedVertexBuffer != BufferHandle::Invalid)
						? meshPool.Resolve(cmdData.skinnedVertexBuffer)
						: cmdData.posMesh;

	auto* attrMesh = cmdData.attrMesh;
	auto* skinMesh = cmdData.skinMesh;
	auto* idxMesh = cmdData.indexMesh;

	if (posMesh == nullptr || attrMesh == nullptr) {
		return {};
	}

	uint64_t posAddr = posMesh->vboAddress;
	uint64_t attrAddr = attrMesh->vboAddress;
	uint64_t skinAddr = (skinMesh != nullptr) ? skinMesh->vboAddress : 0;

	if (posMesh == attrMesh) {
		attrAddr = posMesh->vboAddress + (500000 * sizeof(VertexPosition));
	} else if (cmdData.skinnedVertexBuffer != BufferHandle::Invalid) {
		attrAddr = posMesh->vboAddress + (cmdData.posMesh->vertexCount * sizeof(VertexPosition));
	}

	uint32_t isSkinned = (cmdData.skinnedVertexBuffer == BufferHandle::Invalid &&
						  (cmdData.flags & DrawFlags::Skinned) != DrawFlags::None)
							 ? 1u
							 : 0u;

	uint32_t activeMorphCount = cmdData.activeMorphCount;
	if (cmdData.skinnedVertexBuffer != BufferHandle::Invalid) {
		activeMorphCount = 0; // Displacement is evaluated on GPU pre-skinning
	}

	return InstanceData{
		.world = cmdData.transform,
		.prevWorld = cmdData.prevTransform,
		.posAddress = posAddr,
		.attrAddress = attrAddr,
		.skinAddress = skinAddr,
		.iboAddress = (idxMesh != nullptr) ? idxMesh->vboAddress : 0,
		.vertexCount = cmdData.posMesh->vertexCount,
		.indexCount = cmdData.indexCount,
		.texIndices0 = (cmdData.normalIndex << 16) | (cmdData.albedoIndex & 0xFFFF),
		.texIndices1 = (cmdData.emissiveIndex << 16) | (cmdData.pbrIndex & 0xFFFF),
		.cullRadius = cmdData.cullRadius,
		.metallicFactor = cmdData.metallicFactor,
		.roughnessFactor = cmdData.roughnessFactor,
		.alphaCutoff = cmdData.alphaCutoff,
		.flags = (isSkinned << 8) | (cmdData.alphaMode & 0xFF),
		.jointOffset = cmdData.jointOffset,
		.morphOffset = cmdData.morphOffset,
		.activeMorphCount = activeMorphCount,
		.localCenter = cmdData.localCenter,
		._paddingCenter = {},
		.morphWeights = cmdData.morphWeights,
		.baseColorFactor = cmdData.baseColorFactor,
		.emissiveFactor = cmdData.emissiveFactor,
	};
}

void RenderContext::Impl::BuildTLAS(VkCommandBuffer cmd) noexcept {
	if (!rtCtx.Valid() || drawQueue.empty()) {
		return;
	}

	std::vector<VkAccelerationStructureInstanceKHR> tlasInstances;
	tlasInstances.reserve(drawQueue.size());

	for (uint32_t i = 0; i < drawQueue.size(); ++i) {
		const auto& drawCmd = drawQueue[i];
		auto* mesh = drawCmd.posMesh;

		if (mesh == nullptr) {
			continue;
		}

		bool isSkinned = (drawCmd.flags & DrawFlags::Skinned) != DrawFlags::None;
		bool isExcluded = (drawCmd.flags & DrawFlags::ExcludeFromTLAS) != DrawFlags::None;

		if (mesh->blasAddress == 0 || isSkinned || isExcluded) {
			continue;
		}

		VkAccelerationStructureInstanceKHR inst{};
		const auto& t = drawCmd.transform;

		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 4; ++col) {
				inst.transform.matrix[row][col] = t(row, col);
			}
		}

		inst.instanceCustomIndex = i;
		inst.mask = 0xFF;
		inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		inst.accelerationStructureReference = mesh->blasAddress;
		tlasInstances.push_back(inst);
	}

	if (tlasInstances.empty()) {
		return;
	}

	auto& stagingBuf = tlasStagingBuffers[frame_index];
	auto& instanceBuf = tlasInstanceBuffers[frame_index];

	std::memcpy(stagingBuf.Map().data, tlasInstances.data(),
				tlasInstances.size() * sizeof(VkAccelerationStructureInstanceKHR));

	ZHLN_BufferCopyDesc copy = {.src = stagingBuf.Handle(),
								.dst = instanceBuf.Handle(),
								.size = tlasInstances.size() *
										sizeof(VkAccelerationStructureInstanceKHR),
								.src_offset = 0,
								.dst_offset = 0};
	ZHLN_CmdCopyBuffer(cmd, &copy);

	Vk::MemoryBarrier(cmd, {.src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
							.src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
							.dst_stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
							.dst_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
										  VK_ACCESS_2_SHADER_READ_BIT});

	ZHLN_TlasGeometryDesc geom = {
		.instance_data = Vk::GetBufferDeviceAddress(ctx.Device(), instanceBuf.Handle())};

	rtCtx.CmdBuildTlas(
		cmd, geom, tlas[frame_index],
		Vk::GetBufferDeviceAddress(ctx.Device(), tlasScratchBuffer[frame_index].Handle()),
		static_cast<uint32_t>(tlasInstances.size()));

	Vk::MemoryBarrier(cmd, {.src_stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
							.src_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
							.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
							.dst_access = VK_ACCESS_2_SHADER_READ_BIT});
}

RenderResult RenderContext::EndFrame() noexcept {
	ZHLN_PROFILE_SCOPE("Render (CPU Record)");
	if (_impl->current_cmd == VK_NULL_HANDLE) {
		_impl->drawQueue.clear();
		_impl->uiDrawQueue.clear();
		return std::unexpected(RenderFrameResult::Error);
	}

	_impl->DispatchSkinningPasses();

	VkCommandBuffer cmd = _impl->current_cmd;

	if (_impl->drawQueue.size() > kGpuCullingMaxInstances) {
		_impl->drawQueue.resize(kGpuCullingMaxInstances);
	}

	_impl->SortDrawQueue();

	auto drawCount = static_cast<uint32_t>(_impl->drawQueue.size());
	if (drawCount > 0) {
		auto mapped = _impl->instanceDataBuffers[_impl->frame_index].Map();
		auto* dst = static_cast<InstanceData*>(mapped.data);

		for (uint32_t i = 0; i < drawCount; ++i) {
			dst[i] = _impl->ResolveInstanceData(_impl->drawQueue[i]);
		}
	}

	bool isFullBright = (_impl->currentUniforms.fullBright != 0);
	if (!isFullBright) {
		_impl->BuildTLAS(cmd);
	}

	FrameRecorder recorder(cmd, *_impl);

	if (!isFullBright) {
		VkCommandBuffer c = cmd;
		uint32_t fIdx = _impl->frame_index;

		_impl->clusterBoundsPass.Dispatch(c, _impl->clusterCullingSets[fIdx], 1, 1, 24);

		Vk::FillBuffer(c, _impl->globalCounterBuffers[fIdx]);

		Vk::BarrierBuilder()
			.From(Vk::BarrierStage::Transfer | Vk::BarrierStage::Compute,
				  Vk::BarrierAccess::TransferWrite | Vk::BarrierAccess::ShaderWrite)
			.To(c, Vk::BarrierStage::Compute,
				Vk::BarrierAccess::ShaderRead | Vk::BarrierAccess::ShaderWrite);

		_impl->clusterCullingPass.Dispatch(c, _impl->clusterCullingSets[fIdx], 1, 1, 24);

		Vk::BarrierBuilder()
			.From(Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite)
			.To(c, Vk::BarrierStage::Fragment, Vk::BarrierAccess::ShaderRead);

		RunPass(Passes::ShadowPass{}, recorder);
	}

	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
		initialState = {.sceneColor = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
							_impl->sceneColor),
						.velocity = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
							_impl->velocityBuffer),
						.normRough = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
							_impl->normalRoughnessBuffer),
						.depth = Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
							_impl->presentation.depthTarget, VK_IMAGE_ASPECT_DEPTH_BIT)};

	Passes::MainPass{}.Execute(recorder, initialState);

	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> finalColor_ro;
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> bloomFinal_ro;

	if (isFullBright) {
		finalColor_ro = initialState.sceneColor;
		Passes::ForwardPass{}.Execute(recorder, finalColor_ro, initialState.depth);
		bloomFinal_ro =
			Vk::AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(_impl->bloomFinalTarget);
	} else {
		finalColor_ro = Passes::DeferredLightingPass{}.Execute(recorder, initialState);
		Passes::ForwardPass{}.Execute(recorder, finalColor_ro, initialState.depth);
		bloomFinal_ro = Passes::BloomPass{}.Execute(recorder, finalColor_ro);
	}

	SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
		resourcesForAA = {.sceneColor = finalColor_ro,
						  .velocity = initialState.velocity,
						  .normRough = initialState.normRough,
						  .depth = initialState.depth};

	auto aa_ro = Passes::AAPass{}.Execute(recorder, resourcesForAA);

	Passes::BlitPass{}.Execute(recorder, aa_ro.sceneColor, bloomFinal_ro);

	ZHLN_EndCommandBuffer(cmd);

	auto res = _impl->SubmitFrame();
	if (res != ZHLN_FrameResult_Ok) {
		return std::unexpected(MapFrameResult(res));
	}

	_impl->drawQueue.clear();
	_impl->uiDrawQueue.clear();

	return {};
}

void RenderContext::Impl::ProvokeDeviceLostInternal() const {
	if (!hangGpuPass.pipeline.Valid()) {
		ZHLN::Log("ERROR: Cannot provoke device lost because the Hang GPU pipeline is invalid.");
		return;
	}

	VkCommandBuffer cmd = current_cmd;
	bool submitQueue = false;
	Vk::CommandPool tempPool;

	if (cmd == VK_NULL_HANDLE) {
		tempPool = Vk::CommandPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
		if (!tempPool.Allocate(1)) {
			return;
		}
		cmd = tempPool[0];
		ZHLN_BeginCommandBuffer(cmd);
		submitQueue = true;
	}

	hangGpuPass.Bind(cmd);
	hangGpuPass.Dispatch(cmd, 512, 512, 1);

	if (submitQueue) {
		ZHLN_EndCommandBuffer(cmd);
		VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											 .pNext = nullptr,
											 .commandBuffer = cmd,
											 .deviceMask = 0};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.pNext = nullptr,
								.flags = 0,
								.waitSemaphoreInfoCount = 0,
								.pWaitSemaphoreInfos = nullptr,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &subInfo,
								.signalSemaphoreInfoCount = 0,
								.pSignalSemaphoreInfos = nullptr};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(ctx.GraphicsQueue());
	}
}

namespace Renderer {

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const DrawParams& params) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	auto* posMesh = impl->meshPool.Resolve(mesh.posBuffer);
	if (posMesh == nullptr) [[unlikely]] {
		return;
	}

	auto* attrMesh = impl->meshPool.Resolve(mesh.attrBuffer);
	if (attrMesh == nullptr) [[unlikely]] {
		return;
	}

	auto* skinMesh = mesh.skinBuffer != BufferHandle::Invalid
						 ? impl->meshPool.Resolve(mesh.skinBuffer)
						 : nullptr;

	auto* nativeIndexMesh = mesh.indexBuffer != BufferHandle::Invalid
								? impl->meshPool.Resolve(mesh.indexBuffer)
								: nullptr;

	auto* nativeMaterial = impl->materialPool.Resolve(material.pipeline);
	if (nativeMaterial == nullptr) [[unlikely]] {
		return;
	}

	if (params.skinnedVertexBuffer != BufferHandle::Invalid) {
		impl->hasSkinnedThisFrame = true;
	}

	impl->drawQueue.push_back(DrawCommand{
		.material = nativeMaterial,
		.posMesh = posMesh,
		.attrMesh = attrMesh,
		.skinMesh = skinMesh,
		.indexMesh = nativeIndexMesh,
		.transform = params.transform,
		.prevTransform = params.prevTransform,
		.albedoIndex = material.albedoIndex,
		.normalIndex = material.normalIndex,
		.pbrIndex = material.pbrIndex,
		.emissiveIndex = material.emissiveIndex,
		.cullRadius = params.cullRadius,
		.localCenter = params.localCenter,
		.metallicFactor = params.metallic >= 0.0f ? params.metallic : material.metallicFactor,
		.roughnessFactor = params.roughness >= 0.0f ? params.roughness : material.roughnessFactor,
		.alphaCutoff = material.alphaCutoff,
		.alphaMode = material.alphaMode,
		.jointOffset = params.jointOffset,
		.baseColorFactor = {material.baseColorFactor[0], material.baseColorFactor[1],
							material.baseColorFactor[2], material.baseColorFactor[3]},
		.emissiveFactor = {material.emissiveFactor[0], material.emissiveFactor[1],
						   material.emissiveFactor[2], material.emissiveFactor[3]},
		.morphOffset = params.morphOffset,
		.activeMorphCount = params.activeMorphCount,
		.morphWeights = UnpackMorphWeights(params.morphWeights),
		.indexCount = mesh.indexCount,
		.skinnedVertexBuffer = params.skinnedVertexBuffer,
		.flags = params.flags,
	});
}

void DrawUI(RenderContext& ctx, const Mesh& mesh, uint32_t fontIndex, bool useScissor,
			ScissorRect scissorRect) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	auto* posMesh = impl->meshPool.Resolve(mesh.posBuffer);
	auto* attrMesh = impl->meshPool.Resolve(mesh.attrBuffer);
	if (posMesh == nullptr || attrMesh == nullptr) [[unlikely]] {
		return;
	}

	impl->uiDrawQueue.push_back({.posMesh = posMesh,
								 .attrMesh = attrMesh,
								 .fontIndex = fontIndex,
								 .useScissor = useScissor,
								 .scissorRect = scissorRect});
}

} // namespace Renderer
} // namespace ZHLN
